# CLAUDE.md — PowerGrade working notes

Operational memory for resuming work. User-facing usage + architecture live in
`README.md`; this file is the "how we work on it and why it's built this way" layer.

## What it is
A **compiled OpenFX plugin** (`.ofx.bundle`) for DaVinci Resolve — one node that does
camera CST → balance → density → exposure → output encode → look/film LUT → trim, on the
GPU (Metal/OpenCL/CUDA) with a CPU fallback. Repo: `github.com/MattGrdinic/PowerGrade`.

**History / dead ends (don't retry):** we started as a **DCTL**, then a scripting panel —
both abandoned. DCTL can't do CST/`.cube` LUTs/multi-node; the Resolve scripting API
can't set node params. OFX is the only path that does everything. Not going back.

## The golden rule
`src/PowerGradePipeline.h` (namespace `pg`, CPU) is the **single source of truth** for all
color math. The three GPU kernels **mirror it exactly**:
- `src/MetalKernel.mm`  (Apple — the one path validated in Resolve)
- `src/OpenCLKernel.cpp` (kernel is a C string; CI-green on Windows, not correctness-tested on HW)
- `src/CudaKernel.cu`   (ported, only built with `-DBUILD_CUDA=ON`)

**Any math change is a 4-file edit.** Keep helper names/formulas identical so they diff
cleanly. Param-count changes also need: Metal `setBytes` length, OpenCL `clSetKernelArg`
loop count + kernel signature, CUDA `cudaMalloc`/`cudaMemcpy` size.

## Pipeline order + SPACES (the crux — took many iterations, do NOT regress)
Per pixel, in `pg::process()`:
0. **RAW** (Camera-RAW-tab analogs, so that tab can be left alone):
   - **RAW Exposure** = linear gain in stops on scene light, right after `decode_log`, before
     the CST. This *is* what RAW exposure does (sensor-linear multiply) → near-exact match.
   - **RAW Temp** = Kelvin white balance via a **Bradford chromatic adaptation** in XYZ (right
     after `to_XYZ`, closest to sensor). Blackbody(T) source white → D65; raise T = warmer.
     Identity at 6500 K. NOT byte-exact to the RAW tab (no sensor metadata reaches OFX), but a
     physically-real WB. `white_balance()` / `cct_to_xy()` (Kim et al. locus) helpers.
1. camera log → scene-linear (`decode_log`)
2. camera gamut → XYZ → **DaVinci Wide Gamut linear** (working space)
3. **Balance** in linear: Gain (multiplicative, pivots highlights) + Offset (additive, even)
4. **Density** = HSV saturation gain in **DI-log** — NOT linear. Linear blows out saturated
   reds; log enriches them. This was a real bug we fixed.
5. output primaries: DWG → Rec.709 linear (or keep DWG for DI/Linear encodes)
6. **Lift/Gamma/Gain in the Rec.709 display curve** (linear toe). This is the big one —
   we tried linear and DI-log first and both were wrong. The grade curve **follows the
   output encode**: Scene OETF for the Scene encode, **pure gamma 2.4 for the Gamma 2.4
   encode** (`g24` flag; `r709_24_enc/dec`). Either way:
   - **Gain** = multiply, pivots **black**
   - **Lift** = `lift*(1 - min(v,1))`, pivots **white**, clamped so **superwhites aren't amplified**
   - **Gamma** = power, pivots **black & white**
   Matches Resolve's timeline primary wheels. `pg_lgg(...,g24)` helper.
7. output encode: **Rec.709 (Scene)** = scene OETF (linear toe), so Lift's taper reads
   linearly on a Rec.709 (Scene) timeline. **Rec.709 (Gamma 2.4)** = pure 2.4 power for a
   display-referred/broadcast timeline — this is the **param default** since the Gen 5 work;
   the grade curve in step 6 follows whichever is picked. (`encode`/`pg_enc`)
8. LUT + trilinear sample + mix (done in processor/kernels, after encode)
9. post-LUT **Trim**: exposure (stops) + contrast about 0.5 + **Highlight Rolloff**
   (`pg::softclip`, per-channel display-space soft clip, asymptote 1.0 — saturated
   practicals converge to white instead of clipping "neon"; gated to display-referred
   output only: `enc <= 1 || active LUT` — never distorts Cineon/DI/Linear feeds)

`P[13] = {temp, tint, density, lift, gamma, gain, offTemp, offTint, postExp, postCon, rawExp,
rawTemp, rolloff}` (postExp/postCon/rolloff applied by the caller in the trim step, not
inside `process()`; rawTemp defaults to 6500 = neutral); `camera` + `outEncode` passed
separately as ints.

Cameras (index): 0 Blackmagic Gen 5 Film — **the param default** (Gen 5 Film log + BMD
Wide Gamut Gen 4/5, from the Gen 5 Color Science white paper; this is what Pocket/URSA/
Pyxis clips are in a YRGB project — DWG/DI is NOT correct for them) · 1 BMD DWG/DI ·
2 Sony S-Log3 · 3 ARRI LogC3 · 4 LogC4 · 5 Canon Log3 · 6 RED Log3G10 · 7 DJI D-Log ·
8 Fuji F-Log2 · 9 Panasonic V-Log · 10 Rec.2100 HLG · 11 Rec.2100 PQ. (Indices were
renumbered when Gen 5 moved to slot 0 — pre-renumber saved grades will show the wrong
camera.) Encodes: 0 Rec.709 (Scene) · 1 Rec.709 (Gamma 2.4) — **the param default** ·
2 Cineon Log · 3 DaVinci Intermediate · 4 Linear. **709 primaries for enc ≤ 2** (Scene/2.4/Cineon); DI &
Linear keep DWG primaries. Film Look LUT auto-sets enc=2 (Cineon); Custom Look sets enc=0.

## Presets (param layer only — no pipeline/kernel involvement)
`preset` choice param (group "0 Preset"): 0 None/Reset · 1 Cinematic Film (Kodak 2383) ·
2 Cinematic Smooth (PQ Decode) · 3 Vivid Landscape · 4 Vivid Landscape Smooth (PQ Decode).
The two Smooth variants are the ONLY presets that set Camera (→ 11 Rec.2100 PQ, on
purpose): PQ-decoding log footage gives a compressive "wrong" transform the user loves
(near-perfect rolloff, smooth color) — discovered when the camera renumber made an old
node decode Gen 5 as PQ. Cinematic Smooth sets our Rolloff to 0 (PQ is already the
shoulder). None/Reset does NOT restore Camera. Applied in `PowerGrade::applyPreset()` from `changedParam`, **guarded
on `eChangeUserEdit`** so project loads don't re-stamp the preset over user tweaks.
Presets only set look params (balance temp/tint + offsets, density, LGG, lutMode/filmLut/
lookGroup/lookLut/lutMix, postExp/postCon) — never Camera/RAW/Encode. Balance IS part of
presets (user decision: cool highlights are part of the cinematic look, not just WB) and
None/Reset clears it. `kodak2383Index()` finds the film LUT
(shared with the filmLut describe-time default). Vivid Landscape targets IWLTBAP's free
"Sedona" LUT (user-installed, found by name via `findLookLut()` — prefer the "- LOG"
variant, mix 0.54, user-validated); LUT-free fallback when absent (density 0.45 /
contrast 1.18 — NOT validated on footage). Cinematic Film values are user-validated
(lift 0.11, gain 0.80, 2383, +0.55 post-exp, rolloff 0.5 — the rolloff exists because the
user's original reference was accidentally PQ-decoded Gen 5 footage, whose compressive top
end read "bright but not neon"; correct Gen 5 decode + the exposure push clipped practicals
until the rolloff was added). Don't bundle the Sedona .cube in the repo
(license). Values are starting points — expect on-footage tuning requests.

## Build / test / install (macOS, the dev machine)
```bash
make                 # -> PowerGrade.ofx.bundle (universal arm64+x86_64)
make test            # CPU unit tests (test/pipeline_test.cpp) — must stay green
cmake -S . -B build-cmake && cmake --build build-cmake   # cross-platform path
# install for testing in Resolve (needs sudo; Resolve does NOT follow symlinks — copy real files):
sudo cp -fr PowerGrade.ofx.bundle /Library/OFX/Plugins/
```
After installing, the user restarts Resolve and checks Color page → Effects → OpenFX →
Power Grade. Only the user can visually verify in Resolve; we can't from here.

## Deploy / release (tag-driven)
CI = `.github/workflows/ci.yml`. Every push builds+tests macOS + Windows. Pushing a
**`v*` tag** additionally runs the `release` job → packages per-OS zip (bundle + installer
from `install/`) → publishes a GitHub Release. First release not cut yet; plugin internal
version is `kPluginVersionMajor/Minor` in `src/PowerGrade.cpp` (still 1.0 — bump to match tags).

## Git workflow
Branch per change → push → user opens PR and merges on GitHub (they do the merge, not us).
`main` is protected-in-practice; don't commit to it directly. Commits end with the
`Co-Authored-By: Claude Opus 4.8 (1M context)` trailer. Merged so far: #1 look-first-and-hdr
(all the color fixes), #2 docs-and-distribution, #3 docs-release-process.

## Validation status / gotchas
- **Validated in Resolve:** Metal + CPU on the user's M3 Max, Rec.709 (Scene) / DaVinci YRGB project.
- **Not HW-validated:** OpenCL correctness (CI compiles/tests only), CUDA (not built in CI).
- **Camera matrices** other than Blackmagic are published/approx — flagged for on-footage validation.
- **Can't auto-read the timeline colorspace** from OFX without becoming color-managed (which
  would make Resolve override our CST). So Output Encode is manual; default Rec.709 (Gamma 2.4).
- HDR (HLG/PQ) is a **normalize, not a tone-map** — highlights can clip; a real shoulder is future work.
- Required project setup (also in the plugin's Setup/Help group): DaVinci YRGB, Timeline
  set to match Output Encode (default Rec.709 Gamma 2.4), clips left at camera log, no
  CST/LUT before this node.
- **Don't expect a pixel match against Resolve's "Gen 5 Film to Video" LUT** — "to Video"
  bakes in Blackmagic's contrast/tone curve, not a plain colorimetric conversion. The right
  neutral reference is a CST node (Gen 5 Film → Rec.709 / Gamma 2.4, tone mapping off).

## Likely next tasks
**Rolloff smoothness on Gen 5 (user's active thread):** the Highlight Rolloff softclip is
not yet as smooth as the "Blackmagic Gen 5 Film to Video" LUT, which is the stated target
for the default Gen 5 path (Cinematic Film preset). Candidates: tune softclip knee/curve,
or a scene-linear shoulder before encode instead of (or blended with) the display-space clip.

Cut `v0.1.0`; validate OpenCL/CUDA on real HW; per-camera gamut validation; HDR tone-map
(highlight roll-off). (Done: Rec.709 Gamma 2.4 output with grade-space-following LGG,
branch `feature/rec709-gamma24` — validated in Resolve. In progress: camera 0 Blackmagic
Gen 5 Film — now the default camera, list reordered so both Blackmagic entries lead —
+ default encode → Gamma 2.4, branch `feature/gen5-camera-g24-default` — needs visual
verify on Pyxis footage vs a CST node.)
