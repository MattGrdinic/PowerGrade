// PowerGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// SPDX-License-Identifier: BSD-3-Clause

#include "PowerGrade.h"
#include "PowerGradePipeline.h"
#include "CubeLUT.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <map>
#include <filesystem>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"
#include "ofxsLog.h"

#define kPluginName        "PowerGrade"
#define kPluginGrouping    "Power Grade"
#define kPluginDescription "One-node camera CST + balance + density + exposure + output encode. " \
                           "Set Output Encode to Cineon Log to feed a film-look LUT node."
#define kPluginIdentifier  "com.mattgrdinic.PowerGrade"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kSupportsTiles              false
#define kSupportsMultiResolution    false
#define kSupportsMultipleClipPARs   false

#define kParamCount 10 // temp,tint,density,lift,gamma,gain,offTemp,offTint,postExp,postCon

// Folder scanned for built-in / film-look LUTs (Resolve's default LUT install).
#define kFilmLutDir "/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT"

// LUT lists, built once at describe.
typedef std::vector<std::pair<std::string, std::string>> LutList;   // (label, absolute path)
typedef std::pair<std::string, LutList> LutGroup;                   // (group name, luts)

//   s_FilmLuts: Resolve's "Film Looks" folder (print-emulation, need Cineon input).
//   s_LookGroups: the whole master LUT folder, grouped by top-level subfolder (Group -> LUT cascade).
static LutList s_FilmLuts;
static std::vector<LutGroup> s_LookGroups;
static bool s_Scanned = false;

static void scanDir(const std::string& root, LutList& out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".cube") { out.emplace_back(p.stem().string(), p.string()); if (out.size() >= 1000) break; }
    }
    std::sort(out.begin(), out.end());
}

static void scanLuts()
{
    if (s_Scanned) return;
    s_Scanned = true;
    namespace fs = std::filesystem;
    std::error_code ec;

    std::string filmDir = std::string(kFilmLutDir) + "/Film Looks";
    scanDir(fs::exists(filmDir, ec) ? filmDir : kFilmLutDir, s_FilmLuts);

    // Group the whole master folder by top-level subfolder (files in root -> "General").
    std::map<std::string, LutList> groups;
    if (fs::exists(kFilmLutDir, ec)) {
        for (fs::recursive_directory_iterator it(kFilmLutDir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".cube") continue;
            std::error_code ec2;
            fs::path rel = fs::relative(p, kFilmLutDir, ec2);
            std::string group = rel.has_parent_path() ? rel.begin()->string() : "General";
            groups[group].emplace_back(p.stem().string(), p.string());
        }
    }
    for (auto& g : groups) { std::sort(g.second.begin(), g.second.end()); s_LookGroups.emplace_back(g.first, g.second); }
    // std::map already sorts group names alphabetically.
}

////////////////////////////////////////////////////////////////////////////////

class PowerGradeProcessor : public OFX::ImageProcessor
{
public:
    explicit PowerGradeProcessor(OFX::ImageEffect& p_Instance);

    virtual void processImagesCuda();
    virtual void processImagesOpenCL();
    virtual void processImagesMetal();
    virtual void multiThreadProcessImages(OfxRectI p_ProcWindow);

    void setSrcImg(OFX::Image* p_SrcImg);
    void setParams(const float p_Params[kParamCount], int p_Camera, int p_Encode);
    void setLut(const float* p_Lut, int p_LutSize, float p_LutMix);

private:
    OFX::Image* _srcImg = nullptr;
    float _params[kParamCount];
    int   _camera = 0;
    int   _encode = 0;
    const float* _lut = nullptr;   // not owned
    int   _lutSize = 0;
    float _lutMix = 0.0f;
};

PowerGradeProcessor::PowerGradeProcessor(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
{
    for (int i = 0; i < kParamCount; ++i) _params[i] = 0.0f;
}

#ifdef OFX_SUPPORTS_CUDARENDER
extern void RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);
#endif

void PowerGradeProcessor::processImagesCuda()
{
#ifdef OFX_SUPPORTS_CUDARENDER
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunCudaKernel(_pCudaStream, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

#ifdef __APPLE__
extern void RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);
#endif

void PowerGradeProcessor::processImagesMetal()
{
#ifdef __APPLE__
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunMetalKernel(_pMetalCmdQ, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

extern void RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);

void PowerGradeProcessor::processImagesOpenCL()
{
#ifdef OFX_SUPPORTS_OPENCLRENDER
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunOpenCLKernelBuffers(_pOpenCLCmdQ, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

void PowerGradeProcessor::multiThreadProcessImages(OfxRectI p_ProcWindow)
{
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : nullptr);
            if (srcPix)
            {
                pg::process(_camera, _encode, _params, srcPix[0], srcPix[1], srcPix[2],
                            dstPix[0], dstPix[1], dstPix[2]);
                if (_lut && _lutSize >= 2 && _lutMix > 0.0f)
                    pg::apply_lut(_lut, _lutSize, _lutMix, dstPix[0], dstPix[1], dstPix[2]);
                pg::apply_trim(_params[8], _params[9], dstPix[0], dstPix[1], dstPix[2]);  // post-LUT trim
                dstPix[3] = srcPix[3];
            }
            else
            {
                for (int c = 0; c < 4; ++c) dstPix[c] = 0;
            }
            dstPix += 4;
        }
    }
}

void PowerGradeProcessor::setSrcImg(OFX::Image* p_SrcImg) { _srcImg = p_SrcImg; }

void PowerGradeProcessor::setParams(const float p_Params[kParamCount], int p_Camera, int p_Encode)
{
    std::memcpy(_params, p_Params, sizeof(float) * kParamCount);
    _camera = p_Camera;
    _encode = p_Encode;
}

void PowerGradeProcessor::setLut(const float* p_Lut, int p_LutSize, float p_LutMix)
{
    _lut = p_Lut;
    _lutSize = p_LutSize;
    _lutMix = p_LutMix;
}

////////////////////////////////////////////////////////////////////////////////

class PowerGrade : public OFX::ImageEffect
{
public:
    explicit PowerGrade(OfxImageEffectHandle p_Handle);
    virtual void render(const OFX::RenderArguments& p_Args);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);
    void setEnabledness();
    void populateLookLut();     // repopulate the Look LUT dropdown for the current group
    void setupAndProcess(PowerGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args);

private:
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;

    OFX::ChoiceParam* m_Camera;
    OFX::DoubleParam* m_Temp;
    OFX::DoubleParam* m_Tint;
    OFX::DoubleParam* m_OffTemp;
    OFX::DoubleParam* m_OffTint;
    OFX::DoubleParam* m_Density;
    OFX::DoubleParam* m_Lift;
    OFX::DoubleParam* m_Gamma;
    OFX::DoubleParam* m_Gain;
    OFX::ChoiceParam* m_Encode;
    OFX::DoubleParam* m_PostExp;
    OFX::DoubleParam* m_PostCon;

    OFX::ChoiceParam* m_LutMode;    // 0 none, 1 custom look, 2 film-look built-in
    OFX::ChoiceParam* m_FilmLut;
    OFX::ChoiceParam* m_LookGroup;
    OFX::ChoiceParam* m_LookLut;
    OFX::DoubleParam* m_LutMix;
    CubeLUT           m_Lut;        // cached loaded LUT
};

PowerGrade::PowerGrade(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_Camera  = fetchChoiceParam("camera");
    m_Temp    = fetchDoubleParam("temp");
    m_Tint    = fetchDoubleParam("tint");
    m_OffTemp = fetchDoubleParam("offTemp");
    m_OffTint = fetchDoubleParam("offTint");
    m_Density = fetchDoubleParam("density");
    m_Lift    = fetchDoubleParam("lift");
    m_Gamma   = fetchDoubleParam("gamma");
    m_Gain    = fetchDoubleParam("gain");
    m_Encode  = fetchChoiceParam("outEncode");
    m_PostExp = fetchDoubleParam("postExp");
    m_PostCon = fetchDoubleParam("postCon");

    m_LutMode  = fetchChoiceParam("lutMode");
    m_FilmLut  = fetchChoiceParam("filmLut");
    m_LookGroup= fetchChoiceParam("lookGroup");
    m_LookLut  = fetchChoiceParam("lookLut");
    m_LutMix   = fetchDoubleParam("lutMix");

    populateLookLut();
    setEnabledness();
}

// Mutually exclusive: Film Look and Custom Look can't be used together.
void PowerGrade::setEnabledness()
{
    int mode = 0;
    m_LutMode->getValue(mode);
    m_FilmLut->setEnabled(mode == 2);
    m_LookGroup->setEnabled(mode == 1);
    m_LookLut->setEnabled(mode == 1);
    m_LutMix->setEnabled(mode != 0);
}

// Rebuild the Look LUT dropdown to list only the currently selected group's LUTs.
void PowerGrade::populateLookLut()
{
    int gi = 0;
    m_LookGroup->getValue(gi);
    m_LookLut->resetOptions();
    if (gi >= 0 && gi < (int)s_LookGroups.size() && !s_LookGroups[gi].second.empty())
        for (const auto& f : s_LookGroups[gi].second) m_LookLut->appendOption(f.first);
    else
        m_LookLut->appendOption("(none)");
}

void PowerGrade::changedParam(const OFX::InstanceChangedArgs& /*p_Args*/, const std::string& p_ParamName)
{
    if (p_ParamName == "lutMode") setEnabledness();
    else if (p_ParamName == "lookGroup") { populateLookLut(); m_LookLut->setValue(0); }
}

void PowerGrade::render(const OFX::RenderArguments& p_Args)
{
    if ((m_DstClip->getPixelDepth() == OFX::eBitDepthFloat) && (m_DstClip->getPixelComponents() == OFX::ePixelComponentRGBA))
    {
        PowerGradeProcessor proc(*this);
        setupAndProcess(proc, p_Args);
    }
    else
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

void PowerGrade::setupAndProcess(PowerGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args)
{
    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));

    if ((src->getPixelDepth() != dst->getPixelDepth()) || (src->getPixelComponents() != dst->getPixelComponents()))
        OFX::throwSuiteStatusException(kOfxStatErrValue);

    int camera = 0, encode = 0, lutMode = 0;
    m_Camera->getValueAtTime(p_Args.time, camera);
    m_Encode->getValueAtTime(p_Args.time, encode);
    m_LutMode->getValueAtTime(p_Args.time, lutMode);

    // Couple the pre-LUT encoding to the LUT path so the two can't mismatch:
    //   Film Look LUTs require Cineon log input; Custom look LUTs use Rec.709.
    if (lutMode == 2)      encode = 1;   // Film Look  -> Cineon Log
    else if (lutMode == 1) encode = 0;   // Custom Look -> Rec.709 (Scene)
    // lutMode == 0 (None) -> user's Output Encode is used unchanged

    float params[kParamCount];
    params[0] = (float)m_Temp->getValueAtTime(p_Args.time);
    params[1] = (float)m_Tint->getValueAtTime(p_Args.time);
    params[2] = (float)m_Density->getValueAtTime(p_Args.time);
    params[3] = (float)m_Lift->getValueAtTime(p_Args.time);
    params[4] = (float)m_Gamma->getValueAtTime(p_Args.time);
    params[5] = (float)m_Gain->getValueAtTime(p_Args.time);
    params[6] = (float)m_OffTemp->getValueAtTime(p_Args.time);
    params[7] = (float)m_OffTint->getValueAtTime(p_Args.time);
    params[8] = (float)m_PostExp->getValueAtTime(p_Args.time);
    params[9] = (float)m_PostCon->getValueAtTime(p_Args.time);

    // Resolve the active LUT (path from mode) and load it (cached by path).
    std::string lutPath;
    if (lutMode == 1) {
        int gi = 0, li = 0;
        m_LookGroup->getValueAtTime(p_Args.time, gi);
        m_LookLut->getValueAtTime(p_Args.time, li);
        if (gi >= 0 && gi < (int)s_LookGroups.size()) {
            const LutList& luts = s_LookGroups[gi].second;
            if (li >= 0 && li < (int)luts.size()) lutPath = luts[li].second;
        }
    } else if (lutMode == 2) {
        int idx = 0; m_FilmLut->getValueAtTime(p_Args.time, idx);
        if (idx >= 0 && idx < (int)s_FilmLuts.size()) lutPath = s_FilmLuts[idx].second;
    }
    bool lutOk = (lutMode != 0) && !lutPath.empty() && m_Lut.load(lutPath);
    float lutMix = (float)m_LutMix->getValueAtTime(p_Args.time);

    p_Proc.setDstImg(dst.get());
    p_Proc.setSrcImg(src.get());
    p_Proc.setGPURenderArgs(p_Args);
    p_Proc.setRenderWindow(p_Args.renderWindow);
    p_Proc.setParams(params, camera, encode);
    p_Proc.setLut(lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0, lutOk ? lutMix : 0.0f);
    p_Proc.process();
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

PowerGradeFactory::PowerGradeFactory()
    : OFX::PluginFactoryHelper<PowerGradeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void PowerGradeFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);

    p_Desc.setSupportsOpenCLBuffersRender(true);
#ifdef OFX_SUPPORTS_CUDARENDER
    p_Desc.setSupportsCudaRender(true);
    p_Desc.setSupportsCudaStream(true);
#endif
#ifdef __APPLE__
    p_Desc.setSupportsMetalRender(true);
#endif
}

static DoubleParamDescriptor* defineSlider(OFX::ImageEffectDescriptor& p_Desc, const char* name, const char* label,
                                           const char* hint, double def, double lo, double hi, double inc,
                                           GroupParamDescriptor* parent)
{
    DoubleParamDescriptor* param = p_Desc.defineDoubleParam(name);
    param->setLabels(label, label, label);
    param->setHint(hint);
    param->setDefault(def);
    param->setRange(lo, hi);
    param->setDisplayRange(lo, hi);
    param->setIncrement(inc);
    if (parent) param->setParent(*parent);
    return param;
}

void PowerGradeFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
{
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // ---- 1. Input Transform (CST) ----
    GroupParamDescriptor* gInput = p_Desc.defineGroupParam("gInput");
    gInput->setLabels("1  Input Transform", "1  Input Transform", "1  Input Transform");
    ChoiceParamDescriptor* cam = p_Desc.defineChoiceParam("camera");
    cam->setLabels("Camera", "Camera", "Camera");
    cam->setHint("Source camera log/gamut. Decodes to DaVinci Wide Gamut linear working space (matches the CST node).");
    cam->appendOption("Blackmagic (DWG/DI)");
    cam->appendOption("Sony S-Log3");
    cam->appendOption("ARRI LogC3");
    cam->appendOption("ARRI LogC4");
    cam->appendOption("Canon Log3");
    cam->appendOption("RED Log3G10");
    cam->appendOption("DJI D-Log");
    cam->appendOption("Fuji F-Log2");
    cam->appendOption("Panasonic V-Log");
    cam->appendOption("Rec.2100 HLG (HDR)");
    cam->appendOption("Rec.2100 PQ / ST.2084 (HDR)");
    cam->setDefault(0);
    cam->setParent(*gInput);
    page->addChild(*cam);

    // ---- 2. Balance ----  (white balance in linear; watch the vectorscope while adjusting)
    GroupParamDescriptor* gBal = p_Desc.defineGroupParam("gBalance");
    gBal->setLabels("2  Balance", "2  Balance", "2  Balance");
    {
        StringParamDescriptor* tip = p_Desc.defineStringParam("balanceTip");
        tip->setLabels("Tip", "Tip", "Tip");
        tip->setStringType(eStringTypeLabel);
        tip->setDefault("Open the Vectorscope while adjusting. Offset = even balance across all tones; Gain = neutral highlights.");
        tip->setEnabled(false);
        tip->setParent(*gBal);
        page->addChild(*tip);
    }
    // Offset balance (additive) — shifts every tone's chroma evenly; best for stubborn casts.
    page->addChild(*defineSlider(p_Desc, "offTemp", "Offset Temp", "Warm (+) / cool (-) balance, additive (Offset wheel). Even across all tones.", 0.0, -1.0, 1.0, 0.001, gBal));
    page->addChild(*defineSlider(p_Desc, "offTint", "Offset Tint", "Green (+) / magenta (-) balance, additive (Offset wheel). Even across all tones.", 0.0, -1.0, 1.0, 0.001, gBal));
    // Gain balance (multiplicative) — keeps highlights neutral.
    page->addChild(*defineSlider(p_Desc, "temp", "Gain Temp", "Warm (+) / cool (-) balance, multiplicative (Gain wheel). Neutral highlights.", 0.0, -1.0, 1.0, 0.001, gBal));
    page->addChild(*defineSlider(p_Desc, "tint", "Gain Tint", "Green (+) / magenta (-) balance, multiplicative (Gain wheel). Neutral highlights.", 0.0, -1.0, 1.0, 0.001, gBal));

    // ---- 3. Density ----  (HSV saturation gain — the green-of-Gain-in-HSV trick)
    GroupParamDescriptor* gDen = p_Desc.defineGroupParam("gDensity");
    gDen->setLabels("3  Density", "3  Density", "3  Density");
    page->addChild(*defineSlider(p_Desc, "density", "Density", "Color density: saturation gain in HSV (the green-channel-of-Gain-in-HSV trick). -1 = grayscale, +1 = double saturation.", 0.0, -1.0, 1.0, 0.001, gDen));

    // ---- 4. Exposure (Lift / Gamma / Gain) ----
    GroupParamDescriptor* gExp = p_Desc.defineGroupParam("gExposure");
    gExp->setLabels("4  Exposure (Lift / Gamma / Gain)", "4  Exposure", "4  Exposure");
    page->addChild(*defineSlider(p_Desc, "lift",  "Lift",  "Raise/lower shadows (offset)", 0.0, -0.5, 0.5, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gamma", "Gamma", "Midtone brightness (power)",    1.0,  0.2, 3.0, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gain",  "Gain",  "Highlights / overall (multiply)", 1.0, 0.0, 3.0, 0.001, gExp));

    // ---- 5. Output ----
    GroupParamDescriptor* gOut = p_Desc.defineGroupParam("gOutput");
    gOut->setLabels("5  Output", "5  Output", "5  Output");
    ChoiceParamDescriptor* enc = p_Desc.defineChoiceParam("outEncode");
    enc->setLabels("Output Encode", "Output Encode", "Output Encode");
    enc->setHint("Match your project's Timeline Color Space. With the recommended setup (see Setup / Help) leave this on Rec.709 (Scene). Applies when LUT Mode = None; a LUT auto-sets it (Film Look -> Cineon, Custom Look -> Rec.709).");
    enc->appendOption("Rec.709 (Scene)");
    enc->appendOption("Cineon Log (feed film LUT)");
    enc->appendOption("DaVinci Intermediate");
    enc->appendOption("Linear");
    enc->setDefault(0);
    enc->setParent(*gOut);
    page->addChild(*enc);

    // ---- 6. Look / Film LUT ----
    scanLuts();
    GroupParamDescriptor* gLut = p_Desc.defineGroupParam("gLut");
    gLut->setLabels("6  Look / Film LUT", "6  Look / Film LUT", "6  Look / Film LUT");

    ChoiceParamDescriptor* lutMode = p_Desc.defineChoiceParam("lutMode");
    lutMode->setLabels("LUT Mode", "LUT Mode", "LUT Mode");
    lutMode->setHint("None; a Custom Look LUT (Rec.709 path); or a built-in Film Look (Cineon path). Film and Look are mutually exclusive.");
    lutMode->appendOption("None");
    lutMode->appendOption("Custom Look LUT");
    lutMode->appendOption("Film Look (built-in)");
    lutMode->setDefault(0);
    lutMode->setParent(*gLut);
    page->addChild(*lutMode);

    ChoiceParamDescriptor* filmLut = p_Desc.defineChoiceParam("filmLut");
    filmLut->setLabels("Film Look LUT", "Film Look LUT", "Film Look LUT");
    filmLut->setHint("Built-in film-look LUT (Resolve's Film Looks). Active when LUT Mode = Film Look; encodes to Cineon automatically.");
    if (s_FilmLuts.empty()) filmLut->appendOption("(no .cube LUTs found)");
    else for (const auto& fl : s_FilmLuts) filmLut->appendOption(fl.first);
    filmLut->setDefault(0);
    filmLut->setParent(*gLut);
    page->addChild(*filmLut);

    // Look LUT: two-level cascade (Group -> LUT) to tame the big master-folder list.
    ChoiceParamDescriptor* lookGroup = p_Desc.defineChoiceParam("lookGroup");
    lookGroup->setLabels("Look LUT Group", "Look LUT Group", "Look LUT Group");
    lookGroup->setHint("LUT category (top-level folder in Resolve's LUT directory). Active when LUT Mode = Custom Look.");
    if (s_LookGroups.empty()) lookGroup->appendOption("(no .cube LUTs found)");
    else for (const auto& g : s_LookGroups) lookGroup->appendOption(g.first);
    lookGroup->setDefault(0);
    lookGroup->setParent(*gLut);
    page->addChild(*lookGroup);

    ChoiceParamDescriptor* lookLut = p_Desc.defineChoiceParam("lookLut");
    lookLut->setLabels("Look LUT", "Look LUT", "Look LUT");
    lookLut->setHint("LUT within the selected group. Active when LUT Mode = Custom Look; applied on the Rec.709 path.");
    if (!s_LookGroups.empty() && !s_LookGroups[0].second.empty())
        for (const auto& f : s_LookGroups[0].second) lookLut->appendOption(f.first);   // first group; repopulated per instance
    else
        lookLut->appendOption("(none)");
    lookLut->setDefault(0);
    lookLut->setParent(*gLut);
    page->addChild(*lookLut);

    page->addChild(*defineSlider(p_Desc, "lutMix", "LUT Mix", "LUT output level / strength (like Key Output). 0 = off, 1 = full.", 1.0, 0.0, 1.0, 0.001, gLut));

    // ---- 7. Trim (after LUT) ----  final display-space trims on top of the look/LUT
    GroupParamDescriptor* gTrim = p_Desc.defineGroupParam("gTrim");
    gTrim->setLabels("7  Trim (after LUT)", "7  Trim (after LUT)", "7  Trim (after LUT)");
    page->addChild(*defineSlider(p_Desc, "postExp", "Exposure", "Post-LUT exposure trim in stops. Bring brightness back after a film-emulation LUT.", 0.0, -3.0, 3.0, 0.01, gTrim));
    page->addChild(*defineSlider(p_Desc, "postCon", "Contrast", "Post-LUT contrast trim about mid (0.5), applied after the LUT.", 1.0, 0.0, 2.0, 0.001, gTrim));

    // ---- 8. Setup / Help ----
    GroupParamDescriptor* gHelp = p_Desc.defineGroupParam("gHelp");
    gHelp->setLabels("8  Setup / Help", "8  Setup / Help", "8  Setup / Help");
    gHelp->setOpen(false);
    auto helpLine = [&](const char* name, const char* label, const char* text) {
        StringParamDescriptor* s = p_Desc.defineStringParam(name);
        s->setLabels(label, label, label);
        s->setStringType(eStringTypeLabel);
        s->setDefault(text);
        s->setEnabled(false);
        s->setParent(*gHelp);
        page->addChild(*s);
    };
    helpLine("help0", "Requires", "Project > Color Management set to (not color managed):");
    helpLine("help1", "Color Science", "DaVinci YRGB");
    helpLine("help2", "Timeline Color Space", "Rec.709 (Scene)");
    helpLine("help3", "Output Color Space", "Same as Timeline");
    helpLine("help4", "Clips", "Leave at camera raw/log defaults - no input CST or LUT before this node.");
    helpLine("help5", "Camera control", "Set it to match the source footage; this node does the input transform.");
    helpLine("help6", "Output Encode", "Leave on Rec.709 (Scene) to match the timeline above; change only if your timeline differs.");
}

ImageEffect* PowerGradeFactory::createInstance(OfxImageEffectHandle p_Handle, ContextEnum /*p_Context*/)
{
    return new PowerGrade(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static PowerGradeFactory powerGradePlugin;
    p_FactoryArray.push_back(&powerGradePlugin);
}
