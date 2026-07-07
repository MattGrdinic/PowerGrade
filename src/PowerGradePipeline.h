// PowerGrade — shared CPU-side color pipeline (single source of truth for the math).
// The GPU kernels (Metal/OpenCL/CUDA) mirror this exact math.
// SPDX-License-Identifier: BSD-3-Clause
//
//  Faithful to the reference node tree:  CST -> Balance -> Density -> Exposure -> Output
//    1. camera log -> scene-linear            (per-camera decode)
//    2. camera gamut -> XYZ -> DaVinci Wide Gamut LINEAR   (working space, matches tree)
//    3. Balance   : 2-axis white balance (temp/tint) in linear      [linear color-wheel analog]
//    4. Density   : HSV saturation gain  (the "green channel of Gain in HSV" trick)
//    5. Exposure  : basic Lift / Gamma / Gain
//    6. Output    : DWG -> {Rec709 g2.4 | Cineon Log | DaVinci Intermediate | Linear}
//
//  P[] layout: {temp, tint, density, lift, gamma, gain}
#pragma once
#include <cmath>
#include <algorithm>

namespace pg {

static inline float safe_pow(float b, float e) { return powf(b < 0.f ? 0.f : b, e); }
static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// ---------- camera log -> scene-linear (mid-gray 0.18) ----------
static inline float decode_log(int cam, float x)
{
    if (cam == 0) { // DaVinci Intermediate
        const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;
        return (x > LC) ? (exp2f(x/C - B) - A) : (x / M);
    } else if (cam == 1) { // Sony S-Log3
        return (x >= 171.2102946f/1023.0f)
            ? (safe_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f - 0.01f)
            : ((x*1023.0f-95.0f)*0.01125000f/(171.2102946f-95.0f));
    } else if (cam == 2) { // ARRI LogC3 (EI800)
        const float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;
        return (x > e*cut+f) ? ((safe_pow(10.0f,(x-d)/c)-b)/a) : ((x-f)/e);
    } else if (cam == 3) { // ARRI LogC4
        const float a=(exp2f(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;
        const float s=(7.0f*logf(2.0f)*exp2f(7.0f-14.0f*c/b))/(a*b);
        const float t=(exp2f(14.0f*(-c/b)+6.0f)-64.0f)/a;
        float p=(x-c)/b; float hi=(exp2f(14.0f*p+6.0f)-64.0f)/a;
        return (hi > t) ? hi : (x*s);
    } else if (cam == 4) { // Canon Log 3
        float v=x;
        if (v < 0.09755646f) return -(safe_pow(10.0f,(0.07623209f - v)/0.42889912f)-1.0f)/14.98325f;
        if (v <= 0.15277891f) return (v - 0.12512219f)/1.9754798f;
        return (safe_pow(10.0f,(v - 0.19022340f)/0.42889912f)-1.0f)/14.98325f;
    } else if (cam == 5) { // RED Log3G10
        return (safe_pow(10.0f, x/0.224282f) - 1.0f)/155.975327f - 0.01f;
    } else if (cam == 6) { // DJI D-Log
        return (x <= 0.14f) ? ((x-0.0929f)/6.025f) : (safe_pow(10.0f,(x-0.5595f)/0.9892f) - 0.0108f);
    } else if (cam == 7) { // Fuji F-Log2
        const float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;
        return (x >= cut) ? ((safe_pow(10.0f,(x-d)/c)-b)/a) : ((x-f)/e);
    } else { // Panasonic V-Log
        return (x < 0.181f) ? ((x - 0.125f)/5.6f) : (safe_pow(10.0f,(x - 0.598206f)/0.241514f) - 0.00873f);
    }
}

static inline void mul33(const float m[9], const float v[3], float o[3])
{
    o[0]=m[0]*v[0]+m[1]*v[1]+m[2]*v[2];
    o[1]=m[3]*v[0]+m[4]*v[1]+m[5]*v[2];
    o[2]=m[6]*v[0]+m[7]*v[1]+m[8]*v[2];
}

// camera native gamut -> CIE XYZ (D65)
static inline void to_XYZ(int cam, const float v[3], float o[3])
{
    float m[9];
    if (cam == 0) { float t[9]={0.7006223f,0.1487748f,0.1010587f, 0.2740150f,0.8736457f,-0.1476607f, -0.0989629f,-0.1378905f,1.3259942f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 1) { float t[9]={0.5990839f,0.2489255f,0.1024464f, 0.2150758f,0.8850685f,-0.1001443f, -0.0320658f,-0.0276902f,1.1487819f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 2) { float t[9]={0.6380076f,0.2147014f,0.0977226f, 0.2919283f,0.8238731f,-0.1158014f, 0.0027932f,-0.0670795f,1.1533751f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 3) { float t[9]={0.7048583f,0.1297602f,0.1158373f, 0.2545241f,0.7814843f,-0.0360084f, 0.0f,0.0f,1.0890577f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 5) { float t[9]={0.7352750f,0.0686090f,0.1465710f, 0.2866940f,0.8429790f,-0.1296730f, -0.0796810f,-0.3473430f,1.5164950f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 8) { float t[9]={0.6796440f,0.1522110f,0.1186000f, 0.2606860f,0.7748940f,-0.0355800f, -0.0093100f,-0.0046120f,1.1029800f}; for(int i=0;i<9;i++)m[i]=t[i]; } // Panasonic V-Gamut
    else { float t[9]={0.6369580f,0.1446169f,0.1688810f, 0.2627002f,0.6779981f,0.0593017f, 0.0f,0.0280727f,1.0609851f}; for(int i=0;i<9;i++)m[i]=t[i]; } // 4,6,7 -> Rec2020 stand-in
    mul33(m, v, o);
}

// CIE XYZ(D65) -> DaVinci Wide Gamut linear (working space)
static inline void XYZ_to_DWG(const float v[3], float o[3])
{
    const float m[9]={ 1.5166283f,-0.2814601f,-0.1469306f, -0.4647205f,1.2513509f,0.1747665f, 0.0648641f,0.1091221f,0.7613593f };
    mul33(m, v, o);
}
// DaVinci Wide Gamut linear -> CIE XYZ(D65)
static inline void DWG_to_XYZ(const float v[3], float o[3])
{
    const float m[9]={ 0.7006223f,0.1487748f,0.1010587f, 0.2740150f,0.8736457f,-0.1476607f, -0.0989629f,-0.1378905f,1.3259942f };
    mul33(m, v, o);
}
// CIE XYZ(D65) -> Rec.709 linear
static inline void XYZ_to_709(const float v[3], float o[3])
{
    const float m[9]={ 3.2409699f,-1.5373832f,-0.4986108f, -0.9692436f,1.8759675f,0.0415551f, 0.0556301f,-0.2039770f,1.0569715f };
    mul33(m, v, o);
}

// ---------- HSV (works on linear values; V = max, S in [0,1]) ----------
static inline void rgb2hsv(float r,float g,float b,float& h,float& s,float& v)
{
    float mx = fmaxf(r, fmaxf(g,b)), mn = fminf(r, fminf(g,b));
    v = mx; float d = mx - mn;
    s = (mx <= 0.f) ? 0.f : (d / mx);
    if (d <= 0.f) { h = 0.f; return; }
    if (mx == r)      h = (g-b)/d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b-r)/d + 2.f;
    else              h = (r-g)/d + 4.f;
    h /= 6.f;
}
static inline void hsv2rgb(float h,float s,float v,float& r,float& g,float& b)
{
    if (s <= 0.f) { r = g = b = v; return; }
    h *= 6.f; int i = (int)floorf(h); float f = h - i;
    float p = v*(1.f-s), q = v*(1.f-s*f), t = v*(1.f-s*(1.f-f));
    i %= 6; if (i < 0) i += 6;
    switch (i) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default:r=v; g=p; b=q; break;
    }
}

// DaVinci Intermediate log encode/decode (the space the tree's Lift/Gamma/Gain runs in)
static inline float di_encode(float x)
{ const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); }
static inline float di_decode(float x)
{ const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2f(x/C-B)-A):(x/M); }

static inline float encode(int enc, float x)
{
    if (enc == 0) return safe_pow(x < 0.f ? 0.f : x, 1.0f/2.4f);          // Rec.709 gamma 2.4
    if (enc == 1) { float code = 685.0f + 300.0f*log10f(x < 1e-4f ? 1e-4f : x); return std::min(std::max(code/1023.0f,0.0f),1.0f); } // Cineon
    if (enc == 2) { const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); } // DI
    return x;                                                             // linear
}

// ---------- 3D LUT trilinear sampling + mix ----------
//  lut: N*N*N*3, red index fastest. Input rgb clamped to [0,1]. mix in [0,1].
static inline void apply_lut(const float* lut, int N, float mix,
                             float& r, float& g, float& b)
{
    if (N < 2 || mix <= 0.f) return;
    float cr = clamp01(r)*(N-1), cg = clamp01(g)*(N-1), cb = clamp01(b)*(N-1);
    int r0=(int)cr, g0=(int)cg, b0=(int)cb;
    int r1 = r0<N-1?r0+1:r0, g1 = g0<N-1?g0+1:g0, b1 = b0<N-1?b0+1:b0;
    float fr=cr-r0, fg=cg-g0, fb=cb-b0;
    #define PG_IDX(R,G,B) (((size_t)(B)*N + (G))*N + (R))*3
    float out[3];
    for (int c=0;c<3;c++) {
        float c00 = lut[PG_IDX(r0,g0,b0)+c]*(1-fr) + lut[PG_IDX(r1,g0,b0)+c]*fr;
        float c10 = lut[PG_IDX(r0,g1,b0)+c]*(1-fr) + lut[PG_IDX(r1,g1,b0)+c]*fr;
        float c01 = lut[PG_IDX(r0,g0,b1)+c]*(1-fr) + lut[PG_IDX(r1,g0,b1)+c]*fr;
        float c11 = lut[PG_IDX(r0,g1,b1)+c]*(1-fr) + lut[PG_IDX(r1,g1,b1)+c]*fr;
        float c0 = c00*(1-fg) + c10*fg;
        float c1 = c01*(1-fg) + c11*fg;
        out[c] = c0*(1-fb) + c1*fb;
    }
    #undef PG_IDX
    r = r + (out[0]-r)*mix;
    g = g + (out[1]-g)*mix;
    b = b + (out[2]-b)*mix;
}

// full per-pixel process. in/out are RGB (alpha handled by caller).
static inline void process(int cam, int enc, const float* P, float inR, float inG, float inB,
                           float& outR, float& outG, float& outB)
{
    const float temp=P[0], tint=P[1], density=P[2], lift=P[3], gamma=P[4], gain=P[5], offTemp=P[6], offTint=P[7];

    // 1-2. decode + into DWG-linear working space
    float lin[3] = { decode_log(cam,inR), decode_log(cam,inG), decode_log(cam,inB) };
    float xyz[3]; to_XYZ(cam, lin, xyz);
    float w[3];   XYZ_to_DWG(xyz, w);

    // 3. Balance (2-axis white balance in linear)
    //    Gain (multiplicative) — neutral highlights
    w[0] *= (1.0f + temp*0.20f);
    w[2] *= (1.0f - temp*0.20f);
    w[1] *= (1.0f + tint*0.20f);
    //    Offset (additive) — shifts every tone's chroma evenly (best for stubborn casts)
    w[0] += offTemp*0.10f;
    w[2] -= offTemp*0.10f;
    w[1] += offTint*0.10f;

    // 4. Density (HSV saturation gain in DI-log — the "green of Gain in HSV" trick).
    //    Run in log (not linear) so saturated highlights get richer instead of blowing out.
    if (density != 0.0f) {
        float l0=di_encode(w[0]), l1=di_encode(w[1]), l2=di_encode(w[2]);
        float h,s,v; rgb2hsv(l0,l1,l2,h,s,v);
        s = clamp01(s * (1.0f + density));
        hsv2rgb(h,s,v,l0,l1,l2);
        w[0]=di_decode(l0); w[1]=di_decode(l1); w[2]=di_decode(l2);
    }

    // 5. Exposure — Lift / Gamma / Gain in DaVinci Intermediate (log), like the tree's
    //    default wheels. Running in log (not linear) makes all three behave consistently
    //    as normal SDR wheels instead of Lift acting like an HDR/linear wheel.
    // 5. Output encode (working -> display/log)
    float outc[3];
    if (enc == 0 || enc == 1) {           // Rec.709 primaries (display / film-LUT feed)
        float x2[3]; DWG_to_XYZ(w, x2);
        XYZ_to_709(x2, outc);
    } else {                              // DI / Linear keep DWG primaries
        outc[0]=w[0]; outc[1]=w[1]; outc[2]=w[2];
    }
    float e0 = encode(enc, outc[0]), e1 = encode(enc, outc[1]), e2 = encode(enc, outc[2]);

    // 6. Lift / Gamma / Gain in the OUTPUT (display) space, so the wheels pivot exactly
    //    like Resolve's primary wheels: gain @ black(0), lift @ white(1), gamma @ both.
    //    out = (in*(gain - lift) + lift) ^ (1/gamma)
    outR = safe_pow(e0*(gain - lift) + lift, 1.0f/gamma);
    outG = safe_pow(e1*(gain - lift) + lift, 1.0f/gamma);
    outB = safe_pow(e2*(gain - lift) + lift, 1.0f/gamma);
}

} // namespace pg
