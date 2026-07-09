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
6. **Lift/Gamma/Gain in Rec.709 scene-OETF space** (linear toe). This is the big one —
   we tried linear, DI-log, and pure-2.4 first and all were wrong. In scene space:
   - **Gain** = multiply, pivots **black**
   - **Lift** = `lift*(1 - min(v,1))`, pivots **white**, clamped so **superwhites aren't amplified**
   - **Gamma** = power, pivots **black & white**
   Matches Resolve's timeline primary wheels. `pg_lgg()` helper.
7. output encode: **Rec.709 output = scene OETF, NOT pure gamma 2.4**. Matches the user's
   Rec.709 (Scene) timeline so Lift's taper reads linearly on the scope. (`encode`/`pg_enc`)
8. LUT + trilinear sample + mix (done in processor/kernels, after encode)
9. post-LUT **Trim**: exposure (stops) + contrast about 0.5

`P[12] = {temp, tint, density, lift, gamma, gain, offTemp, offTint, postExp, postCon, rawExp,
rawTemp}` (postExp/postCon applied by the caller in the trim step, not inside `process()`;
rawTemp defaults to 6500 = neutral); `camera` + `outEncode` passed separately as ints.

Cameras (index): 0 BMD DWG/DI · 1 Sony S-Log3 · 2 ARRI LogC3 · 3 LogC4 · 4 Canon Log3 ·
5 RED Log3G10 · 6 DJI D-Log · 7 Fuji F-Log2 · 8 Panasonic V-Log · 9 Rec.2100 HLG ·
10 Rec.2100 PQ. Encodes: 0 Rec.709 (Scene) · 1 Cineon Log · 2 DaVinci Intermediate · 3 Linear.

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
  would make Resolve override our CST). So Output Encode is manual; default Rec.709 (Scene).
- HDR (HLG/PQ) is a **normalize, not a tone-map** — highlights can clip; a real shoulder is future work.
- Required project setup (also in the plugin's Setup/Help group): DaVinci YRGB, Timeline
  Rec.709 (Scene), clips left at camera log, no CST/LUT before this node.

## Likely next tasks
Cut `v0.1.0`; validate OpenCL/CUDA on real HW; per-camera gamut validation; HDR tone-map
(highlight roll-off); possibly a 2.4/Video output option with grade-space following.
