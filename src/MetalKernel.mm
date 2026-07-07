// PowerGrade — Metal GPU kernel (Apple). Mirrors src/PowerGradePipeline.h.
// SPDX-License-Identifier: BSD-3-Clause
#import <Metal/Metal.h>
#include <unordered_map>
#include <mutex>

const char* kernelSource = R"KERNEL(
#include <metal_stdlib>
using namespace metal;

inline float pg_pow(float b, float e){ return pow(fmax(b,0.0f), e); }

inline float pg_decode(int cam, float x){
    if(cam==0){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;
        return (x>LC)?(exp2(x/C-B)-A):(x/M); }
    else if(cam==1){ return (x>=171.2102946f/1023.0f)?(pg_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f-0.01f):((x*1023.0f-95.0f)*0.01125f/(171.2102946f-95.0f)); }
    else if(cam==2){ float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;
        return (x>e*cut+f)?((pg_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else if(cam==3){ float a=(exp2(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;
        float s=(7.0f*log(2.0f)*exp2(7.0f-14.0f*c/b))/(a*b); float t=(exp2(14.0f*(-c/b)+6.0f)-64.0f)/a;
        float p=(x-c)/b; float hi=(exp2(14.0f*p+6.0f)-64.0f)/a; return (hi>t)?hi:(x*s); }
    else if(cam==4){ float v=x; if(v<0.09755646f) return -(pg_pow(10.0f,(0.07623209f-v)/0.42889912f)-1.0f)/14.98325f;
        if(v<=0.15277891f) return (v-0.12512219f)/1.9754798f; return (pg_pow(10.0f,(v-0.19022340f)/0.42889912f)-1.0f)/14.98325f; }
    else if(cam==5){ return (pg_pow(10.0f,x/0.224282f)-1.0f)/155.975327f-0.01f; }
    else if(cam==6){ return (x<=0.14f)?((x-0.0929f)/6.025f):(pg_pow(10.0f,(x-0.5595f)/0.9892f)-0.0108f); }
    else if(cam==7){ float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;
        return (x>=cut)?((pg_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else { return (x<0.181f)?((x-0.125f)/5.6f):(pg_pow(10.0f,(x-0.598206f)/0.241514f)-0.00873f); } // Panasonic V-Log
}

inline float3 pg_mv(float3 r0,float3 r1,float3 r2,float3 v){ return float3(dot(r0,v),dot(r1,v),dot(r2,v)); }

inline float3 pg_toXYZ(int cam, float3 v){
    if(cam==0) return pg_mv(float3(0.7006223f,0.1487748f,0.1010587f),float3(0.2740150f,0.8736457f,-0.1476607f),float3(-0.0989629f,-0.1378905f,1.3259942f),v);
    else if(cam==1) return pg_mv(float3(0.5990839f,0.2489255f,0.1024464f),float3(0.2150758f,0.8850685f,-0.1001443f),float3(-0.0320658f,-0.0276902f,1.1487819f),v);
    else if(cam==2) return pg_mv(float3(0.6380076f,0.2147014f,0.0977226f),float3(0.2919283f,0.8238731f,-0.1158014f),float3(0.0027932f,-0.0670795f,1.1533751f),v);
    else if(cam==3) return pg_mv(float3(0.7048583f,0.1297602f,0.1158373f),float3(0.2545241f,0.7814843f,-0.0360084f),float3(0.0f,0.0f,1.0890577f),v);
    else if(cam==5) return pg_mv(float3(0.7352750f,0.0686090f,0.1465710f),float3(0.2866940f,0.8429790f,-0.1296730f),float3(-0.0796810f,-0.3473430f,1.5164950f),v);
    else if(cam==8) return pg_mv(float3(0.6796440f,0.1522110f,0.1186000f),float3(0.2606860f,0.7748940f,-0.0355800f),float3(-0.0093100f,-0.0046120f,1.1029800f),v);
    else return pg_mv(float3(0.6369580f,0.1446169f,0.1688810f),float3(0.2627002f,0.6779981f,0.0593017f),float3(0.0f,0.0280727f,1.0609851f),v);
}
inline float3 pg_XYZtoDWG(float3 v){ return pg_mv(float3(1.5166283f,-0.2814601f,-0.1469306f),float3(-0.4647205f,1.2513509f,0.1747665f),float3(0.0648641f,0.1091221f,0.7613593f),v); }
inline float3 pg_DWGtoXYZ(float3 v){ return pg_mv(float3(0.7006223f,0.1487748f,0.1010587f),float3(0.2740150f,0.8736457f,-0.1476607f),float3(-0.0989629f,-0.1378905f,1.3259942f),v); }
inline float3 pg_XYZto709(float3 v){ return pg_mv(float3(3.2409699f,-1.5373832f,-0.4986108f),float3(-0.9692436f,1.8759675f,0.0415551f),float3(0.0556301f,-0.2039770f,1.0569715f),v); }

inline float3 pg_rgb2hsv(float3 c){
    float mx=max(c.x,max(c.y,c.z)), mn=min(c.x,min(c.y,c.z));
    float v=mx, d=mx-mn; float s=(mx<=0.0f)?0.0f:(d/mx); float h=0.0f;
    if(d>0.0f){
        if(mx==c.x) h=(c.y-c.z)/d + (c.y<c.z?6.0f:0.0f);
        else if(mx==c.y) h=(c.z-c.x)/d + 2.0f;
        else h=(c.x-c.y)/d + 4.0f;
        h/=6.0f;
    }
    return float3(h,s,v);
}
inline float3 pg_hsv2rgb(float3 c){
    float h=c.x, s=c.y, v=c.z;
    if(s<=0.0f) return float3(v,v,v);
    h*=6.0f; int i=(int)floor(h); float f=h-(float)i;
    float p=v*(1.0f-s), q=v*(1.0f-s*f), t=v*(1.0f-s*(1.0f-f));
    i=i%6; if(i<0)i+=6;
    if(i==0) return float3(v,t,p); else if(i==1) return float3(q,v,p); else if(i==2) return float3(p,v,t);
    else if(i==3) return float3(p,q,v); else if(i==4) return float3(t,p,v); else return float3(v,p,q);
}

inline float pg_dienc(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); }
inline float pg_didec(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2(x/C-B)-A):(x/M); }

inline float pg_enc(int enc, float x){
    if(enc==0) return pg_pow(fmax(x,0.0f),1.0f/2.4f);
    else if(enc==1){ float code=685.0f+300.0f*log10(fmax(x,1e-4f)); return fmin(fmax(code/1023.0f,0.0f),1.0f); }
    else if(enc==2){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); }
    else return x;
}

inline float3 pg_sampleLUT(const device float* lut, int N, float3 c){
    float cr=clamp(c.x,0.0f,1.0f)*(N-1), cg=clamp(c.y,0.0f,1.0f)*(N-1), cb=clamp(c.z,0.0f,1.0f)*(N-1);
    int r0=(int)cr,g0=(int)cg,b0=(int)cb;
    int r1=r0<N-1?r0+1:r0, g1=g0<N-1?g0+1:g0, b1=b0<N-1?b0+1:b0;
    float fr=cr-r0, fg=cg-g0, fb=cb-b0;
    float3 res;
    for(int ch=0;ch<3;ch++){
        float c00=lut[((b0*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g0)*N+r1)*3+ch]*fr;
        float c10=lut[((b0*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g1)*N+r1)*3+ch]*fr;
        float c01=lut[((b1*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g0)*N+r1)*3+ch]*fr;
        float c11=lut[((b1*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g1)*N+r1)*3+ch]*fr;
        float c0=c00*(1-fg)+c10*fg, c1=c01*(1-fg)+c11*fg;
        res[ch]=c0*(1-fb)+c1*fb;
    }
    return res;
}

kernel void PowerGradeKernel(constant int& W [[buffer(11)]], constant int& H [[buffer(12)]],
    constant float* P [[buffer(13)]], constant int& cam [[buffer(14)]], constant int& enc [[buffer(15)]],
    const device float* lut [[buffer(1)]], constant int& lutN [[buffer(16)]], constant float& lutMix [[buffer(17)]],
    const device float* in [[buffer(0)]], device float* out [[buffer(8)]], uint2 id [[thread_position_in_grid]])
{
    if(id.x<(uint)W && id.y<(uint)H){
        int i=((id.y*W)+id.x)*4;
        float temp=P[0],tint=P[1],density=P[2],lift=P[3],gamma=P[4],gain=P[5];
        float3 lin = float3(pg_decode(cam,in[i]),pg_decode(cam,in[i+1]),pg_decode(cam,in[i+2]));
        float3 w = pg_XYZtoDWG(pg_toXYZ(cam,lin));           // working: DWG linear
        w.x*=(1.0f+temp*0.20f); w.z*=(1.0f-temp*0.20f); w.y*=(1.0f+tint*0.20f);   // balance
        if(density!=0.0f){ float3 hsv=pg_rgb2hsv(w); hsv.y=fmin(fmax(hsv.y*(1.0f+density),0.0f),1.0f); w=pg_hsv2rgb(hsv); } // density
        float3 outc = (enc==0||enc==1) ? pg_XYZto709(pg_DWGtoXYZ(w)) : w;         // output primaries
        float3 e = float3(pg_enc(enc,outc.x), pg_enc(enc,outc.y), pg_enc(enc,outc.z));
        e.x=pg_pow(e.x*(gain-lift)+lift,1.0f/gamma); e.y=pg_pow(e.y*(gain-lift)+lift,1.0f/gamma); e.z=pg_pow(e.z*(gain-lift)+lift,1.0f/gamma); // LGG in display space
        if(lutN>=2 && lutMix>0.0f){ float3 s=pg_sampleLUT(lut,lutN,e); e = e + (s-e)*lutMix; }  // LUT + mix
        out[i]=e.x; out[i+1]=e.y; out[i+2]=e.z; out[i+3]=in[i+3];
    }
}
)KERNEL";

std::mutex s_PipelineQueueMutex;
typedef std::unordered_map<id<MTLCommandQueue>, id<MTLComputePipelineState>> PipelineQueueMap;
PipelineQueueMap s_PipelineQueueMap;

void RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode,
                    const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output)
{
    const char* kernelName = "PowerGradeKernel";

    id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue> >(p_CmdQ);
    id<MTLDevice> device = queue.device;
    id<MTLComputePipelineState> pipelineState;

    std::unique_lock<std::mutex> lock(s_PipelineQueueMutex);
    const auto it = s_PipelineQueueMap.find(queue);
    if (it == s_PipelineQueueMap.end())
    {
        id<MTLLibrary> metalLibrary;
        id<MTLFunction> kernelFunction;
        NSError* err;

        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = YES;
        if (!(metalLibrary = [device newLibraryWithSource:@(kernelSource) options:options error:&err]))
        {
            fprintf(stderr, "PowerGrade: failed to load metal library, %s\n", err.localizedDescription.UTF8String);
            return;
        }
        [options release];
        if (!(kernelFunction = [metalLibrary newFunctionWithName:[NSString stringWithUTF8String:kernelName]]))
        {
            fprintf(stderr, "PowerGrade: failed to retrieve kernel\n");
            [metalLibrary release];
            return;
        }
        if (!(pipelineState = [device newComputePipelineStateWithFunction:kernelFunction error:&err]))
        {
            fprintf(stderr, "PowerGrade: unable to compile, %s\n", err.localizedDescription.UTF8String);
            [metalLibrary release];
            [kernelFunction release];
            return;
        }
        s_PipelineQueueMap[queue] = pipelineState;
        [metalLibrary release];
        [kernelFunction release];
    }
    else
    {
        pipelineState = it->second;
    }

    id<MTLBuffer> srcDeviceBuf = reinterpret_cast<id<MTLBuffer> >(const_cast<float*>(p_Input));
    id<MTLBuffer> dstDeviceBuf = reinterpret_cast<id<MTLBuffer> >(p_Output);

    // LUT buffer: upload the 3D LUT (or a tiny dummy when none), released on GPU completion.
    int lutN = (p_Lut && p_LutSize >= 2) ? p_LutSize : 0;
    NSUInteger lutBytes = lutN ? (NSUInteger)lutN*lutN*lutN*3*sizeof(float) : sizeof(float);
    float dummy = 0.0f;
    id<MTLBuffer> lutBuf = [device newBufferWithBytes:(lutN ? (const void*)p_Lut : (const void*)&dummy)
                                               length:lutBytes options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    commandBuffer.label = @"PowerGradeKernel";

    id<MTLComputeCommandEncoder> computeEncoder = [commandBuffer computeCommandEncoder];
    [computeEncoder setComputePipelineState:pipelineState];

    int exeWidth = [pipelineState threadExecutionWidth];
    MTLSize threadGroupCount = MTLSizeMake(exeWidth, 1, 1);
    MTLSize threadGroups     = MTLSizeMake((p_Width + exeWidth - 1) / exeWidth, p_Height, 1);

    [computeEncoder setBuffer:srcDeviceBuf offset:0 atIndex:0];
    [computeEncoder setBuffer:lutBuf       offset:0 atIndex:1];
    [computeEncoder setBuffer:dstDeviceBuf offset:0 atIndex:8];
    [computeEncoder setBytes:&p_Width  length:sizeof(int) atIndex:11];
    [computeEncoder setBytes:&p_Height length:sizeof(int) atIndex:12];
    [computeEncoder setBytes:p_Params  length:sizeof(float)*6 atIndex:13];
    [computeEncoder setBytes:&p_Camera length:sizeof(int) atIndex:14];
    [computeEncoder setBytes:&p_Encode length:sizeof(int) atIndex:15];
    [computeEncoder setBytes:&lutN     length:sizeof(int) atIndex:16];
    [computeEncoder setBytes:&p_LutMix length:sizeof(float) atIndex:17];

    [computeEncoder dispatchThreadgroups:threadGroups threadsPerThreadgroup:threadGroupCount];
    [computeEncoder endEncoding];
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>){ [lutBuf release]; }];
    [commandBuffer commit];
}
