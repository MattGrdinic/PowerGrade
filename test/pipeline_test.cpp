// PowerGrade — CPU unit tests for the color pipeline (PowerGradePipeline.h).
// Builds with any C++17 compiler; no OFX/GPU needed. Returns non-zero on failure.
// SPDX-License-Identifier: BSD-3-Clause
#include "../src/PowerGradePipeline.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const std::string& name) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) g_fail++;
}
static bool close(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool finite3(float r, float g, float b) { return std::isfinite(r) && std::isfinite(g) && std::isfinite(b); }

// neutral parameter vector: temp,tint,density,lift,gamma,gain,offTemp,offTint,postExp,postCon,rawExp,rawTemp
static void neutral(float P[12]) { for (int i=0;i<12;i++) P[i]=0.f; P[4]=1.f; P[5]=1.f; P[9]=1.f; P[11]=6500.f; }

int main() {
    printf("PowerGrade pipeline tests\n");

    // 1. Transfer-function round trips (must be exact inverses)
    {
        bool ok = true;
        for (float v = 0.f; v <= 1.5f; v += 0.05f) {
            ok &= close(pg::r709_dec(pg::r709_enc(v)), v, 2e-3f);
            ok &= close(pg::r709_24_dec(pg::r709_24_enc(v)), v, 2e-3f);
            ok &= close(pg::di_decode(pg::di_encode(v)),  v, 2e-3f);
        }
        check(ok, "r709 scene / r709 g2.4 / DI encode/decode round-trip");
    }

    // 1b. Rec.709 Gamma 2.4 (encode index 1) is a pure power curve, no linear toe
    {
        bool ok = close(pg::encode(1, 0.5f), std::pow(0.5f, 1.0f/2.4f), 1e-4f)
               && close(pg::encode(1, 1.0f), 1.0f, 1e-4f)
               && close(pg::encode(1, 0.0f), 0.0f, 1e-4f);
        check(ok, "Rec.709 Gamma 2.4 encode is pure power");
    }

    // 2. HDR decodes finite and monotonic over the signal range
    {
        bool ok = true;
        for (int cam = 9; cam <= 10; ++cam) {           // 9=HLG, 10=PQ
            float prev = -1e9f;
            for (float x = 0.f; x <= 1.0f; x += 0.05f) {
                float y = pg::decode_log(cam, x);
                ok &= std::isfinite(y);
                ok &= (y >= prev - 1e-4f);               // non-decreasing
                prev = y;
            }
        }
        check(ok, "HLG/PQ decode finite and monotonic");
    }

    // 3. Identity 3D LUT leaves pixels unchanged
    {
        const int N = 9;
        std::vector<float> lut((size_t)N*N*N*3);
        for (int b=0;b<N;b++) for (int g=0;g<N;g++) for (int r=0;r<N;r++) {
            size_t i = (((size_t)b*N + g)*N + r)*3;
            lut[i+0] = (float)r/(N-1); lut[i+1] = (float)g/(N-1); lut[i+2] = (float)b/(N-1);
        }
        bool ok = true;
        for (float t = 0.f; t <= 1.f; t += 0.1f) {
            float r=t, g=t*0.5f, b=1.f-t;
            pg::apply_lut(lut.data(), N, 1.0f, r, g, b);
            ok &= close(r, t, 3e-3f) && close(g, t*0.5f, 3e-3f) && close(b, 1.f-t, 3e-3f);
        }
        check(ok, "identity 3D LUT is a pass-through");
    }

    // 4. Neutral trim is identity; exposure/contrast move predictably
    {
        float r=0.4f,g=0.5f,b=0.6f;
        pg::apply_trim(0.f, 1.f, r, g, b);
        check(close(r,0.4f)&&close(g,0.5f)&&close(b,0.6f), "neutral trim is identity");
    }

    // 5. Full pipeline is finite for every camera x encode x sample input
    {
        float P[12]; neutral(P);
        bool ok = true;
        for (int cam = 0; cam <= 10; ++cam)
          for (int enc = 0; enc <= 4; ++enc)
            for (float x = 0.02f; x <= 0.98f; x += 0.12f) {
                float or_,og,ob; pg::process(cam, enc, P, x, x*0.9f, x*1.1f, or_, og, ob);
                ok &= finite3(or_,og,ob);
            }
        check(ok, "process() finite for all cameras/encodes/inputs");
    }

    // 6. Gain pivots black: a black input stays black under gain
    {
        float P[12]; neutral(P); P[5] = 2.0f;            // gain = 2
        float r,g,b; pg::process(0, 0, P, 0.f, 0.f, 0.f, r, g, b);
        check(close(r,0.f,2e-3f)&&close(g,0.f,2e-3f)&&close(b,0.f,2e-3f), "gain pins black");
    }

    // 7. Lift pivots white: diffuse white (BMD/DI code ~0.5139 -> linear 1.0) unchanged by lift
    {
        const float whiteCode = pg::di_encode(1.0f);     // camera code that decodes to linear 1.0
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[3] = -0.25f;        // lift down
        float a0,b0,c0, a1,b1,c1;
        pg::process(0, 0, P0, whiteCode, whiteCode, whiteCode, a0, b0, c0);
        pg::process(0, 0, P1, whiteCode, whiteCode, whiteCode, a1, b1, c1);
        check(close(a0,a1,5e-3f)&&close(b0,b1,5e-3f)&&close(c0,c1,5e-3f), "lift pins white (diffuse white unchanged)");
    }

    // 8. Lift does not amplify superwhites (BMD/DI code 1.0 -> linear ~100)
    {
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[3] = -0.25f;
        float a0,b0,c0, a1,b1,c1;
        pg::process(0, 4, P0, 1.0f, 1.0f, 1.0f, a0, b0, c0);   // enc=4 linear so we compare raw
        pg::process(0, 4, P1, 1.0f, 1.0f, 1.0f, a1, b1, c1);
        check(close(a0,a1,1e-2f), "lift leaves superwhites untouched");
    }

    // 9. RAW exposure: +1 stop doubles scene-linear (linear output path)
    {
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[10] = 1.0f;    // +1 stop
        float a0,b0,c0, a1,b1,c1;
        pg::process(0, 4, P0, 0.5f, 0.5f, 0.5f, a0, b0, c0);   // enc=4 = linear output
        pg::process(0, 4, P1, 0.5f, 0.5f, 0.5f, a1, b1, c1);
        check(close(a1,2.0f*a0,2e-2f)&&close(b1,2.0f*b0,2e-2f)&&close(c1,2.0f*c0,2e-2f),
              "RAW exposure +1 stop doubles linear output");
    }

    // 10. RAW temperature: neutral at 6500; warmer raises R / lowers B, cooler the reverse
    {
        float P6[12]; neutral(P6);                    // rawTemp = 6500 (neutral)
        float Pw[12]; neutral(Pw); Pw[11] = 9000.f;   // warmer
        float Pc[12]; neutral(Pc); Pc[11] = 4000.f;   // cooler
        float r6,g6,b6, rw,gw,bw, rc,gc,bc;
        pg::process(0, 0, P6, 0.5f, 0.5f, 0.5f, r6, g6, b6);
        pg::process(0, 0, Pw, 0.5f, 0.5f, 0.5f, rw, gw, bw);
        pg::process(0, 0, Pc, 0.5f, 0.5f, 0.5f, rc, gc, bc);
        check(finite3(r6,g6,b6) && rw>r6 && r6>rc && bw<b6 && b6<bc,
              "RAW temp: warmer raises R / lowers B, 6500 sits between");
    }

    printf("%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail==1?"":"s");
    return g_fail ? 1 : 0;
}
