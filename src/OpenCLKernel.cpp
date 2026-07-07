// PowerGrade — OpenCL GPU kernel (buffers). Mirrors src/PowerGradePipeline.h.
// SPDX-License-Identifier: BSD-3-Clause

#ifdef _WIN64
#include <Windows.h>
#else
#include <pthread.h>
#endif
#include <map>
#include <cstdio>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

const char* KernelSource = "\n" \
"inline float pg_pow(float b, float e){ return pow(fmax(b,0.0f), e); }                                          \n" \
"inline float pg_decode(int cam, float x){                                                                      \n" \
"  if(cam==0){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;                              \n" \
"    return (x>LC)?(exp2(x/C-B)-A):(x/M); }                                                                      \n" \
"  else if(cam==1){ return (x>=171.2102946f/1023.0f)?(pg_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f-0.01f):((x*1023.0f-95.0f)*0.01125f/(171.2102946f-95.0f)); } \n" \
"  else if(cam==2){ float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;\n" \
"    return (x>e*cut+f)?((pg_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }                                               \n" \
"  else if(cam==3){ float a=(exp2(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;              \n" \
"    float s=(7.0f*log(2.0f)*exp2(7.0f-14.0f*c/b))/(a*b); float t=(exp2(14.0f*(-c/b)+6.0f)-64.0f)/a;            \n" \
"    float p=(x-c)/b; float hi=(exp2(14.0f*p+6.0f)-64.0f)/a; return (hi>t)?hi:(x*s); }                          \n" \
"  else if(cam==4){ float v=x; if(v<0.09755646f) return -(pg_pow(10.0f,(0.07623209f-v)/0.42889912f)-1.0f)/14.98325f; \n" \
"    if(v<=0.15277891f) return (v-0.12512219f)/1.9754798f; return (pg_pow(10.0f,(v-0.19022340f)/0.42889912f)-1.0f)/14.98325f; } \n" \
"  else if(cam==5){ return (pg_pow(10.0f,x/0.224282f)-1.0f)/155.975327f-0.01f; }                                \n" \
"  else if(cam==6){ return (x<=0.14f)?((x-0.0929f)/6.025f):(pg_pow(10.0f,(x-0.5595f)/0.9892f)-0.0108f); }       \n" \
"  else { float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;       \n" \
"    return (x>=cut)?((pg_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); } }                                                \n" \
"inline float3 pg_mv(float3 r0,float3 r1,float3 r2,float3 v){ return (float3)(dot(r0,v),dot(r1,v),dot(r2,v)); }  \n" \
"inline float3 pg_toXYZ(int cam, float3 v){                                                                     \n" \
"  if(cam==0) return pg_mv((float3)(0.7006223f,0.1487748f,0.1010587f),(float3)(0.2740150f,0.8736457f,-0.1476607f),(float3)(-0.0989629f,-0.1378905f,1.3259942f),v); \n" \
"  else if(cam==1) return pg_mv((float3)(0.5990839f,0.2489255f,0.1024464f),(float3)(0.2150758f,0.8850685f,-0.1001443f),(float3)(-0.0320658f,-0.0276902f,1.1487819f),v); \n" \
"  else if(cam==2) return pg_mv((float3)(0.6380076f,0.2147014f,0.0977226f),(float3)(0.2919283f,0.8238731f,-0.1158014f),(float3)(0.0027932f,-0.0670795f,1.1533751f),v); \n" \
"  else if(cam==3) return pg_mv((float3)(0.7048583f,0.1297602f,0.1158373f),(float3)(0.2545241f,0.7814843f,-0.0360084f),(float3)(0.0f,0.0f,1.0890577f),v); \n" \
"  else if(cam==5) return pg_mv((float3)(0.7352750f,0.0686090f,0.1465710f),(float3)(0.2866940f,0.8429790f,-0.1296730f),(float3)(-0.0796810f,-0.3473430f,1.5164950f),v); \n" \
"  else return pg_mv((float3)(0.6369580f,0.1446169f,0.1688810f),(float3)(0.2627002f,0.6779981f,0.0593017f),(float3)(0.0f,0.0280727f,1.0609851f),v); }  \n" \
"inline float3 pg_XYZtoDWG(float3 v){ return pg_mv((float3)(1.5166283f,-0.2814601f,-0.1469306f),(float3)(-0.4647205f,1.2513509f,0.1747665f),(float3)(0.0648641f,0.1091221f,0.7613593f),v); } \n" \
"inline float3 pg_DWGtoXYZ(float3 v){ return pg_mv((float3)(0.7006223f,0.1487748f,0.1010587f),(float3)(0.2740150f,0.8736457f,-0.1476607f),(float3)(-0.0989629f,-0.1378905f,1.3259942f),v); } \n" \
"inline float3 pg_XYZto709(float3 v){ return pg_mv((float3)(3.2409699f,-1.5373832f,-0.4986108f),(float3)(-0.9692436f,1.8759675f,0.0415551f),(float3)(0.0556301f,-0.2039770f,1.0569715f),v); } \n" \
"inline float3 pg_rgb2hsv(float3 c){                                                                            \n" \
"  float mx=fmax(c.x,fmax(c.y,c.z)), mn=fmin(c.x,fmin(c.y,c.z)); float v=mx,d=mx-mn; float s=(mx<=0.0f)?0.0f:(d/mx); float h=0.0f; \n" \
"  if(d>0.0f){ if(mx==c.x) h=(c.y-c.z)/d+(c.y<c.z?6.0f:0.0f); else if(mx==c.y) h=(c.z-c.x)/d+2.0f; else h=(c.x-c.y)/d+4.0f; h/=6.0f; } \n" \
"  return (float3)(h,s,v); }                                                                                    \n" \
"inline float3 pg_hsv2rgb(float3 c){                                                                            \n" \
"  float h=c.x,s=c.y,v=c.z; if(s<=0.0f) return (float3)(v,v,v);                                                 \n" \
"  h*=6.0f; int i=(int)floor(h); float f=h-(float)i; float p=v*(1.0f-s),q=v*(1.0f-s*f),t=v*(1.0f-s*(1.0f-f)); i=i%6; if(i<0)i+=6; \n" \
"  if(i==0) return (float3)(v,t,p); else if(i==1) return (float3)(q,v,p); else if(i==2) return (float3)(p,v,t); \n" \
"  else if(i==3) return (float3)(p,q,v); else if(i==4) return (float3)(t,p,v); else return (float3)(v,p,q); }   \n" \
"inline float pg_dienc(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); } \n" \
"inline float pg_didec(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2(x/C-B)-A):(x/M); } \n" \
"inline float pg_enc(int enc, float x){                                                                         \n" \
"  if(enc==0) return pg_pow(fmax(x,0.0f),1.0f/2.4f);                                                             \n" \
"  else if(enc==1){ float code=685.0f+300.0f*log10(fmax(x,1e-4f)); return fmin(fmax(code/1023.0f,0.0f),1.0f); }  \n" \
"  else if(enc==2){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); } \n" \
"  else return x; }                                                                                             \n" \
"inline float3 pg_sampleLUT(__global const float* lut,int N,float3 c){                                          \n" \
"  float cr=clamp(c.x,0.0f,1.0f)*(N-1),cg=clamp(c.y,0.0f,1.0f)*(N-1),cb=clamp(c.z,0.0f,1.0f)*(N-1);             \n" \
"  int r0=(int)cr,g0=(int)cg,b0=(int)cb; int r1=r0<N-1?r0+1:r0,g1=g0<N-1?g0+1:g0,b1=b0<N-1?b0+1:b0;             \n" \
"  float fr=cr-r0,fg=cg-g0,fb=cb-b0; float res[3];                                                              \n" \
"  for(int ch=0;ch<3;ch++){                                                                                     \n" \
"    float c00=lut[((b0*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g0)*N+r1)*3+ch]*fr;                                 \n" \
"    float c10=lut[((b0*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g1)*N+r1)*3+ch]*fr;                                 \n" \
"    float c01=lut[((b1*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g0)*N+r1)*3+ch]*fr;                                 \n" \
"    float c11=lut[((b1*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g1)*N+r1)*3+ch]*fr;                                 \n" \
"    float c0=c00*(1-fg)+c10*fg,c1=c01*(1-fg)+c11*fg; res[ch]=c0*(1-fb)+c1*fb; }                                \n" \
"  return (float3)(res[0],res[1],res[2]); }                                                                     \n" \
"__kernel void PowerGradeKernel(int W,int H,int cam,int enc,                                                    \n" \
"  float temp,float tint,float density,float lift,float gamma,float gain,                                       \n" \
"  int lutN, float lutMix, __global const float* lut,                                                           \n" \
"  __global const float* in, __global float* out) {                                                             \n" \
"  const int x=get_global_id(0); const int y=get_global_id(1);                                                  \n" \
"  if(x<W && y<H){                                                                                              \n" \
"    const int i=((y*W)+x)*4;                                                                                    \n" \
"    float3 lin=(float3)(pg_decode(cam,in[i]),pg_decode(cam,in[i+1]),pg_decode(cam,in[i+2]));                   \n" \
"    float3 w=pg_XYZtoDWG(pg_toXYZ(cam,lin));                                                                    \n" \
"    w.x*=(1.0f+temp*0.20f); w.z*=(1.0f-temp*0.20f); w.y*=(1.0f+tint*0.20f);                                     \n" \
"    if(density!=0.0f){ float3 hsv=pg_rgb2hsv(w); hsv.y=fmin(fmax(hsv.y*(1.0f+density),0.0f),1.0f); w=pg_hsv2rgb(hsv); } \n" \
"    w.x=pg_didec(pg_pow(pg_dienc(w.x)*gain+lift,1.0f/gamma)); w.y=pg_didec(pg_pow(pg_dienc(w.y)*gain+lift,1.0f/gamma)); w.z=pg_didec(pg_pow(pg_dienc(w.z)*gain+lift,1.0f/gamma)); \n" \
"    float3 outc=(enc==0||enc==1)?pg_XYZto709(pg_DWGtoXYZ(w)):w;                                                 \n" \
"    float3 e=(float3)(pg_enc(enc,outc.x),pg_enc(enc,outc.y),pg_enc(enc,outc.z));                                \n" \
"    if(lutN>=2 && lutMix>0.0f){ float3 s=pg_sampleLUT(lut,lutN,e); e=e+(s-e)*lutMix; }                          \n" \
"    out[i]=e.x; out[i+1]=e.y; out[i+2]=e.z; out[i+3]=in[i+3];                                                  \n" \
"  } }                                                                                                          \n" \
"\n";

void CheckError(cl_int p_Error, const char* p_Msg)
{
    if (p_Error != CL_SUCCESS) fprintf(stderr, "PowerGrade OpenCL: %s [%d]\n", p_Msg, p_Error);
}

class Locker
{
public:
    Locker()  {
#ifdef _WIN64
        InitializeCriticalSection(&mutex);
#else
        pthread_mutex_init(&mutex, NULL);
#endif
    }
    ~Locker() {
#ifdef _WIN64
        DeleteCriticalSection(&mutex);
#else
        pthread_mutex_destroy(&mutex);
#endif
    }
    void Lock()   {
#ifdef _WIN64
        EnterCriticalSection(&mutex);
#else
        pthread_mutex_lock(&mutex);
#endif
    }
    void Unlock() {
#ifdef _WIN64
        LeaveCriticalSection(&mutex);
#else
        pthread_mutex_unlock(&mutex);
#endif
    }
private:
#ifdef _WIN64
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
};

void RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode,
                            const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output)
{
    cl_int error;
    cl_command_queue cmdQ = static_cast<cl_command_queue>(p_CmdQ);

    static std::map<cl_command_queue, cl_device_id> deviceIdMap;
    static std::map<cl_command_queue, cl_kernel> kernelMap;
    static Locker locker;

    locker.Lock();

    cl_device_id deviceId = NULL;
    if (deviceIdMap.find(cmdQ) == deviceIdMap.end())
    {
        error = clGetCommandQueueInfo(cmdQ, CL_QUEUE_DEVICE, sizeof(cl_device_id), &deviceId, NULL);
        CheckError(error, "Unable to get the device");
        deviceIdMap[cmdQ] = deviceId;
    }
    else
    {
        deviceId = deviceIdMap[cmdQ];
    }

    cl_context clContext = NULL;
    error = clGetCommandQueueInfo(cmdQ, CL_QUEUE_CONTEXT, sizeof(cl_context), &clContext, NULL);
    CheckError(error, "Unable to get the context");

    cl_kernel kernel = NULL;
    if (kernelMap.find(cmdQ) == kernelMap.end())
    {
        cl_program program = clCreateProgramWithSource(clContext, 1, (const char**)&KernelSource, NULL, &error);
        CheckError(error, "Unable to create program");
        error = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
        CheckError(error, "Unable to build program");
        kernel = clCreateKernel(program, "PowerGradeKernel", &error);
        CheckError(error, "Unable to create kernel");
        kernelMap[cmdQ] = kernel;
    }
    else
    {
        kernel = kernelMap[cmdQ];
    }

    locker.Unlock();

    // Upload the LUT (or a 1-float dummy when none).
    int lutN = (p_Lut && p_LutSize >= 2) ? p_LutSize : 0;
    size_t lutBytes = lutN ? (size_t)lutN*lutN*lutN*3*sizeof(float) : sizeof(float);
    float dummy = 0.0f;
    cl_mem lutBuf = clCreateBuffer(clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, lutBytes,
                                   (void*)(lutN ? (const void*)p_Lut : (const void*)&dummy), &error);
    CheckError(error, "Unable to create LUT buffer");

    int count = 0;
    error  = clSetKernelArg(kernel, count++, sizeof(int), &p_Width);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Height);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Camera);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Encode);
    for (int i = 0; i < 6; ++i)
        error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Params[i]);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &lutN);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_LutMix);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &lutBuf);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &p_Input);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &p_Output);
    CheckError(error, "Unable to set kernel arguments");

    size_t localWorkSize[2], globalWorkSize[2];
    clGetKernelWorkGroupInfo(kernel, deviceId, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), localWorkSize, NULL);
    localWorkSize[1] = 1;
    globalWorkSize[0] = ((p_Width + localWorkSize[0] - 1) / localWorkSize[0]) * localWorkSize[0];
    globalWorkSize[1] = p_Height;

    clEnqueueNDRangeKernel(cmdQ, kernel, 2, NULL, globalWorkSize, localWorkSize, 0, NULL, NULL);
    clReleaseMemObject(lutBuf);
}
