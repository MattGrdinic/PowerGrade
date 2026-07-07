# PowerGrade — OpenFX plugin for DaVinci Resolve

A Dehancer-style, single-node color grade effect with its own grouped UI, running
on the GPU. Replaces the CST → Balance → Density → Exposure → output chain with one
OpenFX node. Cross-platform: **Metal** (macOS), **OpenCL** (all), **CUDA** (Linux/Win NVIDIA),
plus a CPU fallback.

```
[ PowerGrade OFX node ]  →  [ optional film-look LUT node ]
   camera + all controls      your .cube (set Output Encode = Cineon Log to feed it)
```

## Layout
- `src/PowerGrade.cpp` — OFX plugin: params, grouped UI, render dispatch
- `src/PowerGradePipeline.h` — the color math (single source of truth, used by the CPU path)
- `src/MetalKernel.mm` / `src/OpenCLKernel.cpp` / `src/CudaKernel.cu` — GPU kernels (mirror the pipeline)
- `Makefile` — universal build → `PowerGrade.ofx.bundle`
- `third_party/openfx/` — vendored OpenFX 1.4 SDK

## Build
```
make            # -> PowerGrade.ofx.bundle (universal arm64+x86_64 on macOS)
make clean
```

## Install (needs admin — /Library is root-owned)
```
sudo mkdir -p /Library/OFX/Plugins
sudo cp -fr "PowerGrade.ofx.bundle" /Library/OFX/Plugins/
```
Then relaunch Resolve. On the Color page, open the **Effects** library → **OpenFX** →
**Power Grade → PowerGrade**, and drag it onto a node.

## Controls (top-to-bottom, mirrors the reference node tree)
1. **Input Transform (CST)** — Camera: Blackmagic / Sony SLog3 / ARRI LogC3 / LogC4 /
   Canon Log3 / RED Log3G10 / DJI D-Log / Fuji F-Log2. Decodes to **DaVinci Wide Gamut
   linear** working space (all grading happens there, like the tree).
2. **Balance** — Temperature, Tint. 2-axis white balance in linear (color-wheel analog;
   watch the vectorscope while adjusting).
3. **Density** — HSV **saturation gain** — the "green channel of Gain in HSV" trick.
   −1 = grayscale, +1 = double saturation.
4. **Exposure** — basic **Lift / Gamma / Gain**.
5. **Output** — DWG → Rec.709 g2.4 / Cineon Log / DaVinci Intermediate / Linear.
   Use **Cineon Log** to feed a film-look LUT.
6. **Look / Film LUT** — apply a LUT inside the node:
   - **LUT Mode**: None / Custom Look LUT (file) / Film Look (built-in).
   - **Film Look LUT**: dropdown scanned from Resolve's `LUT/Film Looks` (Kodak 2383,
     Fuji 3513DI, …). Use with Output = Cineon Log.
   - **Look LUT File**: browse to your own `.cube` (use with the Rec.709 path).
   - **LUT Mix**: strength / output level, like Key Output (0 = off, 1 = full).

## Status / next steps
- Blackmagic (DWG/DI) is the trusted reference; other cameras' gamut matrices are
  published/approx and need on-footage validation vs Resolve's own CST.
- Metal + CPU are validated first (Apple Silicon). OpenCL/CUDA are ported but untested here.
- Only 3D `.cube` LUTs are supported (1D shaper LUTs are skipped).
- Roadmap: highlight roll-off / tone-map before Rec.709, film-stock presets, grain, halation.
