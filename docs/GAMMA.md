# How gamma works in PowerGrade

PowerGrade is a display pipeline in one node: camera log in, graded picture out. Gamma
is the space the grading wheels live in and the last thing that happens to a pixel, so
understanding it explains most of the plugin's output stage. This doc covers what gamma
is, the curves PowerGrade implements, and every place a transfer function acts in the
pixel path.

## 1. Linear light vs. code values

Scene light is linear: twice the photons, twice the value. Displays and video files are
not — they store values through a *transfer function* so the limited bits go where eyes
are most sensitive (the shadows). Human vision is roughly logarithmic; a straight linear
encoding would waste most of its precision on highlights we can barely differentiate and
starve the shadows where we see every step. "Gamma" is the classic form of that
redistribution: store `V = L^(1/γ)`, display `L = V^γ`.

Two families of transfer function matter here, and they are not interchangeable:

- **Scene-referred (OETF — opto-electronic transfer function).** The *camera-side*
  curve: how scene light becomes code values. The BT.709 OETF is a linear toe below
  0.018 followed by a ~0.45 power. It describes encoding, not viewing.
- **Display-referred (EOTF — electro-optical transfer function, "gamma").** The
  *display-side* curve: how code values become emitted light. These are pure power
  functions with no toe — BT.1886 standardizes γ = 2.4 for reference monitors in dim
  surroundings; desktop monitors, laptops, and phones in bright rooms behave close to
  γ = 2.2 (sRGB's effective end-to-end response).

The visible difference between a pure power curve and the scene OETF is concentrated in
the shadows: the OETF's linear toe keeps near-blacks linear, a pure power does not. That
one difference drives the plugin's "grade curve follows the encode" design (§3b).

Camera *log* curves (S-Log3, LogC, Gen 5 Film…) and the HDR inverse-EOTFs (PQ, HLG) are
transfer functions too, but they exist to squeeze 14+ stops of scene range into a
recordable signal, not to match a display — PowerGrade treats them purely as input
decodes (see `docs/CAMERAS.md`).

## 2. The curves PowerGrade implements

All in [src/PowerGradePipeline.h](../src/PowerGradePipeline.h), mirrored exactly in the
Metal/OpenCL/CUDA kernels:

| Helper | Curve | Shape |
|---|---|---|
| `r709_enc/dec` | BT.709 scene OETF | linear toe (`4.5·L` below 0.018), then `1.099·L^0.45 − 0.099` |
| `r709_g_enc/dec(x, g)` | pure display gamma | `x^(1/g)` / `x^g`, no toe; called with 2.2 or 2.4 |
| `di_encode/decode` | DaVinci Intermediate log | log2-based camera-style log |
| `encode()` Cineon branch | Cineon film log | `(685 + 300·log10 x)/1023`, clamped |
| `decode_log()` per camera | log/HDR decodes | input-side only |

## 3. The three places transfer functions act in the pixel path

Everything below is `pg::process()` plus the trim step in the processor/kernels.

**a. Camera decode (step 1).** `decode_log()` linearizes the footage — log curves
inverted to scene-linear (mid-gray 0.18), PQ/HLG via their inverse EOTFs normalized so
reference white lands near 1.0. Fixed per camera, independent of the output choice.

**b. The grade curve for Lift/Gamma/Gain (step 6).** The wheels run neither in linear
nor in log — they run **in the same Rec.709 curve the node will output**, so a wheel
move reads linearly on the timeline's scope, exactly like Resolve's own primary wheels:

```c
const float dg = (enc == 1) ? 2.2f : (enc == 2) ? 2.4f : 0.0f;   // 0 = Scene OETF
float v = (dg > 0.f) ? r709_g_enc(outc[i], dg) : r709_enc(outc[i]);
v = v * gain;                                  // pivots black
v = v + lift * (1.0f - min(v, 1.0f));          // pivots white, superwhites not amplified
v = safe_pow(v, 1.0f / gamma);                 // pivots both
outc[i] = (dg > 0.f) ? r709_g_dec(v, dg) : r709_dec(v);
```

`dg` (display gamma; `0` = Scene OETF) threads the choice through all four
implementations (`pg_lgg(..., dg)` in the kernels). If the encode is a pure power, the
wheels grade in that power; if it's the Scene OETF, they grade in the OETF — grading in
one curve and encoding in another would make Lift's white pivot and the toe behavior
land in the wrong place on the scope.

Note the user-facing **Gamma wheel** is a different animal: a creative power adjustment
*inside* whatever grade curve is active (midtone brightness), not a transfer-function
choice.

**c. The output encode (step 7).** `encode()` (CPU) / `pg_enc()` (kernels) turn the
graded linear pixel into the delivery curve:

| Index | Option | Curve | Primaries |
|---|---|---|---|
| 0 | Rec.709 (Scene) | BT.709 OETF (linear toe) | Rec.709 |
| 1 | **Rec.709 (Gamma 2.2)** — default | pure `x^(1/2.2)` | Rec.709 |
| 2 | Rec.709 (Gamma 2.4) | pure `x^(1/2.4)` | Rec.709 |
| 3 | Cineon Log | film log (feeds film-print LUTs) | Rec.709 |
| 4 | DaVinci Intermediate | DI log | DWG |
| 5 | Linear | none | DWG |

Two downstream behaviors key off this index:

- **Output primaries:** `enc <= 3` converts DWG → Rec.709 linear before the grade curve;
  DI and Linear stay in DaVinci Wide Gamut.
- **Highlight Rolloff gating:** the soft clip is a display-space operation, so it runs
  only when `enc <= 2` (the three Rec.709 encodes) *or* a LUT is active — never on
  Cineon/DI/Linear feeds to downstream nodes.

The LUT paths override the encode at render time (`setupAndProcess()` in
[src/PowerGrade.cpp](../src/PowerGrade.cpp)): Film Look forces Cineon (3), Custom Look
forces Rec.709 Scene (0), because that's the input space those LUTs are built for
(see `docs/LUTS.md`).

## 4. Matching the timeline

Resolve cannot tell the plugin the timeline space — reading it from OFX would require
becoming color-managed, which would let Resolve override the plugin's own CST. So Output
Encode is a manual choice, and the project's **Timeline Color Space must be set to
match it** (Setup/Help group repeats this). If the two disagree, the scopes lie to you:
the node encodes one curve while Resolve interprets the timeline as another.

Choosing between the Rec.709 curves is a *viewing-environment* question, not a
right-vs-wrong one:

- **Gamma 2.2** — matches sRGB-ish desktop/laptop/phone displays in bright
  surroundings, i.e. web and streaming playback (YouTube et al.). The plugin default.
- **Gamma 2.4** — BT.1886: broadcast delivery, calibrated reference monitor, dim
  grading suite.
- **Rec.709 (Scene)** — a deliberately scene-referred timeline; the OETF's linear toe
  keeps near-blacks linear on the scope.

Grade against one gamma and play back on a display honoring another and the picture
shifts: shown on a *lighter* response (2.4-graded material on a 2.2 display) shadows
lift and contrast flattens; on a *darker* response the reverse. Matching encode,
timeline, and the audience's actual display is the whole game.
