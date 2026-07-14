# PowerGrade

A single-node **OpenFX** color grade plugin for DaVinci Resolve. It collapses the
classic scene-linear grading chain — **input CST → balance → density → exposure →
output transform → look / film LUT** — into one node with a clean, grouped interface,
running on the GPU (Metal / CUDA / OpenCL) with a CPU fallback.

```
[ PowerGrade ]  ==  CST → Balance → Density → Exposure → Output → LUT → Trim   (one node)
```

- **macOS** (Apple Silicon + Intel, universal) — Metal + OpenCL
- **Windows / Linux** — CUDA + OpenCL
- Cross-platform 3D `.cube` LUT support, HDR (HLG / PQ) input, and camera log/gamut transforms for Blackmagic, Sony, ARRI, Canon, RED, DJI, Fuji, Panasonic.

---

# Part 1 — For Colorists

## Install (end users)

1. Download the latest release for your OS from **[Releases](../../releases)**.
2. Unzip and run the installer:
   - **macOS**: double-click `install-macos.command` (copies the plugin to `/Library/OFX/Plugins/`; you'll be asked for your password).
   - **Windows**: right-click `install-windows.bat` → **Run as administrator**.
   - Or copy `PowerGrade.ofx.bundle` manually to:
     - macOS: `/Library/OFX/Plugins/`
     - Windows: `C:\Program Files\Common Files\OFX\Plugins\`
3. Restart DaVinci Resolve.

## Project setup (do this once)

PowerGrade does the camera transform itself, so Resolve must **not** be color-managing
your clips. In **Project Settings → Color Management**:

| Setting | Value |
|---|---|
| Color Science | **DaVinci YRGB** (not Color Managed / not ACES) |
| Timeline Color Space | **Rec.709 Gamma 2.4** (matches the plugin's default Output Encode) — or **Rec.709 (Scene)** for a scene-referred timeline; set Output Encode to match |
| Output Color Space | Same as Timeline |

Leave your clips at their **camera raw / log defaults** — don't put a CST or input LUT
before this node. (This guidance is also in the plugin's **Setup / Help** section.)

## Add it

Color page → **Effects** (Library) → **OpenFX → Power Grade → PowerGrade** → drag it
onto a node. The controls appear top-to-bottom in the order they're applied.

## The controls

**0 · Preset** — one-click starting points for a look (they never touch Camera, RAW,
or Output Encode, and every slider stays live to tweak per clip):
- **Cinematic Film (Kodak 2383)** — cools the highlights against warm practicals, lifts
  shadows off video-black, pulls highlights down so they roll off into the Kodak 2383
  print stock, adds mild density. A fast path from "video" to "graded film".
- **Cinematic Smooth (PQ Decode)** — the same recipe, but *deliberately decodes the clip
  as Rec.2100 PQ* (this is the one preset that sets Camera). The PQ curve's compressive
  top end gives a near-perfect built-in highlight rolloff, smooth color, and rich
  texture on log footage. Leaving the preset does not restore Camera — set it back per clip.
- **Desert Day** — for pale, washed-out mid-day scenery (dry-season browns, hazy skies):
  warm pop, deeper teal-leaning skies, richer ground oranges, solid contrast. Uses the
  **built-in PowerGrade Desert Day LUT** shipped inside the plugin — nothing to download.
  Dial **LUT Mix** back to taste.
- **Cinematic Landscape** — the creamy outdoor look: softly lifted shadows, an early
  smooth highlight shoulder, enriched greens, low-chroma toe. Uses the **built-in
  PowerGrade Cinematic Landscape LUT**. Dial **LUT Mix** back to taste.

The built-in LUTs live at `PowerGrade.ofx.bundle/Contents/Resources/LUTs` and appear in
the **Look LUT Group** dropdown as **PowerGrade (built-in)** — you can use them directly
at any mix, on any machine the plugin is installed on.

## Creating built-in looks

The shipped looks are generated, not hand-painted: each one is a small set of tunable
numbers in [luts/generate_luts.py](luts/generate_luts.py). The full authoring process —
design constraints, every parameter, and the step-by-step for adding a new look and
preset — is documented in [docs/CREATING-LUTS.md](docs/CREATING-LUTS.md).
- **None / Reset Look** — returns the look params to neutral.

**1 · Input Transform**
- **Camera** — pick the source format. Decodes the log/gamut into the working space.
  Supports **Blackmagic Gen 5 Film** (the default — Pocket 4K/6K, URSA, Pyxis clips left
  at Blackmagic Design / Gen 5 Film), Blackmagic (DWG/DI), Sony S-Log3, ARRI LogC3/LogC4,
  Canon Log3, RED Log3G10, DJI D-Log, Fuji F-Log2, Panasonic V-Log, and **Rec.2100 HLG / PQ**
  for HDR clips.

**2 · Balance** — white balance, in linear. *Open the Vectorscope while adjusting.*
- **Offset Temp / Tint** — additive; shifts every tone's chroma **evenly**. Best for a
  stubborn cast across the whole image.
- **Gain Temp / Tint** — multiplicative; keeps **highlights neutral**.
- Use whichever suits the shot (or both).

**3 · Density**
- **Density** — color density via an HSV saturation gain (the "green channel of Gain in
  HSV" trick). Deepens saturated colors. −1 = grayscale, +1 = double saturation.

**4 · Exposure (Lift / Gamma / Gain)**
- Classic wheels behavior on the master: **Gain** pivots black, **Lift** pivots white,
  **Gamma** pivots both. Matches Resolve's primary wheels.

**5 · Output**
- **Output Encode** — match your Timeline Color Space. With the setup above, leave it on
  **Rec.709 (Gamma 2.4)** (the default); use **Rec.709 (Scene)** for a scene-referred
  timeline. (Also: Cineon Log, DaVinci Intermediate, Linear.) The Lift/Gamma/Gain wheels
  grade in whichever Rec.709 curve you pick, so they read linearly on that timeline's scope.

**6 · Look / Film LUT** — a LUT applied inside the node. The two paths are mutually
exclusive (they use different transforms):
- **Film Look** → set **LUT Mode = Film Look**, pick from **Film Look LUT** (Resolve's
  built-in print emulations: Kodak 2383, Fuji 3513DI…). Output auto-switches to Cineon.
- **Custom Look** → set **LUT Mode = Custom Look**, choose a **Look LUT Group** then a
  **Look LUT** (any `.cube` from Resolve's LUT folder). Output stays Rec.709.
- **LUT Mix** — strength / output level, like Key Output (0 = off, 1 = full).

**7 · Trim (after LUT)**
- **Exposure / Contrast** — final trims applied *after* the LUT. Film emulations darken
  the image by design; raise **Exposure** here to bring it back.
- **Highlight Rolloff** — per-channel soft clip so lamps and speculars roll off to white
  instead of clipping into a flat "neon" patch. Higher = earlier, stronger shoulder.
  Only engages on display-referred output (Rec.709 encodes or any LUT path) — never on
  Cineon / DI / Linear feeds to downstream nodes.

## Workflows

- **Clean Rec.709 grade** — set Camera, balance on the vectorscope, set Density and
  Exposure, done.
- **Film emulation** — grade first, then LUT Mode → Film Look → pick a stock, then
  **Trim → Exposure** to taste.
- **Custom look** — LUT Mode → Custom Look → pick your `.cube`, dial **LUT Mix**.
- **HDR clip (e.g. DJI drone)** — set Camera = Rec.2100 HLG (or PQ), then trim exposure.

---

# Part 2 — For Developers & Agents

## What it is

A standard **OpenFX image effect** (`OFX::ImageEffect`) built on the OpenFX 1.4 C++
Support library (vendored). One node, float RGBA, GPU render with a CPU fallback. All
color math lives in **one header** used by the CPU path; the three GPU kernels mirror it.

## Layout

```
src/
  PowerGrade.cpp        OFX plugin: params, grouped UI, render dispatch, LUT scan
  PowerGrade.h          factory declaration
  PowerGradePipeline.h  the color pipeline — SINGLE SOURCE OF TRUTH (CPU path)
  CubeLUT.h             minimal .cube 3D-LUT parser (host side)
  MetalKernel.mm        Metal kernel      (mirrors PowerGradePipeline.h)
  OpenCLKernel.cpp      OpenCL kernel     (mirrors PowerGradePipeline.h)
  CudaKernel.cu         CUDA kernel       (mirrors PowerGradePipeline.h)
  Info.plist            bundle plist
Makefile                macOS/Linux build
CMakeLists.txt          cross-platform build (macOS/Windows/Linux)
test/pipeline_test.cpp  CPU unit tests for the color math
third_party/openfx/     vendored OpenFX 1.4 SDK
```

## The color pipeline (order and spaces)

Per pixel, in `pg::process()` (`PowerGradePipeline.h`). The **space each step runs in is
deliberate** — this is where most of the correctness lives:

| # | Step | Space | Why |
|---|------|-------|-----|
| 1 | camera decode | camera log → scene-linear | per-camera `decode_log()` |
| 2 | gamut | camera → XYZ → **DaVinci Wide Gamut linear** | wide working space, like the reference node tree |
| 3 | **Balance** | linear (DWG) | gain = multiply, offset = additive; even vs. highlight-weighted |
| 4 | **Density** | **DI-log** HSV | saturating in log enriches highlights instead of blowing them out |
| 5 | gamut out | DWG → Rec.709 linear (or keep DWG for DI/Linear) | output primaries |
| 6 | **Lift/Gamma/Gain** | **Rec.709 display curve** — Scene OETF *or* pure 2.4, **follows the output encode** | matches Resolve's timeline wheels; blacks stay pinned; lift clamped at white so superwhites aren't amplified |
| 7 | output encode | Rec.709 Scene / Rec.709 Gamma 2.4 / Cineon / DI / linear | `encode()` |
| 8 | **LUT + mix** | output space | trilinear 3D-LUT sample, then lerp by mix (done in the processor / kernels) |
| 9 | **Trim** | output (display) space | post-LUT exposure (stops) + contrast about 0.5 + per-channel highlight roll-off (display-referred only) |

Parameter vector `P[10]` = `{temp, tint, density, lift, gamma, gain, offTemp, offTint,
postExp, postCon}`; `camera` and `outEncode` are passed separately as ints.

## Golden rule: the CPU header is the source of truth

`PowerGradePipeline.h` is authoritative. The Metal/OpenCL/CUDA kernels **must mirror it
exactly**. Any change to the math is a **4-file change** (CPU + 3 kernels). Keep the
helper names and formulas identical so they're easy to diff.

## How to extend

**Add a camera** (e.g. a new log format):
1. `PowerGradePipeline.h` → add a branch to `decode_log()` (log→linear) and, if the
   gamut isn't already covered, a matrix branch in `to_XYZ()`.
2. Mirror both in `MetalKernel.mm`, `OpenCLKernel.cpp`, `CudaKernel.cu` (same `cam`
   index).
3. `PowerGrade.cpp` → `cam->appendOption("…")` in the same order.

**Add an output encode:** add a branch to `encode()` (CPU) + `pg_enc()` (3 kernels), and
an `enc->appendOption("…")`. If it changes the grade space, update the LGG accordingly.

**Add a control:** bump `kParamCount`, define the param in `describeInContext`, fetch it,
push into `params[]`, and read `P[n]` in `pg::process()` + the 3 kernels (mind the Metal
`setBytes` length, the OpenCL arg list/count, and the CUDA `cudaMalloc` size).

**LUTs:** `CubeLUT.h` parses 3D `.cube` files; the host scans Resolve's LUT folder
(`kFilmLutDir`) into a Film list and a grouped Look cascade, loads the selected `.cube`
(cached by path), and passes `(data, size, mix)` to the processor. Sampling is trilinear
in `apply_lut()` / `pg_sampleLUT()`.

## Build from source

```bash
# macOS (universal arm64+x86_64) / Linux
make                 # -> PowerGrade.ofx.bundle
make install         # -> /Library/OFX/Plugins   (needs sudo)

# any platform, via CMake
cmake -S . -B build-cmake
cmake --build build-cmake --config Release
```

**GPU backends:** Metal + CPU are validated on Apple Silicon. OpenCL and CUDA are ported
from the same math but are **best-effort / not yet hardware-validated** — see CI and the
tests below. CUDA is compiled only where a CUDA toolkit is present.

## Tests

`test/pipeline_test.cpp` compiles `PowerGradePipeline.h` on the CPU and asserts pipeline
invariants (neutral pass-through sanity, Lift pins white, Gain pins black, Gamma pins
both, LUT identity, HDR decodes finite). Run locally:

```bash
make test            # or: cmake --build build-cmake --target pipeline_test && ./build-cmake/pipeline_test
```

CI (GitHub Actions) builds the bundle and runs these tests on macOS and Windows for every
push, and attaches release artifacts on tags.

## Cutting a release

Releases are **git-tag driven** — pushing a `v*` tag is the only trigger. There is no
manual upload step.

**When to run it:** after your changes are merged to `main` and CI is green there. A
release should represent a known-good `main`.

**How to run it:**

```bash
git checkout main && git pull          # be on the merged, green main
git tag v0.1.0                         # semantic version, must start with "v"
git push origin v0.1.0                 # this push is what triggers the release
```

**What it does** (`.github/workflows/ci.yml`, the `release` job — it's skipped on normal
pushes and only runs for `refs/tags/v*`):

1. Builds and **tests** on macOS **and** Windows (the same `build` matrix as every push).
2. Packages a zip per OS — each contains `PowerGrade.ofx.bundle` **plus its installer**
   (`install-macos.command` / `install-windows.bat`).
3. Publishes a **GitHub Release** named for the tag, attaches both zips, and
   auto-generates release notes from the merged commits.

So the whole flow is: **merge → green `main` → tag `vX.Y.Z` → push tag → Release appears
under [Releases](../../releases)** with ready-to-install downloads.

**Versioning:** use [SemVer](https://semver.org) — `vMAJOR.MINOR.PATCH`. The plugin's own
internal version is `kPluginVersionMajor` / `kPluginVersionMinor` in `src/PowerGrade.cpp`;
bump it to match when you cut a release so Resolve reports the same number. If a tag was
wrong, delete it (`git push origin :refs/tags/vX.Y.Z`) and re-tag.

## License

Plugin code: BSD-3-Clause. Vendored OpenFX SDK under `third_party/openfx/` retains its own
license.
