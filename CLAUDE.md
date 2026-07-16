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
   output encode**: Scene OETF for the Scene encode, **pure gamma 2.2/2.4 for those
   encodes** (`dg` float, 0 = Scene OETF; `r709_g_enc/dec(x, g)` helpers — replaced the
   old `g24` flag/`r709_24_enc/dec` when 2.2 landed). Either way:
   - **Gain** = multiply, pivots **black**
   - **Lift** = `lift*(1 - min(v,1))`, pivots **white**, clamped so **superwhites aren't amplified**
   - **Gamma** = power, pivots **black & white**
   Matches Resolve's timeline primary wheels. `pg_lgg(...,dg)` helper.
7. output encode: **Rec.709 (Scene)** = scene OETF (linear toe), so Lift's taper reads
   linearly on a Rec.709 (Scene) timeline. **Rec.709 (Gamma 2.2)** = pure 2.2 power for
   web/YouTube delivery — the **param default** since 2026-07-16 (user call: that's where
   most exports land). **Rec.709 (Gamma 2.4)** = pure 2.4 power for broadcast/BT.1886;
   the grade curve in step 6 follows whichever is picked. (`encode`/`pg_enc`)
   Full rationale + code map: `docs/GAMMA.md`.
8. LUT + trilinear sample + mix (done in processor/kernels, after encode)
9. post-LUT **Trim**: exposure (stops) + contrast about 0.5 + **Highlight Rolloff**
   (`pg::softclip`, per-channel display-space soft clip, asymptote 1.0 — saturated
   practicals converge to white instead of clipping "neon"; gated to display-referred
   output only: `enc <= 2 || active LUT` — never distorts Cineon/DI/Linear feeds)

`P[13] = {temp, tint, density, lift, gamma, gain, offTemp, offTint, postExp, postCon, rawExp,
rawTemp, rolloff}` (postExp/postCon/rolloff applied by the caller in the trim step, not
inside `process()`; rawTemp defaults to 6500 = neutral); `camera` + `outEncode` passed
separately as ints.

Cameras (index): 0 Blackmagic Gen 5 Film (Gen 5 Film log + BMD Wide Gamut Gen 4/5, from
the Gen 5 Color Science white paper; the colorimetric match for Pocket/URSA/Pyxis clips
in a YRGB project — DWG/DI is NOT correct for them) · 1 BMD DWG/DI · 2 Sony S-Log3 ·
3 ARRI LogC3 · 4 LogC4 · 5 Canon Log3 · 6 RED Log3G10 · 7 DJI D-Log · 8 Fuji F-Log2 ·
9 Panasonic V-Log · 10 Rec.2100 HLG · 11 Rec.2100 PQ — **the param default** since the
happy-path redesign: NOT a camera match but the user's preferred creative "smooth
decode" for log footage (compressive inverse-EOTF = near-perfect rolloff, smooth color;
"best results in the most situations"). (Indices were renumbered when Gen 5 moved to
slot 0 — pre-renumber saved grades will show the wrong camera.) Encodes: 0 Rec.709 (Scene) ·
1 Rec.709 (Gamma 2.2) — **the param default** (web/YouTube delivery, 2026-07-16) · 2 Rec.709
(Gamma 2.4) · 3 Cineon Log · 4 DaVinci Intermediate · 5 Linear. (2.2 was INSERTED at slot 1,
shifting 2.4/Cineon/DI/Linear up one — grades saved before 2026-07-16 load one encode off;
old default-2.4 grades become 2.2, film-look grades still render right because lutMode
re-forces Cineon at render.) **709 primaries for enc ≤ 3** (Scene/2.2/2.4/Cineon); DI &
Linear keep DWG primaries. Film Look LUT auto-sets enc=3 (Cineon); Custom Look sets enc=0.
Gamma design doc (why 2.2, all code touch-points): `docs/GAMMA.md`.

## Presets (param layer only — no pipeline/kernel involvement)
`preset` choice param (group "0 Preset"): 0 None/Reset · 1 Cinematic Film Emulation
(Kodak 2383 D60) · 2 Cinematic Film Emulation (Fujifilm 3513DI D60) · 3 Custom LUT -
Cinematic Landscape · 4 Custom LUT - Teal Orange. **Happy-path redesign (2026-07-14,
user's design):** EVERY preset sets Camera → 11 Rec.2100 PQ (same as the new param
default) so there's no more "some presets change Camera, some don't" confusion; preset
names call out which LUT path they drive (Film Emulation = Resolve print stocks, Cineon
path; Custom LUT = our built-in looks, Rec.709 path). Film Emulation presets share the
user-validated Cinematic Film recipe (see below); Fuji falls back to Kodak when the
stock is missing (`filmLutIndex()` by name fragment, -1 when absent). Custom LUT presets
are neutral except Offset Temp -0.14 — the user's measured "happy medium" cool offset
for the PQ path. The PQ smooth-decode trick was discovered when the camera renumber made
an old node decode Gen 5 as PQ. Presets set Rolloff 0 (PQ is already the shoulder).
None/Reset does NOT restore Camera. The former Desert Day / Cinematic Smooth presets are
gone (Smooth is now literally preset 1+default camera; Desert Day lives on as a built-in
LUT only).

**Built-in LUTs** (six ship; Cinematic Landscape + Teal Orange are preset-backed):
generated by
`luts/generate_luts.py` (pure Python, all tunables in the LOOKS dict — edit, re-run,
`make`, reinstall). The .cube files are checked in AND copied into
`Contents/Resources/LUTs` by both Makefile (`bundle-luts`, globs) and CMake bundle
assembly (**explicit file list — new LUTs must be added there by name**).
`bundleLutDir()` resolves them from the plugin binary's own path (dladdr /
GetModuleFileName) and `scanLuts()` surfaces them as the FIRST Look group,
"PowerGrade (built-in)" — zero external installs, works on any render machine. The old
Sedona-LUT dependency is gone (was a third-party download — too much to ask of users).
Desert Day + Cinematic Landscape were authored numerically and **user-validated on
footage first try** (2026-07-14). The other four (Golden Hour · Teal Orange (uses the generator's
`split` luminance split-tone) · Silver Bleach · Midnight Blue) are deliberately spread
across the look-space so a default lands close — **NOT yet user-validated on footage**
except Teal Orange-as-preset pending user check. Full authoring process + parameter
reference: `docs/CREATING-LUTS.md` (LOOKS entry → regenerate → CMake copy list →
optional preset via `findLookLut()`; user plans to add more looks over time).

Preset mechanics: applied in `PowerGrade::applyPreset()` from `changedParam`, **guarded
on `eChangeUserEdit`** so project loads don't re-stamp the preset over user tweaks.
Presets set Camera (→ PQ, see above) + look params (balance temp/tint + offsets, density,
LGG, lutMode/filmLut/lookGroup/lookLut/lutMix, postExp/postCon, rolloff) — never RAW or
Output Encode. Balance IS part of presets (user decision: cool highlights are part of the
cinematic look, not just WB) and None/Reset clears it. The Film Emulation recipe is
user-validated (lift 0.11, gain 0.80, print LUT, +0.55 post-exp; on the old Gen 5 decode
it needed Rolloff 0.5 to stop practicals clipping neon — under PQ decode the shoulder is
built in, so presets set rolloff 0). Values are starting points — expect on-footage
tuning requests.

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
**ALWAYS check `git branch --show-current` before committing** — the user merges PRs
mid-session, so the local checkout can silently be sitting on `main` (this bit us once:
d8ef1d8 went straight to main; user OK'd it that time, pre-release, but never again).
`main` is protected-in-practice; don't commit to it directly. Commits end with a
`Co-Authored-By:` trailer naming the Claude model that did the work. Merged so far: #1 look-first-and-hdr
(all the color fixes), #2 docs-and-distribution, #3 docs-release-process.

## Validation status / gotchas
- **Validated in Resolve:** Metal + CPU on the user's M3 Max, Rec.709 (Scene) / DaVinci YRGB project.
- **Not HW-validated:** OpenCL correctness (CI compiles/tests only), CUDA (not built in CI).
- **Camera matrices** other than Blackmagic are published/approx — flagged for on-footage validation.
- **Can't auto-read the timeline colorspace** from OFX without becoming color-managed (which
  would make Resolve override our CST). So Output Encode is manual; default Rec.709 (Gamma 2.2).
- HDR (HLG/PQ) is a **normalize, not a tone-map** — highlights can clip; a real shoulder is future work.
- Required project setup (also in the plugin's Setup/Help group): DaVinci YRGB, Timeline
  set to match Output Encode (default Rec.709 Gamma 2.2), clips left at camera log, no
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
verify on Pyxis footage vs a CST node. Superseded 2026-07-16: default encode is now
Rec.709 Gamma 2.2 for web/YouTube delivery, branch `feature/gamma22-default`, encode
indices renumbered — see `docs/GAMMA.md`.)
