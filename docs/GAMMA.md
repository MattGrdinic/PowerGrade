# Gamma in PowerGrade — where it lives, why the defaults, how it shapes the code

PowerGrade is a display pipeline in one node: camera log in, graded picture out. Gamma is
the last thing that happens to a pixel and the space the grading wheels live in, so the
choices here decide whether what you see on the Color page is what your audience sees.
This doc is the full story: what gamma means in this plugin, why **Rec.709 Gamma 2.2 is
the default**, and every place gamma touches the code.

## 1. The 30-second background

Scene light is linear: twice the photons, twice the value. Displays and video files are
not — they store values through a *transfer function* so the limited bits go where eyes
are sensitive (shadows). Two families matter to us:

- **Scene-referred (OETF)** — the camera-side curve. `Rec.709 (Scene)` is the BT.709
  OETF: a linear toe below 0.018 then a ~0.45 power. It describes how a *camera* encodes
  light, not how a display renders it.
- **Display-referred (EOTF, "gamma")** — the display-side curve, a pure power function
  with no toe. **Gamma 2.4** is BT.1886, the broadcast/reference-monitor standard (dim
  grading suite). **Gamma 2.2** is what desktop monitors, phones, and web players
  actually approximate (sRGB's effective end-to-end response).

A pure power curve and the scene OETF differ most in the shadows: the OETF's linear toe
keeps near-blacks linear, the power curve doesn't. That difference is exactly why the
grade curve (§3) has to follow the encode instead of being hardcoded.

## 2. Why Gamma 2.2 is the default (2026-07-16)

Most PowerGrade exports end up on **YouTube and other web/streaming platforms**, watched
on sRGB-ish desktop monitors, laptops, and phones — displays that behave like gamma
≈2.2, in bright surroundings. If you grade against a 2.4 timeline and deliver there, the
viewer's lighter display response lifts your shadows and flattens contrast: the picture
reads washed-out compared to what you approved. Grading against 2.2 means the Color page
and the eventual YouTube playback agree.

Gamma 2.4 stays in the menu, one notch down, for the cases it is genuinely right:
broadcast delivery, a calibrated reference monitor in a dim suite, or matching a project
that was always BT.1886. Nothing about the 2.4 path changed — only the default.

The rest of the defaults it composes with:

| Setting | Default | Why |
|---|---|---|
| Camera | Rec.2100 PQ ("smooth decode") | compressive inverse-EOTF flatters log footage — presets build on it |
| Output Encode | **Rec.709 (Gamma 2.2)** | matches web/YouTube delivery, where most exports land |
| Timeline Color Space (project) | Rec.709 Gamma 2.2 | must match Output Encode — the plugin can't read it from OFX |

Resolve cannot tell the plugin the timeline space (reading it would require becoming
color-managed, which would make Resolve override our CST), so Output Encode is a manual
choice and the project's Timeline Color Space must be set to match it.

## 3. The three places gamma acts in the pixel pipeline

Everything below is `pg::process()` in [src/PowerGradePipeline.h](../src/PowerGradePipeline.h),
mirrored exactly in the Metal/OpenCL/CUDA kernels.

**a. Camera decode (step 1).** Log curves (and the HLG/PQ inverse-EOTFs) take the
footage to scene-linear. These are fixed per camera and unrelated to the output choice.

**b. The grade curve for Lift/Gamma/Gain (step 6).** The wheels do not run in linear or
in log — they run **in the same Rec.709 curve the node will output**, so a wheel move
reads linearly on the timeline's scope, exactly like Resolve's own primary wheels:

```c
const float dg = (enc == 1) ? 2.2f : (enc == 2) ? 2.4f : 0.0f;   // 0 = Scene OETF
float v = (dg > 0.f) ? r709_g_enc(outc[i], dg) : r709_enc(outc[i]);
v = v * gain;                                  // pivots black
v = v + lift * (1.0f - min(v, 1.0f));          // pivots white, superwhites not amplified
v = safe_pow(v, 1.0f / gamma);                 // pivots both
outc[i] = (dg > 0.f) ? r709_g_dec(v, dg) : r709_dec(v);
```

This "grade curve follows the output encode" rule is hard-won: we tried grading in
linear and in DI-log first, and both made the wheels behave wrongly (see the pipeline
notes in the README). If the encode is a pure power gamma, the wheels grade in that
gamma; if it's the Scene OETF, they grade in the OETF. `dg` (display gamma; `0` = Scene
OETF) is the parameter that threads this choice through all four implementations — it
replaced the old boolean `g24` flag when 2.2 was added.

Note the user-facing **Gamma wheel** is a different thing: it's a power adjustment
*inside* whatever grade curve is active, not a transfer-function choice.

**c. The output encode itself (step 7).** `encode()` (CPU) / `pg_enc()` (kernels) turn
the graded linear pixel into the delivery curve:

| Index | Option | Curve | Primaries |
|---|---|---|---|
| 0 | Rec.709 (Scene) | BT.709 OETF (linear toe) | Rec.709 |
| 1 | **Rec.709 (Gamma 2.2)** — default | pure `x^(1/2.2)` | Rec.709 |
| 2 | Rec.709 (Gamma 2.4) | pure `x^(1/2.4)` | Rec.709 |
| 3 | Cineon Log | film log (feeds film-print LUTs) | Rec.709 |
| 4 | DaVinci Intermediate | DI log | DWG |
| 5 | Linear | none | DWG |

Two downstream behaviors key off this index:

- **Output primaries**: `enc <= 3` converts DWG → Rec.709 linear before the grade curve;
  DI and Linear stay in DaVinci Wide Gamut.
- **Highlight Rolloff gating**: the soft clip only makes sense on display-referred
  pixels, so it runs only when `enc <= 2` (the three Rec.709 encodes) *or* a LUT is
  active — never on Cineon/DI/Linear feeds to downstream nodes.

The LUT paths override the encode at render time (`setupAndProcess()` in
[src/PowerGrade.cpp](../src/PowerGrade.cpp)): Film Look forces Cineon (3), Custom Look
forces Rec.709 Scene (0), because that's the space those LUTs expect on their input.

## 4. How a gamma change ripples through the code

Gamma is the sharpest example of the repo's golden rule — the CPU header is the source
of truth and the three kernels mirror it. Adding Gamma 2.2 touched:

1. [src/PowerGradePipeline.h](../src/PowerGradePipeline.h) — `r709_g_enc()/r709_g_dec()`
   (pure-power pair taking the exponent), the `dg` selection, `encode()` branch,
   `enc <= 3` primaries test.
2. [src/MetalKernel.mm](../src/MetalKernel.mm) — same edits: `pg_r709ge()/pg_r709gd()`,
   `pg_lgg(..., float dg)`, `pg_enc()`, `enc<=3`, rolloff gate `enc<=2`.
3. [src/OpenCLKernel.cpp](../src/OpenCLKernel.cpp) — identical, inside the kernel C string.
4. [src/CudaKernel.cu](../src/CudaKernel.cu) — identical.
5. [src/PowerGrade.cpp](../src/PowerGrade.cpp) — the `outEncode` option list (2.2
   inserted at index 1), the default, the Film-Look coupling (`encode = 3`), the CPU
   rolloff gate, and the Setup/Help text.
6. [test/pipeline_test.cpp](../test/pipeline_test.cpp) — round-trip + pure-power checks
   for both gammas; enc loops now run 0–5.

To add another display gamma (say 2.6 for DCI-ish preview): append an option, map its
index to `dg` in the four math files, keep it inside the `enc <= N` Rec.709-primaries
and rolloff ranges, and extend the tests. No new helpers needed — `r709_g_enc/dec`
already take the exponent.

**Saved-grade caveat:** inserting 2.2 at index 1 shifted every encode after it up by one
(the same trade we accepted when the camera list was renumbered, while the plugin is
pre-release). Grades saved before 2026-07-16 that stored Gamma 2.4 (old index 1) now
load as Gamma 2.2 — i.e. they inherit the new default; old Cineon/DI/Linear selections
load one entry off and need re-picking. Film-look grades render correctly regardless,
because LUT Mode re-forces Cineon at render time.

## 5. Practical rules of thumb

- Leave Output Encode at **Gamma 2.2** for anything headed to YouTube/Vimeo/web — and
  set the project's Timeline Color Space to Rec.709 Gamma 2.2 so the viewer matches.
- Switch to **Gamma 2.4** only when delivering to broadcast or grading on a calibrated
  BT.1886 monitor in a dim room — and change the Timeline Color Space with it.
- Use **Rec.709 (Scene)** only for a deliberately scene-referred timeline.
- Don't chase a "correct" single gamma: 2.2 vs 2.4 is a *viewing-environment* choice,
  not a right-vs-wrong one. The default just matches where the audience actually is.
