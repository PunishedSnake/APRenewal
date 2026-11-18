// APRenewal - d3d9.dll proxy for Alpha Protocol (GOG 2023 build, UE3 / D3D9 SM3)
//
// Coexists with AlphaProtocolFixer (winmm.dll) and GOG dixi (dinput8/xinput1_3).
//
// Features:
//   * Chain-loads a real d3d9 (system32 or e.g. DXVK renamed to d3d9_dxvk.dll)
//   * D3DXCompileShader IAT hook: log / dump / override UE3 shaders at runtime
//   * Precise frame limiter (high-resolution waitable timer + short spin)
//   * Optional borderless-windowed and forced VSync on/off
//   * Diagnostic log of present parameters and device creation
//
// Build: see build.sh (i686-w64-mingw32-g++). 32-bit only.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <vector>
#include <string>
#include <unordered_map>
#ifndef DI_OK
#define DI_OK S_OK
#endif
#include "imgui/imgui.h"
#include "matclass.h"
#include "imgui/imgui_impl_dx9.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// --------------------------------------------------------------------------
// Globals / config
// --------------------------------------------------------------------------
static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
static char    g_dir[MAX_PATH];          // directory containing this DLL (Binaries\)
static char    g_ini[MAX_PATH];
static FILE*   g_log = nullptr;
static CRITICAL_SECTION g_logcs;
static HWND    g_hwnd = nullptr;          // game window (device window)
static bool    g_inPostFX = false;        // our own draws: excluded from the render trace
static UINT    g_ppW = 0, g_ppH = 0;      // backbuffer size from the present parameters
struct IntzEntry { IDirect3DSurface9* surf; IDirect3DTexture9* tex; };
static IntzEntry g_intzTab[4] = {};
// supersampling / HDR state
static bool    g_ssaa = false;                    // backbuffer larger than the window: downscale at Present
static UINT    g_outW = 0, g_outH = 0;            // window (output) size while supersampling
static bool    g_hdrUnlocked = false;             // DXVK allowed an fp16 backbuffer
static bool    g_hdrForced = false;               // we replaced the backbuffer format with fp16
static volatile bool g_worldEverSeen = false;
static IDirect3DTexture9* g_glassMask = nullptr;  // coverage of our window glass this frame (MRT 1)
static IDirect3DSurface9* g_glassMaskSurf = nullptr;
static int  g_scenesThisFrame = 0;               // scenes rendered so far this frame (scene captures first)
static unsigned g_frameNo = 0, g_glassMaskFrame = ~0u;   // g_glassMaskFrame = frame * 64 + scene of the mask's content
static unsigned GlassMaskKey() { return g_frameNo * 64u + (unsigned)(g_scenesThisFrame & 63); }
static bool g_glassMaskBound = false;
static IDirect3DTexture9* g_fogMask = nullptr;    // fog transmittance this frame (MRT 1 of our fog shader)
static IDirect3DSurface9* g_fogMaskSurf = nullptr;
static unsigned g_fogMaskFrame = ~0u;
static bool g_fogMaskBound = false;
// view-projection of the main view, from the game's vertex shaders (screen <-> world for reflections)
static float g_vp[4][4];
static unsigned g_vpKey = ~0u;
static float g_vsc[256][4];                        // mirror of the vertex shader float constants
static int   g_curVSvp = -1;                       // ViewProjectionMatrix register of the bound game VS
static bool  g_bloomBound = false;                 // our bloom texture is on s10 for this draw
// material mask (MRT 1 of tagged base-pass shaders): r = skin, g = metal
static IDirect3DTexture9* g_matMask = nullptr;
static IDirect3DSurface9* g_matMaskSurf = nullptr;
static unsigned g_matMaskFrame = ~0u;
static bool g_matMaskBound = false;
static long g_matTaggedDraws = 0;
// previous frame's finished HDR scene (half res) + its view-projection: glass reflections
static IDirect3DTexture9* g_prevHdr = nullptr;
static IDirect3DSurface9* g_prevHdrSurf = nullptr;
static unsigned g_prevHdrFrame = ~0u;
static float g_vpPrev[4][4];
static bool g_glassSsrBound = false;
static void GlassMaskRelease()
{
    if (g_matMaskSurf) { g_matMaskSurf->Release(); g_matMaskSurf = nullptr; }
    if (g_matMask) { g_matMask->Release(); g_matMask = nullptr; }
    g_matMaskFrame = ~0u;
    if (g_prevHdrSurf) { g_prevHdrSurf->Release(); g_prevHdrSurf = nullptr; }
    if (g_prevHdr) { g_prevHdr->Release(); g_prevHdr = nullptr; }
    g_prevHdrFrame = ~0u;
    if (g_fogMaskSurf) { g_fogMaskSurf->Release(); g_fogMaskSurf = nullptr; }
    if (g_fogMask) { g_fogMask->Release(); g_fogMask = nullptr; }
    g_fogMaskFrame = ~0u;
    if (g_glassMaskSurf) { g_glassMaskSurf->Release(); g_glassMaskSurf = nullptr; }
    if (g_glassMask) { g_glassMask->Release(); g_glassMask = nullptr; }
    g_glassMaskFrame = ~0u;
}    // a 3D scene was rendered (intro logos are over)
static char    g_wineVer[64] = "";                // non-empty = running under Wine/Proton
static bool    g_steamDeck = false;               // Steam sets SteamDeck=1 on the Deck
static bool    g_hdrActive = false;               // scRGB colour space is set: encode at Present
static bool    g_hdrPQ = false;                   // HDR10 colour space (unused since 0.9.1)
static float   g_gammaExp = 1.0f;                 // game's brightness ramp as an exponent (HDR applies it itself)
static float   g_hdrMaxNits = 0;                  // what the display reports
static D3DFORMAT g_bbLogicalFmt = D3DFMT_A8R8G8B8; // format the game asked for
static IDirect3DSurface9* g_hdrSceneSurf = nullptr; // this frame's HDR scene target (identity)
static bool    g_hdrArm = false;                  // expand highlights after the world lands in the backbuffer
static float   g_hdrK = 0;                        // exposure: display-linear / scene-linear
static int     g_hdrFrames = 0;       // depth buffers we created as INTZ textures (weak pointers)
static void IntzForget() { memset(g_intzTab, 0, sizeof g_intzTab); }

struct Config {
    char chain[MAX_PATH] = {};
    int  borderless = 0;
    int  vsync      = -1;   // -1 = leave to game, 0 = off, 1 = on
    int  fpsLimit   = 0;    // 0 = off
    int  shLog      = 1;
    int  shDump     = 0;
    int  shOverride = 1;
    int  compiler   = 0;     // our shaders: 0 auto (Windows' d3dcompiler_47, else the game's), 1 d3dcompiler_47, 2 the game's D3DX
    int  verbose    = 0;
    int  captureFloatDDS = 1;   // save float/16-bit targets losslessly as .dds
    // focus
    int  pauseOnFocusLoss = 1;
    int  confineCursor    = 1;
    int  backgroundFps    = 15;
    // post fx
    int  fxaa       = 1;
    int  sharpen    = 40;   // 0..100
    int  toggleKey  = VK_F11;
    int  beforeHud  = 1;
    // ambient occlusion
    int  aoEnable    = 1;
    int  aoIntensity = 100;   // percent
    int  aoRadius    = 40;    // world units (UE3)
    int  aoFov       = 85;    // horizontal FOV estimate, degrees
    int  aoMaxDist   = 5000;  // world units; AO fades out towards this distance
    int  aoKey       = VK_F10;
    int  aoProtect   = 50;    // highlight protection, percent
    int  aoGlass     = 100;   // % of AO removed where window glass covers the pixel
    int  aoFog       = 100;   // % of AO faded out by the game's fog
    int  fxaaQuality = 1;     // 0 = fast FXAA (console style), 1 = FXAA 3.11 quality (edge search)
    int  ssr         = 1;     // screen-space reflections on floors
    int  ssrStrength = 35;    // %
    int  ssrLength   = 1500;  // max ray length, world units
    int  ssrFloor    = 70;    // surfaces whose normal points up at least this much (% of straight up)
    int  matEnable   = 1;     // tag skin / metal materials (by shader hash) for the effects below
    int  sssStrength = 50;    // % screen-space subsurface scattering on skin
    int  sssWidth    = 100;   // scatter width, % of ~1 cm
    int  metalRefl   = 25;    // % screen-space reflections on metal (weapons, metal props)
    int  ssrRough    = 40;    // % blur of floor reflections (roughness)
    int  metalRough  = 50;    // % blur of metal reflections
    int  glassSsr    = 100;   // % screen-space reflections on glass (from the previous frame)
    int  bloomModern = 35;    // % of APRenewal's wide HDR bloom added in the final pass
    int  bloomModernThr = 100;// threshold x100 (HDR scene value)
    int  aoNear      = 30;    // AO fades in from this camera distance (world units)
    int  aoPrecise   = 1;     // read the game's depth buffer via INTZ (24-bit) instead of HDR alpha
    // in-game menu
    int  menuKey     = VK_INSERT;
    int  menuMouse   = 100;   // menu cursor speed, percent
    int  fxEnabled   = 1;     // post-processing master switch (F11) at startup
    int  aoMode      = 1;     // 0 off, 1 on, 2 debug view (F10) at startup
    // colour grading (linear light, before the UI)
    int  gradeEnable   = 1;
    int  gradeExposure = 10;     // 1/100 stop
    int  gradeContrast = 106;   // percent, S-curve pivoting on mid grey
    int  gradeVibrance = 45;    // percent extra saturation for muted colours
    int  gradeSaturation = 100; // percent, all colours
    int  gradeBlack    = 1;     // black level offset, 1/1000 of linear white
    int  gradeTemp     = 0;     // -100 cool .. +100 warm
    // supersampling: the game renders RenderScale% of the desktop, shown downscaled
    int  renderScale   = 100;
    // HDR output (needs DXVK as RealD3D9 and Windows HDR on)
    int  hdrEnable     = 0;
    int  hdrWindows    = 0;
    // game
    int  skipIntros    = 1;   // skip startup logo movies
    char skipMovies[256] = "slate_";   // comma-separated name prefixes always skipped
    int  shadowScale   = 1;   // shadow map resolution multiplier (1, 2, 4)
    int  console       = 1;   // developer console in the menu
    int  noOcclusion   = 1;   // occlusion queries always report visible (fixes late pop-in)
    int  drawDistance  = 150; // % of the game's draw / LOD distances (PlayerController.LODDistanceFactor)
    int  shadowSoft    = 0;   // shadow filter width: 0 = matches the enlarged map, 100 = game's original
    // highlight roll-off in the game's final HDR->LDR pass (replaces its hard clip)
    int  tmEnable      = 1;
    int  tmKnee        = 60;  // % of white where compression starts (linear, before the game's gamma)
    int  tmWhite       = 400; // scene value that maps to white, x100 (game's max scene colour is 4)
    int  tmHue         = 60;  // % hue preservation (0 = per channel, highlights go to white)
    int  fxBlend       = 1;   // use our version of the final pass at all (master switch)
    int  gammaMode     = 0;   // 0 game (square root), 1 sRGB, 2 gamma 2.2
    // the game's own effects, tuned through its shaders (100 = the game's look)
    int  bloomIntensity = 100;
    int  bloomThreshold = 0;  // offset x100 on the game's threshold (its range is 0..4)
    int  bloomSmooth   = 1;   // tent-filtered bloom upsample instead of one bilinear tap
    int  dofStrength   = 100;
    int  dofSmooth     = 1;
    int  fogDensity    = 100;
    int  motionBlur    = 100;
    int  edgeAA        = 100; // the game's directional edge blur (inside its motion blur pass)
    int  gameSharpen   = 100; // the game's unsharp mask (same pass)
    int  shadowFilter  = 2;   // 0 game's PCF, 1 soft (16 taps), 2 softer (32), 3 smoothest (48)
    int  shadowRadius  = 25;  // soft filter radius in 1/10 texels of the game's 2048 map
    int  charEven      = 0;   // % flattening of the character/dynamic-object lighting (dark side lifted)
    int  charFill      = 0;   // % extra ambient on characters/dynamic objects
    // window glass (the game's two glass materials, replaced; 100 = the game's look)
    int  glassEnable   = 1;
    int  glassClarity  = 65;  // % of the game's opacity
    int  glassRefl     = 160; // % reflection strength
    int  glassFresnel  = 70;  // % angle-dependent reflection (more at grazing angles)
    int  glassReflContrast = 30;  // % of the game's luminance weighting that crushes dim reflections
    int  glassBump     = 100; // % normal-map strength
    int  glassRefract  = 100; // % refraction (distortion) strength   // HDR on native Windows (never got above SDR white there)
    int  hdrPaper      = 200;   // nits for UI / SDR white
    int  hdrPeak       = 0;     // nits, 0 = what the display reports
    int  hdrHighlights = 100;   // percent highlight expansion
    int  hdrEncoding   = 1;     // scRGB only (HDR10 + fp16 backbuffer froze the game with DXVK)
    int  hdrSaturation = 100;   // percent, HDR only
    int  hdrCurve      = 0;     // 0 = sRGB like Windows shows SDR, 1 = gamma 2.2
    int  hdrReference  = 0;     // A/B: plain SDR-in-HDR exactly as Windows would show it
    int  hdrCompensate = 0;     // pre-encode for a driver/compositor that decodes the scRGB swapchain as sRGB
    // audio
    int  hrtf          = 1;     // 0 off, 1 on (headphones), 2 leave to OpenAL
    // view (replaces AlphaFixer)
    int  viewEnable    = 1;
    int  fovDeg        = 85;    // gameplay camera FOV at 16:9, degrees (AlphaFixer FOVFactor 1.20 = 85)
    int  hudLimit      = 1;     // keep HUD and menus 16:9 on wider screens
    int  aoFovAuto     = 1;     // AO follows the game's live FOV    // apply PostFX to the finished world image, before the UI is drawn
} cfg;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logcs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logcs);
}

static void PathJoin(char* out, const char* a, const char* b)
{
    snprintf(out, MAX_PATH, "%s\\%s", a, b);
}

static void LoadConfig()
{
    PathJoin(g_ini, g_dir, "APRenewal.ini");
    GetPrivateProfileStringA("Chain", "RealD3D9", "", cfg.chain, MAX_PATH, g_ini);
    cfg.borderless = GetPrivateProfileIntA("Display", "Borderless", 0, g_ini);
    cfg.vsync      = (int)GetPrivateProfileIntA("Display", "VSync", -1, g_ini);
    cfg.fpsLimit   = GetPrivateProfileIntA("Display", "FPSLimit", 0, g_ini);
    cfg.shLog      = GetPrivateProfileIntA("Shaders", "LogCompiles", 1, g_ini);
    cfg.shDump     = GetPrivateProfileIntA("Shaders", "DumpSource", 0, g_ini);
    cfg.shOverride = GetPrivateProfileIntA("Shaders", "Override", 1, g_ini);
    {
        char c[32] = "";
        GetPrivateProfileStringA("Shaders", "Compiler", "auto", c, sizeof c, g_ini);
        cfg.compiler = !_stricmp(c, "game") ? 2 : (!_stricmp(c, "47") || !_stricmp(c, "new")) ? 1 : 0;
    }
    cfg.verbose    = GetPrivateProfileIntA("Debug", "Verbose", 0, g_ini);
    cfg.captureFloatDDS = GetPrivateProfileIntA("Debug", "CaptureFloatDDS", 1, g_ini);
    cfg.pauseOnFocusLoss = GetPrivateProfileIntA("Focus", "PauseOnFocusLoss", 1, g_ini);
    cfg.confineCursor    = GetPrivateProfileIntA("Focus", "ConfineCursor", 1, g_ini);
    cfg.fxaa       = GetPrivateProfileIntA("PostFX", "FXAA", 1, g_ini);
    cfg.sharpen    = GetPrivateProfileIntA("PostFX", "Sharpen", 40, g_ini);
    cfg.toggleKey  = GetPrivateProfileIntA("PostFX", "ToggleKey", VK_F11, g_ini);
    cfg.beforeHud  = GetPrivateProfileIntA("PostFX", "BeforeHUD", 1, g_ini);
    cfg.aoEnable    = GetPrivateProfileIntA("AO", "Enable", 1, g_ini);
    cfg.aoIntensity = GetPrivateProfileIntA("AO", "Intensity", 100, g_ini);
    cfg.aoRadius    = GetPrivateProfileIntA("AO", "Radius", 40, g_ini);
    cfg.aoFov       = GetPrivateProfileIntA("AO", "FOV", 85, g_ini);
    cfg.aoMaxDist   = GetPrivateProfileIntA("AO", "MaxDistance", 5000, g_ini);
    cfg.aoKey       = GetPrivateProfileIntA("AO", "ToggleKey", VK_F10, g_ini);
    cfg.aoProtect   = GetPrivateProfileIntA("AO", "ProtectHighlights", 50, g_ini);
    cfg.aoGlass     = GetPrivateProfileIntA("AO", "BehindGlass", 100, g_ini);
    cfg.aoFog       = GetPrivateProfileIntA("AO", "FadeWithFog", 100, g_ini);
    cfg.fxaaQuality = GetPrivateProfileIntA("PostFX", "FXAAQuality", 1, g_ini) ? 1 : 0;
    cfg.ssr         = GetPrivateProfileIntA("Reflections", "Enable", 1, g_ini) ? 1 : 0;
    cfg.ssrStrength = GetPrivateProfileIntA("Reflections", "Strength", 35, g_ini);
    cfg.ssrLength   = GetPrivateProfileIntA("Reflections", "Length", 1500, g_ini);
    cfg.ssrFloor    = GetPrivateProfileIntA("Reflections", "FloorsOnly", 70, g_ini);
    cfg.matEnable   = GetPrivateProfileIntA("Materials", "Enable", 1, g_ini) ? 1 : 0;
    cfg.sssStrength = GetPrivateProfileIntA("Materials", "SkinScatter", 50, g_ini);
    cfg.sssWidth    = GetPrivateProfileIntA("Materials", "SkinScatterWidth", 100, g_ini);
    cfg.metalRefl   = GetPrivateProfileIntA("Materials", "MetalReflections", 25, g_ini);
    cfg.metalRough  = GetPrivateProfileIntA("Materials", "MetalRoughness", 50, g_ini);
    cfg.ssrRough    = GetPrivateProfileIntA("Reflections", "Roughness", 40, g_ini);
    cfg.glassSsr    = GetPrivateProfileIntA("Glass", "ScreenReflections", 100, g_ini);
    cfg.bloomModern = GetPrivateProfileIntA("Effects", "ModernBloom", 35, g_ini);
    cfg.bloomModernThr = GetPrivateProfileIntA("Effects", "ModernBloomThreshold", 100, g_ini);
    cfg.aoNear      = GetPrivateProfileIntA("AO", "NearFade", 30, g_ini);
    cfg.aoPrecise   = GetPrivateProfileIntA("AO", "PreciseDepth", 1, g_ini);
    cfg.menuKey     = GetPrivateProfileIntA("Menu", "Key", VK_INSERT, g_ini);
    cfg.menuMouse   = GetPrivateProfileIntA("Menu", "MouseSpeed", 100, g_ini);
    cfg.fxEnabled   = GetPrivateProfileIntA("PostFX", "Enabled", 1, g_ini);
    cfg.aoMode      = GetPrivateProfileIntA("AO", "Mode", 1, g_ini);
    if (cfg.aoMode < 0 || cfg.aoMode > 2) cfg.aoMode = 1;
    cfg.gradeEnable   = GetPrivateProfileIntA("Grade", "Enable", 1, g_ini);
    cfg.gradeExposure = (int)GetPrivateProfileIntA("Grade", "Exposure", 10, g_ini);
    cfg.gradeContrast = GetPrivateProfileIntA("Grade", "Contrast", 106, g_ini);
    cfg.gradeVibrance = (int)GetPrivateProfileIntA("Grade", "Vibrance", 45, g_ini);
    cfg.gradeSaturation = GetPrivateProfileIntA("Grade", "Saturation", 100, g_ini);
    cfg.gradeBlack    = (int)GetPrivateProfileIntA("Grade", "BlackLevel", 1, g_ini);
    cfg.gradeTemp     = (int)GetPrivateProfileIntA("Grade", "Temperature", 0, g_ini);
    cfg.renderScale   = GetPrivateProfileIntA("Display", "RenderScale", 100, g_ini);
    if (cfg.renderScale < 100) cfg.renderScale = 100;
    if (cfg.renderScale > 200) cfg.renderScale = 200;
    cfg.hdrEnable     = GetPrivateProfileIntA("HDR", "Enable", 0, g_ini);
    cfg.hdrWindows    = GetPrivateProfileIntA("HDR", "AllowWindows", 0, g_ini) ? 1 : 0;
    cfg.skipIntros    = GetPrivateProfileIntA("Game", "SkipIntros", 1, g_ini) ? 1 : 0;
    GetPrivateProfileStringA("Game", "SkipMovies", "slate_", cfg.skipMovies, sizeof cfg.skipMovies, g_ini);
    cfg.console       = GetPrivateProfileIntA("Game", "Console", 1, g_ini) ? 1 : 0;
    cfg.noOcclusion   = GetPrivateProfileIntA("Game", "NoOcclusionCulling", 1, g_ini) ? 1 : 0;
    cfg.drawDistance  = GetPrivateProfileIntA("Game", "DrawDistance", 150, g_ini);
    cfg.shadowScale   = GetPrivateProfileIntA("Shadows", "Scale", 1, g_ini);
    if (cfg.shadowScale != 2 && cfg.shadowScale != 4) cfg.shadowScale = 1;
    cfg.shadowSoft    = GetPrivateProfileIntA("Shadows", "Softness", 0, g_ini);
    cfg.tmEnable      = GetPrivateProfileIntA("Tonemap", "Enable", 1, g_ini) ? 1 : 0;
    cfg.tmKnee        = GetPrivateProfileIntA("Tonemap", "Knee", 60, g_ini);
    cfg.tmWhite       = GetPrivateProfileIntA("Tonemap", "White", 400, g_ini);
    cfg.tmHue         = GetPrivateProfileIntA("Tonemap", "HuePreserve", 60, g_ini);
    cfg.fxBlend       = GetPrivateProfileIntA("Tonemap", "ReplaceFinalPass", 1, g_ini) ? 1 : 0;
    cfg.gammaMode     = GetPrivateProfileIntA("Tonemap", "Gamma", 0, g_ini);
    if (cfg.gammaMode < 0 || cfg.gammaMode > 2) cfg.gammaMode = 0;
    cfg.bloomIntensity = GetPrivateProfileIntA("Effects", "BloomIntensity", 100, g_ini);
    cfg.bloomThreshold = GetPrivateProfileIntA("Effects", "BloomThreshold", 0, g_ini);
    cfg.bloomSmooth   = GetPrivateProfileIntA("Effects", "BloomSmooth", 1, g_ini) ? 1 : 0;
    cfg.dofStrength   = GetPrivateProfileIntA("Effects", "DepthOfField", 100, g_ini);
    cfg.dofSmooth     = GetPrivateProfileIntA("Effects", "DepthOfFieldSmooth", 1, g_ini) ? 1 : 0;
    cfg.fogDensity    = GetPrivateProfileIntA("Effects", "FogDensity", 100, g_ini);
    cfg.motionBlur    = GetPrivateProfileIntA("Effects", "MotionBlur", 100, g_ini);
    cfg.edgeAA        = GetPrivateProfileIntA("Effects", "GameEdgeAA", 100, g_ini);
    cfg.gameSharpen   = GetPrivateProfileIntA("Effects", "GameSharpen", 100, g_ini);
    cfg.shadowFilter  = GetPrivateProfileIntA("Shadows", "Filter", 2, g_ini);
    if (cfg.shadowFilter < 0 || cfg.shadowFilter > 3) cfg.shadowFilter = 2;
    cfg.shadowRadius  = GetPrivateProfileIntA("Shadows", "Radius", 25, g_ini);
    cfg.charEven      = GetPrivateProfileIntA("Lighting", "CharacterEven", 0, g_ini);
    cfg.charFill      = GetPrivateProfileIntA("Lighting", "CharacterFill", 0, g_ini);
    cfg.glassEnable   = GetPrivateProfileIntA("Glass", "Enable", 1, g_ini) ? 1 : 0;
    cfg.glassClarity  = GetPrivateProfileIntA("Glass", "Opacity", 65, g_ini);
    cfg.glassRefl     = GetPrivateProfileIntA("Glass", "Reflection", 160, g_ini);
    cfg.glassFresnel  = GetPrivateProfileIntA("Glass", "Fresnel", 70, g_ini);
    cfg.glassReflContrast = GetPrivateProfileIntA("Glass", "ReflectionContrast", 30, g_ini);
    cfg.glassBump     = GetPrivateProfileIntA("Glass", "Bump", 100, g_ini);
    cfg.glassRefract  = GetPrivateProfileIntA("Glass", "Refraction", 100, g_ini);
    cfg.hdrPaper      = GetPrivateProfileIntA("HDR", "PaperWhite", 0, g_ini);   // 0 = Windows' SDR content brightness
    cfg.hdrCurve      = GetPrivateProfileIntA("HDR", "Curve", 0, g_ini) ? 1 : 0;
    cfg.hdrCompensate = GetPrivateProfileIntA("HDR", "CompensateSRGB", 0, g_ini) ? 1 : 0;
    cfg.hdrPeak       = GetPrivateProfileIntA("HDR", "PeakNits", 0, g_ini);
    cfg.hdrHighlights = GetPrivateProfileIntA("HDR", "Highlights", 100, g_ini);
    cfg.hdrEncoding   = 1;
    cfg.hdrSaturation = GetPrivateProfileIntA("HDR", "Saturation", 100, g_ini);
    if (cfg.hdrPaper != 0 && cfg.hdrPaper < 80) cfg.hdrPaper = 80;
    cfg.hrtf          = GetPrivateProfileIntA("Audio", "HRTF", 1, g_ini);
    {
        // first run after AlphaFixer: AlphaFixer set every camera to 2*atan(tan(37.5 deg) * FOVFactor)
        int def = 85;
        char af[MAX_PATH]; PathJoin(af, g_dir, "AlphaFixer.ini");
        char v[32] = "";
        if (GetPrivateProfileStringA("Settings", "FOVFactor", "", v, sizeof v, af) && v[0])
            def = (int)(2.0 * atan(tan(37.5 * 3.14159265 / 180.0) * atof(v)) * 180.0 / 3.14159265 + 0.5);
        cfg.fovDeg = GetPrivateProfileIntA("View", "FOV", def, g_ini);
    }
    if (cfg.fovDeg < 40) cfg.fovDeg = 40;
    if (cfg.fovDeg > 130) cfg.fovDeg = 130;
    cfg.viewEnable = GetPrivateProfileIntA("View", "Enable", 1, g_ini);
    cfg.hudLimit   = GetPrivateProfileIntA("View", "HUD169", 1, g_ini);
    cfg.aoFovAuto  = GetPrivateProfileIntA("AO", "FOVAuto", 1, g_ini);
    if (cfg.hrtf < 0 || cfg.hrtf > 2) cfg.hrtf = 1;
    if (cfg.aoFov < 30) cfg.aoFov = 30;
    if (cfg.aoFov > 150) cfg.aoFov = 150;
    if (cfg.aoRadius < 1) cfg.aoRadius = 1;
    if (cfg.sharpen < 0) cfg.sharpen = 0;
    if (cfg.sharpen > 100) cfg.sharpen = 100;
}

// --------------------------------------------------------------------------
// Utility: vtable + IAT patching
// --------------------------------------------------------------------------
static void* PatchVtbl(void* obj, int index, void* hook)
{
    void** vtbl = *(void***)obj;
    void* orig = vtbl[index];
    if (orig == hook) return nullptr; // already patched
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return orig;
}

static void** FindIAT(HMODULE mod, const char* dll, const char* func)
{
    auto base = (uint8_t*)mod;
    auto dos  = (IMAGE_DOS_HEADER*)base;
    auto nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat   = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) == 0) return (void**)&iat->u1.Function;
        }
    }
    return nullptr;
}

static void* PatchIAT(void** slot, void* hook)
{
    void* orig = *slot;
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}

// --------------------------------------------------------------------------
// View: FOV and HUD aspect (replaces AlphaFixer / winmm.dll)
//
// AlphaFixer overwrote every camera module's FOV with one fixed value computed at startup from
// AlphaFixer.ini (or the desktop resolution), and forced the HUD/menu width to that same
// resolution. That flattens zoom/aim FOV changes and breaks as soon as the render size differs
// from the desktop (supersampling). Here the game's own values are scaled live, per module, from
// the size the game is actually rendering at, and the FOV factor can change from the menu.
// --------------------------------------------------------------------------
struct HookRegs { DWORD edi, esi, ebp, esp, ebx, edx, ecx, eax; };   // pushad layout
typedef void (__cdecl *HookFn)(HookRegs*);

static BYTE* FindPattern(const char* pat)
{
    HMODULE exe = GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* start = nullptr; size_t size = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 5)) { start = (BYTE*)exe + sec[i].VirtualAddress; size = sec[i].Misc.VirtualSize; }
    if (!start) return nullptr;
    int bytes[64], n = 0;
    for (const char* p = pat; *p && n < 64; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { bytes[n++] = -1; while (*p == '?') p++; }
        else { bytes[n++] = (int)strtoul(p, (char**)&p, 16); }
    }
    for (size_t i = 0; i + n <= size; i++) {
        int k = 0;
        while (k < n && (bytes[k] < 0 || start[i + k] == bytes[k])) k++;
        if (k == n) return start + i;
    }
    return nullptr;
}

// Mid-function hook: `len` bytes at `site` (whole instructions, no relative operands) are replaced
// by a jump to a stub that saves flags, general and XMM registers, calls fn(&regs) (fn may edit the
// registers), restores everything, runs `repl` (or the original bytes) and jumps back.
static bool MidHook(BYTE* site, size_t len, const BYTE* expect, HookFn fn, const BYTE* repl = nullptr, size_t replLen = 0)
{
    if (!site || len < 5 || memcmp(site, expect, len) != 0) return false;
    static BYTE* pool = nullptr; static size_t used = 0;
    if (!pool) pool = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pool || used + 160 > 4096) return false;
    BYTE* s = pool + used; BYTE* p = s;
    auto put = [&](std::initializer_list<int> b) { for (int x : b) *p++ = (BYTE)x; };
    auto rel = [&](BYTE op, const void* to) { *p++ = op; *(int32_t*)p = (int32_t)((BYTE*)to - (p + 4)); p += 4; };
    put({ 0x9C, 0x60 });                                   // pushfd; pushad
    put({ 0x81, 0xEC, 0x80, 0, 0, 0 });                    // sub esp, 0x80
    for (int i = 0; i < 8; i++) put({ 0xF3, 0x0F, 0x7F, 0x44 | (i << 3), 0x24, i * 16 });   // movdqu [esp+i*16], xmmi
    put({ 0x8D, 0x84, 0x24, 0x80, 0, 0, 0, 0x50 });        // lea eax, [esp+0x80]; push eax
    rel(0xE8, (void*)fn);                                  // call fn
    put({ 0x83, 0xC4, 0x04 });                             // add esp, 4
    for (int i = 0; i < 8; i++) put({ 0xF3, 0x0F, 0x6F, 0x44 | (i << 3), 0x24, i * 16 });   // movdqu xmmi, [esp+i*16]
    put({ 0x81, 0xC4, 0x80, 0, 0, 0, 0x61, 0x9D });        // add esp, 0x80; popad; popfd
    if (repl) { memcpy(p, repl, replLen); p += replLen; } else { memcpy(p, site, len); p += len; }
    rel(0xE9, site + len);                                 // jmp back
    used += (size_t)(p - s + 15) & ~(size_t)15;
    DWORD old;
    VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9; *(int32_t*)(site + 1) = (int32_t)(s - (site + 5));
    for (size_t i = 5; i < len; i++) site[i] = 0x90;
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

static float g_fovScratch = 0;          // the scaled module FOV the game's mulss reads instead of its own
static volatile float g_gameFov = 0;    // last blended camera FOV (horizontal, degrees) - AO uses it
static volatile DWORD g_gameFovTick = 0;
static bool  g_viewHooks = false;
static int   g_viewSites = 0;

static float ViewAspect()
{
    return (g_ppW && g_ppH) ? (float)g_ppW / (float)g_ppH : 16.0f / 9.0f;
}

// The game's normal third-person camera is 60 degrees (measured: the module FOV the blend sees).
// The user picks what that camera should become at 16:9; every camera mode is scaled by the same
// tangent ratio, so aim/zoom keep their relation to it, and wider screens widen further (Hor+).
static const float kGameBaseFov = 60.0f;
static float ScaleFov(float deg)
{
    if (!(deg > 1.0f && deg < 170.0f)) return deg;
    const float d2r = 3.14159265f / 180.0f;
    const float k = tanf(cfg.fovDeg * 0.5f * d2r) / tanf(kGameBaseFov * 0.5f * d2r) * (ViewAspect() / (16.0f / 9.0f));
    if (k > 0.999f && k < 1.001f) return deg;
    float r = 2.0f * atanf(tanf(deg * 0.5f * 3.14159265f / 180.0f) * k) * 180.0f / 3.14159265f;
    return r < 170.0f ? r : 170.0f;
}

// at `mulss xmm0, [eax+0x10C]` inside the camera-module FOV blend: feed it the scaled module FOV
static void __cdecl View_ModuleFov(HookRegs* r)
{
    const float f = *(const float*)(r->eax + 0x10C);
    g_fovScratch = cfg.viewEnable ? ScaleFov(f) : f;
    static float seen[8]; static int nSeen = 0;          // log the distinct camera FOVs the game uses
    bool known = false;
    for (int i = 0; i < nSeen; i++) if (fabsf(seen[i] - f) < 0.05f) known = true;
    if (!known && nSeen < 8) { seen[nSeen++] = f; Log("view: camera mode FOV %.1f deg -> %.1f deg", f, g_fovScratch); }
}
// at the blend's `fld [esp+8]` return: remember the final FOV
static void __cdecl View_BlendResult(HookRegs* r)
{
    const float f = *(const float*)(r->esp + 4 + 8);
    if (f > 10.0f && f < 170.0f) { g_gameFov = f; g_gameFovTick = GetTickCount(); }
}
// HUD canvas setup: ecx = width, eax = height, esi = left. Wider than 16:9 -> centred 16:9 HUD.
static void __cdecl View_Hud(HookRegs* r)
{
    if (!cfg.viewEnable || !cfg.hudLimit) return;
    const int w = (int)r->ecx, h = (int)r->eax;
    if (w <= 0 || h <= 0 || w * 9 <= h * 16 + 16) return;
    const int nw = (h * 16 + 8) / 9;
    r->esi += (w - nw) / 2;
    r->ecx = (DWORD)nw;
}
// Menu viewport size query: eax = width just returned by the viewport
static void __cdecl View_Menu(HookRegs* r)
{
    if (!cfg.viewEnable || !cfg.hudLimit) return;
    const int w = (int)r->eax, h = (int)g_ppH;
    if (w <= 0 || h <= 0 || w * 9 <= h * 16 + 16) return;
    r->eax = (DWORD)((h * 16 + 8) / 9);
}

static void InstallViewHooks()
{
    char path[MAX_PATH] = "";
    if (HMODULE wm = GetModuleHandleA("winmm.dll")) {
        GetModuleFileNameA(wm, path, MAX_PATH);
        if (!_strnicmp(path, g_dir, strlen(g_dir))) {
            Log("view: AlphaFixer (%s) is still installed - FOV/HUD handling left to it. Delete winmm.dll "
                "and AlphaFixer.ini to use APRenewal's.", path);
            return;
        }
    }
    // FOV: per camera module, non-destructive
    static const BYTE fovOrig[8] = { 0xF3, 0x0F, 0x59, 0x80, 0x0C, 0x01, 0x00, 0x00 };   // mulss xmm0,[eax+10Ch]
    BYTE* fov = FindPattern("F3 0F 59 80 0C 01 00 00 09 98 10 01 00 00");
    BYTE repl[8] = { 0xF3, 0x0F, 0x59, 0x05 };                                            // mulss xmm0,[g_fovScratch]
    *(float**)(repl + 4) = &g_fovScratch;
    if (MidHook(fov, 8, fovOrig, View_ModuleFov, repl, 8)) g_viewSites |= 1;
    // FOV: blend result (same function, after the loop: fld [esp+8]; pop esi)
    static const BYTE resOrig[5] = { 0xD9, 0x44, 0x24, 0x08, 0x5E };
    if (fov) {
        BYTE* res = nullptr;
        for (BYTE* q = fov; q < fov + 0x60; q++) if (!memcmp(q, resOrig, 5)) { res = q; break; }
        if (MidHook(res, 5, resOrig, View_BlendResult)) g_viewSites |= 2;
    }
    // HUD canvas
    static const BYTE hudOrig[6] = { 0x89, 0x4B, 0x64, 0x89, 0x43, 0x68 };
    BYTE* hud = FindPattern("89 53 70 8B 15 ? ? ? ? 89 4B 64 89 43 68 8B 52 50");
    if (MidHook(hud ? hud + 9 : nullptr, 6, hudOrig, View_Hud)) g_viewSites |= 4;
    // menu viewport
    static const BYTE menuOrig[6] = { 0x85, 0xC0, 0x89, 0x44, 0x24, 0x08 };
    BYTE* menu = FindPattern("FF D2 85 C0 89 44 24 08 DB 44 24 08");
    if (MidHook(menu ? menu + 2 : nullptr, 6, menuOrig, View_Menu)) g_viewSites |= 8;
    g_viewHooks = g_viewSites != 0;
    Log("view: hooks - module FOV %s, FOV readback %s, HUD %s, menu %s (FOV %d deg at 16:9)",
        (g_viewSites & 1) ? "ok" : "NOT FOUND", (g_viewSites & 2) ? "ok" : "NOT FOUND",
        (g_viewSites & 4) ? "ok" : "NOT FOUND", (g_viewSites & 8) ? "ok" : "NOT FOUND", cfg.fovDeg);
}

// --------------------------------------------------------------------------
// Game module: intro skip and developer console (Steam APGame.exe addresses, verified before use)
// --------------------------------------------------------------------------
static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}
static bool CodeIs(DWORD addr, const char* bytes, size_t n) { return Readable((void*)addr, n) && !memcmp((void*)addr, bytes, n); }

// The full-screen movie player's GameThreadPlayMovie(mode, name, startFrame) (vtable slot at 0x12173B0).
// Startup logos play before the first 3D scene; with SkipIntros they are refused. In-game movies
// (news reports etc.) come later and play normally. UE3's "-nomovie" would kill those too.
typedef int (__thiscall *PFN_PlayMovie)(void*, int, const wchar_t*, int);
static PFN_PlayMovie o_PlayMovie = nullptr;
static int __thiscall hk_PlayMovie(void* self, int mode, const wchar_t* name, int start)
{
    char n[160] = "";
    if (name) WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof n, nullptr, nullptr);
    // The logos (slate_sega / slate_obsidian / slate_ap) start right after the main menu's 3D
    // scene, so they are matched by name; anything before the first 3D scene is skipped too.
    bool listed = false;
    {
        char list[256]; snprintf(list, sizeof list, "%s", cfg.skipMovies);
        for (char* t = strtok(list, ", "); t && !listed; t = strtok(nullptr, ", "))
            if (*t && !_strnicmp(n, t, strlen(t))) listed = true;
    }
    if (cfg.skipIntros && (listed || !g_worldEverSeen)) {
        Log("movie: '%s' (mode %d) skipped - %s", n, mode, listed ? "intro logo" : "before the first 3D scene");
        return 0;
    }
    Log("movie: '%s' (mode %d) playing", n, mode);
    return o_PlayMovie(self, mode, name, start);
}
static void InstallMovieHook()
{
    const DWORD slot = 0x12173B0, fn = 0x4E49F0;
    if (!Readable((void*)slot, 4) || *(DWORD*)slot != fn ||
        !CodeIs(fn, "\x55\x8B\xEC\x83\xE4\xF8\x6A\xFF", 8) || !CodeIs(0x4E4A68, "\x68\xE0\x9E\x1D\x01", 5)) {
        Log("movie: movie player not found (different exe build) - intro skip unavailable");
        return;
    }
    o_PlayMovie = (PFN_PlayMovie)fn;
    DWORD old;
    VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
    *(void**)slot = (void*)hk_PlayMovie;
    VirtualProtect((void*)slot, 4, old, &old);
    Log("movie: hook ok - startup movies %s", cfg.skipIntros ? "will be skipped" : "play (SkipIntros=0)");
}

// Developer console: APlayerController::ConsoleCommand (vtable +0x110) is what script's
// ConsoleCommand() calls. The player controller pointer comes from AAPPlayerController natives
// the game calls on it; commands run on the main thread between frames (our PeekMessageW hook).
struct FStr { wchar_t* data; int num; int max; };
typedef FStr* (__thiscall *PFN_ConsoleCmd)(void* self, FStr* ret, const FStr* cmd, int writeToLog);
static void* volatile g_pc = nullptr;
static void* g_pcVtbl = nullptr;
static volatile DWORD g_pcTick = 0;
static bool g_conOk = false;
static CRITICAL_SECTION g_conCs;
static std::vector<std::string> g_conQueue;      // commands waiting for the main thread
static volatile bool g_conQueued = false;
static std::vector<std::string> g_conHistory;
static void ConAddLine(const std::string& l)
{
    EnterCriticalSection(&g_conCs);
    g_conHistory.push_back(l);
    if (g_conHistory.size() > 200) g_conHistory.erase(g_conHistory.begin());
    LeaveCriticalSection(&g_conCs);
}
static void ConQueue(const char* cmd, bool echo = true);
// Draw distance: UE3 culls and picks mesh LODs by distance x the view's LODDistanceFactor, which comes
// from PlayerController.LODDistanceFactor (1 = the game's distances; 0.5 = twice as far).
static void ApplyDrawDistance()
{
    if (!cfg.console) return;
    const int pct = std::min(std::max(cfg.drawDistance, 50), 400);
    char c[96]; snprintf(c, sizeof c, "set PlayerController LODDistanceFactor %.4f", 100.0f / pct);
    ConQueue(c, false);
}
static void __cdecl Con_CapturePC(HookRegs* r)
{
    void* pc = (void*)r->ecx;
    if (!pc) return;
    void* vt = *(void**)pc;
    if (!g_pcVtbl) g_pcVtbl = vt;
    if (vt != g_pcVtbl) return;
    const bool fresh = g_pc != pc;
    if (fresh) Log("console: player controller %p (vtable %p)", pc, vt);
    g_pc = pc; g_pcTick = GetTickCount();
    if (fresh) ApplyDrawDistance();    // a new level / controller starts with the default factor
}
static void ConRunOne(const char* cmd);
static void ConRunQueued()
{
    if (!g_conQueued) return;
    std::vector<std::string> q;
    EnterCriticalSection(&g_conCs);
    q.swap(g_conQueue); g_conQueued = false;
    LeaveCriticalSection(&g_conCs);
    for (auto& c : q) ConRunOne(c.c_str());
}
static void ConQueue(const char* cmd, bool echo)
{
    if (echo) ConAddLine(std::string("> ") + cmd);
    EnterCriticalSection(&g_conCs);
    g_conQueue.push_back(cmd);
    g_conQueued = true;
    LeaveCriticalSection(&g_conCs);
}
static void ConRunOne(const char* cmd)
{
    void* pc = g_pc;
    if (!pc || GetTickCount() - g_pcTick > 3000 || !Readable(pc, 4) || *(void**)pc != g_pcVtbl) {
        ConAddLine("  (no player yet - load a save or start a game first)");
        Log("console: '%s' - no live player controller", cmd);
        return;
    }
    wchar_t w[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, w, 512);
    if (n <= 1) return;
    FStr in = { w, n, n }, out = { nullptr, 0, 0 };
    PFN_ConsoleCmd fn = (PFN_ConsoleCmd)((*(void***)pc)[0x110 / 4]);
    Log("console: > %s", cmd);
    fn(pc, &out, &in, 1);
    if (out.data && out.num > 1) {
        char r[1024] = "";
        WideCharToMultiByte(CP_UTF8, 0, out.data, -1, r, sizeof r, nullptr, nullptr);
        ConAddLine(std::string("  ") + r);
        Log("console:   %s", r);
    } else ConAddLine("  (done)");
    // the returned string was allocated by the game's GMalloc - give it back
    void* gm = *(void**)0x16E8030;
    if (out.data && gm) ((void (__thiscall*)(void*, void*))((*(void***)gm)[3]))(gm, out.data);
}
static void InstallConsole()
{
    InitializeCriticalSection(&g_conCs);
    if (!cfg.console) { Log("console: off (Console=0)"); return; }
    // ConsoleCommand thunk: vtable +0x110 call, and GMalloc->Free at +0xC
    if (!CodeIs(0x4DB7E7, "\x8B\x17\x8B\x92\x10\x01\x00\x00", 8) || !CodeIs(0x4DB821, "\x8B\x0D\x30\x80\x6E\x01", 6) ||
        !CodeIs(0x4DB838, "\x8B\x01\x8B\x50\x0C", 5)) {
        Log("console: ConsoleCommand not found (different exe build) - console unavailable");
        return;
    }
    static const BYTE thunk[8] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0x40, 0x0C, 0x01 };   // mov eax,[esp+4]; add [eax+0Ch],1
    static const BYTE ai[5] = { 0x51, 0x0F, 0x57, 0xC0, 0x56 };                        // push ecx; xorps xmm0,xmm0; push esi
    int n = 0;
    // AAPPlayerController-only thunks (InPDAMenu is shared with the AI controller - not usable):
    // ShouldAllowPauseMenu, ShouldAllowPDAMenu, IsPlayerPastTitleScreen, UpdateAIData
    for (DWORD a : { 0xA50230u, 0xA50270u, 0xA50490u })
        if (MidHook((BYTE*)a, 8, thunk, Con_CapturePC)) n++;
    if (MidHook((BYTE*)0xA50970, 5, ai, Con_CapturePC)) n++;
    g_conOk = n > 0;
    Log("console: %d/4 player-controller probes installed%s", n, g_conOk ? "" : " - console unavailable");
}

// --------------------------------------------------------------------------
// Shader compile hook (D3DXCompileShader from d3dx9_42.dll)
// --------------------------------------------------------------------------
struct D3DXMACRO_ { LPCSTR Name; LPCSTR Definition; };
typedef HRESULT (WINAPI *PFN_D3DXCompileShader)(LPCSTR pSrcData, UINT SrcDataLen,
    const D3DXMACRO_* pDefines, void* pInclude, LPCSTR pFunctionName, LPCSTR pProfile,
    DWORD Flags, void** ppShader, void** ppErrorMsgs, void** ppConstantTable);

static PFN_D3DXCompileShader o_D3DXCompileShader = nullptr;
static volatile LONG g_compileCount = 0;

static uint64_t Fnv1a(uint64_t h, const void* data, size_t n)
{
    auto p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}
static uint64_t FnvStr(uint64_t h, const char* s) { return s ? Fnv1a(h, s, strlen(s) + 1) : Fnv1a(h, "", 1); }

static char* ReadWholeFile(const char* path, UINT* outLen)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(n + 1);
    if (buf && fread(buf, 1, n, f) == (size_t)n) { buf[n] = 0; *outLen = (UINT)n; }
    else { free(buf); buf = nullptr; }
    fclose(f);
    return buf;
}

static HRESULT WINAPI hk_D3DXCompileShader(LPCSTR src, UINT len, const D3DXMACRO_* defs, void* inc,
    LPCSTR entry, LPCSTR profile, DWORD flags, void** ppShader, void** ppErr, void** ppCT)
{
    LONG n = InterlockedIncrement(&g_compileCount);

    uint64_t h = 0xcbf29ce484222325ULL;
    h = Fnv1a(h, src, len);
    h = FnvStr(h, entry);
    h = FnvStr(h, profile);
    for (auto d = defs; d && d->Name; ++d) { h = FnvStr(h, d->Name); h = FnvStr(h, d->Definition); }

    char hashName[32];
    snprintf(hashName, sizeof hashName, "%016llx", (unsigned long long)h);

    if (cfg.shLog && (n <= 50 || cfg.verbose || (n % 500) == 0))
        Log("compile #%ld %s entry=%s profile=%s len=%u flags=0x%lx", n, hashName,
            entry ? entry : "?", profile ? profile : "?", len, flags);

    if (cfg.shDump) {
        char dir[MAX_PATH], path[MAX_PATH];
        PathJoin(dir, g_dir, "APRenewal\\dump");
        snprintf(path, MAX_PATH, "%s\\%s.hlsl", dir, hashName);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            if (FILE* f = fopen(path, "wb")) {
                fprintf(f, "// APRenewal dump  entry=%s  profile=%s  flags=0x%lx\n",
                        entry ? entry : "?", profile ? profile : "?", flags);
                for (auto d = defs; d && d->Name; ++d)
                    fprintf(f, "// #define %s %s\n", d->Name, d->Definition ? d->Definition : "");
                fwrite(src, 1, len, f);
                fclose(f);
            }
        }
    }

    if (cfg.shOverride) {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\APRenewal\\override\\%s.hlsl", g_dir, hashName);
        UINT olen = 0;
        if (char* osrc = ReadWholeFile(path, &olen)) {
            HRESULT hr = o_D3DXCompileShader(osrc, olen, defs, inc, entry, profile, flags, ppShader, ppErr, ppCT);
            free(osrc);
            if (SUCCEEDED(hr)) { Log("override applied: %s", hashName); return hr; }
            Log("override FAILED to compile (hr=0x%08lx), falling back to original: %s", hr, hashName);
            if (ppErr && *ppErr) {
                // ID3DXBuffer: vtbl[3]=GetBufferPointer, vtbl[2]=Release
                auto buf = (IUnknown*)*ppErr;
                auto getPtr = (void*(WINAPI*)(void*))(*(void***)buf)[3];
                Log("  errors: %s", (const char*)getPtr(buf));
                buf->Release(); *ppErr = nullptr;
            }
        }
    }

    return o_D3DXCompileShader(src, len, defs, inc, entry, profile, flags, ppShader, ppErr, ppCT);
}

static void InstallShaderHook()
{
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE exe = GetModuleHandleA(nullptr);
    void** slot = FindIAT(exe, "d3dx9_42.dll", "D3DXCompileShader");
    if (!slot) { Log("D3DXCompileShader import not found - shader hook disabled"); return; }
    o_D3DXCompileShader = (PFN_D3DXCompileShader)PatchIAT(slot, (void*)hk_D3DXCompileShader);
    Log("D3DXCompileShader hooked (IAT slot %p, orig %p)", slot, o_D3DXCompileShader);
}

// --------------------------------------------------------------------------
// Game clock freeze (pause on focus loss)
//
// The engine derives DeltaTime from QueryPerformanceCounter (and GetTickCount
// in places). While paused we hand APGame.exe a clock that stands still, so
// gameplay, dialogue timers, AI and physics all see DeltaTime == 0. Our own
// code (and every other DLL) keeps reading the real clock.
// --------------------------------------------------------------------------
typedef BOOL  (WINAPI *PFN_QPC)(LARGE_INTEGER*);
typedef DWORD (WINAPI *PFN_GetTickCount)();
static PFN_QPC          o_QPC = nullptr;
static PFN_GetTickCount o_GetTickCount = nullptr;

static SRWLOCK  g_clockLock = SRWLOCK_INIT;
static bool     g_paused    = false;
static LONGLONG g_qpcPausedAt = 0, g_qpcOffset = 0;   // in QPC ticks
static DWORD    g_tickPausedAt = 0, g_tickOffset = 0; // in ms

static BOOL WINAPI hk_QPC(LARGE_INTEGER* out)
{
    BOOL ok = o_QPC(out);
    if (!ok || !out) return ok;
    AcquireSRWLockShared(&g_clockLock);
    out->QuadPart = (g_paused ? g_qpcPausedAt : out->QuadPart) - g_qpcOffset;
    ReleaseSRWLockShared(&g_clockLock);
    return ok;
}

static DWORD WINAPI hk_GetTickCount()
{
    DWORD t = o_GetTickCount();
    AcquireSRWLockShared(&g_clockLock);
    t = (g_paused ? g_tickPausedAt : t) - g_tickOffset;
    ReleaseSRWLockShared(&g_clockLock);
    return t;
}

static void SetGamePaused(bool pause)
{
    if (!o_QPC) return;
    LARGE_INTEGER now; o_QPC(&now);
    DWORD tick = o_GetTickCount ? o_GetTickCount() : GetTickCount();
    AcquireSRWLockExclusive(&g_clockLock);
    if (pause && !g_paused) {
        g_qpcPausedAt = now.QuadPart;
        g_tickPausedAt = tick;
        g_paused = true;
    } else if (!pause && g_paused) {
        g_qpcOffset  += now.QuadPart - g_qpcPausedAt;
        g_tickOffset += tick - g_tickPausedAt;
        g_paused = false;
    }
    ReleaseSRWLockExclusive(&g_clockLock);
}

static void InstallClockHooks()
{
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (void** s = FindIAT(exe, "kernel32.dll", "QueryPerformanceCounter"))
        o_QPC = (PFN_QPC)PatchIAT(s, (void*)hk_QPC);
    if (void** s = FindIAT(exe, "kernel32.dll", "GetTickCount"))
        o_GetTickCount = (PFN_GetTickCount)PatchIAT(s, (void*)hk_GetTickCount);
    Log("clock hooks: QPC=%p GetTickCount=%p", o_QPC, o_GetTickCount);
}

// Cursor confinement (multi-monitor): keep the cursor inside the game window
// while it has focus. Windows drops the clip on focus changes, so it's
// re-applied every frame if needed and released whenever we lose focus.
static bool g_cursorClipped = false;

static void ReleaseCursorClip()
{
    if (g_cursorClipped) { ClipCursor(nullptr); g_cursorClipped = false; }
}

static void UpdateCursorClip(bool focused)
{
    if (!g_hwnd) return;
    if (!cfg.confineCursor || !focused || IsIconic(g_hwnd)) { ReleaseCursorClip(); return; }
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return;
    POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
    ClientToScreen(g_hwnd, &tl); ClientToScreen(g_hwnd, &br);
    RECT want = { tl.x, tl.y, br.x, br.y }, cur;
    if (want.right <= want.left || want.bottom <= want.top) return;
    if (!g_cursorClipped || !GetClipCursor(&cur) || !EqualRect(&cur, &want)) {
        ClipCursor(&want);
        g_cursorClipped = true;
    }
}

// Focus-loss pause, part 2: park the game (main) thread inside its own message
// pump while another window is in front. We keep pumping messages ourselves so
// the window stays responsive, and freeze the clock only for that wait, so the
// engine never runs a frame against a stopped clock (UE3's frame limiter
// spin-waits on the clock and would hang).
typedef BOOL (WINAPI *PFN_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static PFN_PeekMessageW o_PeekMessageW = nullptr;
static DWORD g_mainThread = 0;
static volatile bool g_exiting = false;   // window closing / game shutting down: never park the thread

static bool GameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void WaitWhileUnfocused()
{
    static bool inWait = false;            // main thread only; guards re-entry from WndProc
    if (inWait) return;
    inWait = true;
    ReleaseCursorClip();
    SetGamePaused(true);
    Log("focus lost - game paused");
    bool quit = false;
    while (!quit && !g_exiting && !GameHasFocus() && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd)) {
        MSG m;
        while (o_PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); quit = true; break; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!quit) MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    SetGamePaused(false);
    Log(quit || g_exiting ? "game is closing - resuming so it can exit" : "focus regained - resuming");
    inWait = false;
}

static void ConRunQueued();
static BOOL WINAPI hk_PeekMessageW(LPMSG msg, HWND wnd, UINT fmin, UINT fmax, UINT remove)
{
    if (g_conQueued && GetCurrentThreadId() == g_mainThread) ConRunQueued();
    if (cfg.pauseOnFocusLoss && g_hwnd && !g_exiting && GetCurrentThreadId() == g_mainThread &&
        IsWindowVisible(g_hwnd) && !GameHasFocus())
        WaitWhileUnfocused();
    return o_PeekMessageW(msg, wnd, fmin, fmax, remove);
}

// Crash reports: UE3 turns a crash into an error box (with its call stack) before it closes, so the
// box text goes to our log; a vectored handler also logs the first fatal exceptions with the module
// and offset they happened in (the game's own handler runs after it).
typedef int (WINAPI *PFN_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef int (WINAPI *PFN_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static PFN_MessageBoxW o_MessageBoxW = nullptr;
static PFN_MessageBoxA o_MessageBoxA = nullptr;
static int WINAPI hk_MessageBoxW(HWND w, LPCWSTR text, LPCWSTR cap, UINT type)
{
    char t[4096] = "", c[256] = "";
    if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof t, nullptr, nullptr);
    if (cap) WideCharToMultiByte(CP_UTF8, 0, cap, -1, c, sizeof c, nullptr, nullptr);
    Log("game: message box '%s': %s", c, t);
    return o_MessageBoxW(w, text, cap, type);
}
static int WINAPI hk_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT type)
{
    Log("game: message box '%s': %s", cap ? cap : "", text ? text : "");
    return o_MessageBoxA(w, text, cap, type);
}
static LONG CALLBACK CrashVeh(PEXCEPTION_POINTERS e)
{
    static volatile LONG n = 0;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&n) > 6) return EXCEPTION_CONTINUE_SEARCH;
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr; char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof mod);
        if (const char* sl = strrchr(mod, '\\')) memmove(mod, sl + 1, strlen(sl));
    }
    const CONTEXT* x = e->ContextRecord;
    Log("crash?: exception %08lx at %p (%s+0x%lx) thread %lu, %s %p | eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx esp %08lx",
        code, at, mod, (unsigned long)((BYTE*)at - (BYTE*)m), GetCurrentThreadId(),
        e->ExceptionRecord->NumberParameters >= 2 ? (e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading") : "",
        e->ExceptionRecord->NumberParameters >= 2 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : nullptr,
        x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallFocusHook()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (void** s = FindIAT(GetModuleHandleA(nullptr), "user32.dll", "PeekMessageW"))
        o_PeekMessageW = (PFN_PeekMessageW)PatchIAT(s, (void*)hk_PeekMessageW);
    if (void** s = FindIAT(GetModuleHandleA(nullptr), "user32.dll", "MessageBoxW"))
        o_MessageBoxW = (PFN_MessageBoxW)PatchIAT(s, (void*)hk_MessageBoxW);
    if (void** s = FindIAT(GetModuleHandleA(nullptr), "user32.dll", "MessageBoxA"))
        o_MessageBoxA = (PFN_MessageBoxA)PatchIAT(s, (void*)hk_MessageBoxA);
    AddVectoredExceptionHandler(1, CrashVeh);
    Log("focus hook: PeekMessageW=%p mainThread=%lu", o_PeekMessageW, g_mainThread);
}

// --------------------------------------------------------------------------
// Frame limiter
// --------------------------------------------------------------------------
static HANDLE        g_timer = nullptr;
static LARGE_INTEGER g_freq, g_next;

static void LimitFrame()
{
    if (cfg.fpsLimit <= 0) return;
    if (!g_freq.QuadPart) {
        QueryPerformanceFrequency(&g_freq);
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr); // pre-1803 fallback
        QueryPerformanceCounter(&g_next);
    }
    const LONGLONG period = g_freq.QuadPart / cfg.fpsLimit;
    g_next.QuadPart += period;

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG remain = g_next.QuadPart - now.QuadPart;
    if (remain <= 0) {                        // running behind: resync, don't try to catch up
        if (-remain > period) g_next = now;
        return;
    }
    // Sleep until ~1 ms before target, then spin.
    LONGLONG sleepTicks = remain - g_freq.QuadPart / 1000;
    if (sleepTicks > 0 && g_timer) {
        LARGE_INTEGER due; due.QuadPart = -(sleepTicks * 10000000LL / g_freq.QuadPart);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(g_timer, INFINITE);
    }
    do { YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < g_next.QuadPart);
}

// --------------------------------------------------------------------------
// Post FX (applied to the final frame at Present): FXAA + contrast-adaptive sharpen
// --------------------------------------------------------------------------
static const char* kQuadVS = R"HLSL(
float4 halfPixel : register(c0);  // x=-1/w y=+1/h  (D3D9 texel/pixel alignment)
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VOut main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VOut o;
    o.pos = float4(pos.xy + halfPixel.xy, 0, 1);
    o.uv = uv;
    return o;
}
)HLSL";

// Colour grade, in linear light. Contrast is an S-curve in gamma space that keeps black and white
// fixed (no clipping), pivoting on mid grey; vibrance saturates muted colours more than vivid ones.
static const char* kGradePS = R"HLSL(
sampler2D sColor : register(s0);
float4 g0 : register(c2);   // x exposure multiplier, y contrast, z vibrance, w saturation
float4 g1 : register(c3);   // x black offset (linear), y warmth -1..1
float Curve(float x, float k)
{
    const float p = 0.4588;                      // 0.18 in gamma 2.2
    float lo = p * pow(max(x / p, 1e-6), k);
    float hi = 1 - (1 - p) * pow(max((1 - x) / (1 - p), 1e-6), k);
    return lerp(lo, hi, saturate((x - p) * 1e6));
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float4 s = tex2Dlod(sColor, float4(uv, 0, 0));
    float3 c = pow(max(s.rgb, 0), 2.2) * g0.x;
    c *= float3(1 + g1.y * 0.08, 1, 1 - g1.y * 0.08);
    float Y = max(dot(c, L), 1e-5);
    float Yg = pow(Y, 1 / 2.2);
    float Yc = pow(Curve(saturate(Yg), g0.y) + max(Yg - 1, 0), 2.2);
    c *= Yc / Y;
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float sat = (mx - mn) / max(mx, 1e-5);
    c = Yc + (c - Yc) * (g0.w * (1 + g0.z * (1 - sat)));
    c = max(c - g1.x, 0) / (1 - g1.x);
    return float4(pow(c, 1 / 2.2), s.a);
}
)HLSL";

// Supersampling: 4x4 bilinear taps spaced one output pixel apart, Mitchell-Netravali weights
// (B = C = 1/3). In linear light for SDR; scRGB is already linear.
static const char* kDownPS = R"HLSL(
sampler2D sSrc : register(s0);
float4 dp : register(c1);   // xy source texel size, zw source pixels per output pixel
float4 dq : register(c2);   // x = 1: gamma-encoded input
float3 Tap(float2 uv)
{
    float3 c = tex2Dlod(sSrc, float4(uv, 0, 0)).rgb;
    return lerp(c, pow(max(c, 1e-8), 2.2), dq.x);
}
float3 Row(float2 uv, float dy, float2 st)
{
    return Tap(uv + float2(-1.5, dy) * st) * -0.0347 + Tap(uv + float2(-0.5, dy) * st) * 0.5347
         + Tap(uv + float2( 0.5, dy) * st) *  0.5347 + Tap(uv + float2( 1.5, dy) * st) * -0.0347;
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 st = dp.zw * dp.xy;                   // one output pixel, in source uv
    float3 acc = Row(uv, -1.5, st) * -0.0347 + Row(uv, -0.5, st) * 0.5347
               + Row(uv,  0.5, st) *  0.5347 + Row(uv,  1.5, st) * -0.0347;
    acc = max(acc, 1e-8);
    return float4(lerp(acc, pow(acc, 1 / 2.2), dq.x), 1);
}
)HLSL";

// HDR: (display luminance, scene luminance) pairs for the exposure fit
static const char* kHdrFitPS = R"HLSL(
sampler2D sLdr : register(s0);
sampler2D sHdr : register(s1);
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float y0 = dot(pow(max(tex2Dlod(sLdr, float4(uv, 0, 0)).rgb, 0), 2.2), L);
    float y1 = dot(max(tex2Dlod(sHdr, float4(uv, 0, 0)).rgb, 0), L);
    return float4(y0, y1, 0, 1);
}
)HLSL";

// HDR: let near-white pixels grow towards the scene's real brightness (from the game's own HDR
// buffer, scaled by the fitted exposure), rolled off below the display peak. Hue comes from the
// game's graded image, only luminance grows. Output stays gamma-encoded (values above 1 allowed).
static const char* kHdrExpandPS = R"HLSL(
sampler2D sLdr : register(s0);
sampler2D sHdr : register(s1);
float4 h : register(c1);    // x exposure, y peak / paper white, z strength, w knee
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float4 s = tex2Dlod(sLdr, float4(uv, 0, 0));
    float3 c = pow(max(s.rgb, 0), 2.2);
    float Y = max(dot(c, L), 1e-4);
    float Yh = dot(max(tex2Dlod(sHdr, float4(uv, 0, 0)).rgb, 0), L) * h.x;
    // only pixels that are bright in luminance (a saturated blue at full channel is not a highlight)
    float t = saturate((Y - h.w) / (1 - h.w));
    t = t * t * (3 - 2 * t);
    float room = max(h.y - 1, 1e-3);
    float Yt = Y + room * (1 - exp(-max(Yh - Y, 0) / room));
    float b = min(lerp(1, Yt / Y, t * h.z), h.y);
    return float4(pow(c * b, 1 / 2.2), s.a);
}
)HLSL";

// HDR test pattern, in the same gamma-encoded form as the game's frame (docs/hdr_testpattern.png is
// the identical image for an SDR viewer): 75% colour bars, a grey ramp, 100% primaries, skin tones.
static const char* kHdrPatternPS = R"HLSL(
float4 hp : register(c1);   // x = 1: bottom row = highlight steps above paper white
float Sel(float x, float lo, float hi) { return step(lo, x) * (1 - step(hi, x)); }
float Enc(float k) { return 1.055 * pow(k, 1 / 2.4) - 0.055; }   // sRGB-encode k x paper white
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float x = uv.x;
    // row 1: 75% bars white yellow cyan green magenta red blue black
    float3 bars = 0.75 * (float3(1,1,1) * Sel(x, 0, 0.125) + float3(1,1,0) * Sel(x, 0.125, 0.25)
                + float3(0,1,1) * Sel(x, 0.25, 0.375) + float3(0,1,0) * Sel(x, 0.375, 0.5)
                + float3(1,0,1) * Sel(x, 0.5, 0.625) + float3(1,0,0) * Sel(x, 0.625, 0.75)
                + float3(0,0,1) * Sel(x, 0.75, 0.875));
    // row 2: 32-step grey ramp
    float3 ramp = floor(min(x, 0.999) * 32) / 31;
    // row 3: 100% red green blue, white, 50% grey, 20% grey
    float3 prim = float3(1,0,0) * Sel(x, 0, 1.0/6) + float3(0,1,0) * Sel(x, 1.0/6, 2.0/6)
                + float3(0,0,1) * Sel(x, 2.0/6, 3.0/6) + float3(1,1,1) * Sel(x, 3.0/6, 4.0/6)
                + float3(0.5,0.5,0.5) * Sel(x, 4.0/6, 5.0/6) + float3(0.2,0.2,0.2) * Sel(x, 5.0/6, 2);
    // row 4: skin tones
    float3 skin = float3(0.94,0.78,0.69) * Sel(x, 0, 0.25) + float3(0.78,0.57,0.44) * Sel(x, 0.25, 0.5)
                + float3(0.55,0.38,0.26) * Sel(x, 0.5, 0.75) + float3(0.36,0.22,0.15) * Sel(x, 0.75, 2);
    // highlight steps: 1, 1.25, 1.5, 2, 3, 4 x paper white (all equal = this path can't go above white)
    float3 hi = Enc(1) * Sel(x, 0, 1.0/6) + Enc(1.25) * Sel(x, 1.0/6, 2.0/6) + Enc(1.5) * Sel(x, 2.0/6, 3.0/6)
              + Enc(2) * Sel(x, 3.0/6, 4.0/6) + Enc(3) * Sel(x, 4.0/6, 5.0/6) + Enc(4) * Sel(x, 5.0/6, 2);
    skin = lerp(skin, hi, hp.x);
    float y = uv.y;
    float3 c = bars * Sel(y, -1, 0.45) + ramp * Sel(y, 0.45, 0.65) + prim * Sel(y, 0.65, 0.85) + skin * Sel(y, 0.85, 2);
    return float4(c, 1);
}
)HLSL";

// HDR: gamma-encoded frame -> HDR10 (Rec.2020 primaries, PQ) or scRGB (linear Rec.709, 1.0 = 80 nits)
static const char* kHdrEncodePS = R"HLSL(
sampler2D sSrc : register(s0);
float4 e : register(c1);    // x paper white nits, y peak nits, z 1 = PQ / 0 = scRGB, w 1 = sRGB curve / 0 = gamma 2.2
float4 e2 : register(c2);   // xy uv scale (only part of the source holds the image), z game gamma ramp exponent, w saturation
float4 e3 : register(c3);   // x = 1: something after us decodes the swapchain as sRGB once more - pre-encode
float3 PQ(float3 nits)
{
    float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 v = pow(max(tex2Dlod(sSrc, float4(uv * e2.xy, 0, 0)).rgb, 0), e2.z);      // game brightness ramp
    float3 srgb = lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), saturate((v - 0.04045) * 1e6));
    float3 c = lerp(pow(v, 2.2), srgb, e.w) * e.x;                                      // nits, Rec.709
    float Y = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(lerp(Y, c, e2.w), 0);                                                        // saturation trim
    float m = max(c.r, max(c.g, c.b));
    float s = 0.7 * e.y;                                                                 // soft shoulder to the peak
    float mr = min(m, s) + (e.y - s) * (1 - exp(-max(m - s, 0) / (e.y - s)));
    c *= mr / max(m, 1e-4);                                                              // hue preserved
    float3 c2020 = float3(dot(c, float3(0.6274, 0.3293, 0.0433)),
                          dot(c, float3(0.0691, 0.9195, 0.0114)),
                          dot(c, float3(0.0164, 0.0880, 0.8956)));
    float3 sc = c / lerp(80.0, e.x, e3.x);       // compensated: 1.0 = paper white (what that path treats as white)
    float3 enc = lerp(sc * 12.92, 1.055 * pow(max(sc, 1e-8), 1 / 2.4) - 0.055, saturate((sc - 0.0031308) * 1e6));
    sc = lerp(sc, enc, e3.x);
    return float4(lerp(sc, PQ(max(c2020, 0)), e.z), 1);
}
)HLSL";

static const char* kFxaaPS = R"HLSL(
sampler2D s0 : register(s0);
float4 rcpFrame : register(c0);   // x=1/w y=1/h
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 px = rcpFrame.xy;
    float3 rgbM = tex2D(s0, uv).rgb;
    float lNw = Luma(tex2D(s0, uv + float2(-0.5, -0.5) * px).rgb);
    float lSw = Luma(tex2D(s0, uv + float2(-0.5,  0.5) * px).rgb);
    float lNe = Luma(tex2D(s0, uv + float2( 0.5, -0.5) * px).rgb) + 1.0 / 384.0;
    float lSe = Luma(tex2D(s0, uv + float2( 0.5,  0.5) * px).rgb);
    float lM  = Luma(rgbM);
    float lMin = min(lM, min(min(lNw, lSw), min(lNe, lSe)));
    float lMax = max(lM, max(max(lNw, lSw), max(lNe, lSe)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return float4(rgbM, 1);

    float2 dir;
    dir.x = (lSw - lNe) + (lSe - lNw);
    dir.y = (lSw - lNe) - (lSe - lNw);
    float2 dir1 = normalize(dir + 1e-6);
    float3 n1 = tex2D(s0, uv - dir1 * px * 0.5).rgb;
    float3 p1 = tex2D(s0, uv + dir1 * px * 0.5).rgb;
    float dirAbsMin = min(abs(dir1.x), abs(dir1.y)) * 8.0;
    float2 dir2 = clamp(dir1 / max(dirAbsMin, 1e-4), -2.0, 2.0);
    float3 n2 = tex2D(s0, uv - dir2 * px * 2.0).rgb;
    float3 p2 = tex2D(s0, uv + dir2 * px * 2.0).rgb;
    float3 rgbA = (n1 + p1) * 0.5;
    float3 rgbB = rgbA * 0.5 + (n2 + p2) * 0.25;
    float lB = Luma(rgbB);
    return float4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1);
}
)HLSL";

static const char* kSharpenPS = R"HLSL(
sampler2D s0 : register(s0);
float4 rcpFrame : register(c0);   // x=1/w y=1/h
float4 params   : register(c1);   // x=sharpness 0..1
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 px = rcpFrame.xy;
    float3 c = tex2D(s0, uv).rgb;
    float3 n = tex2D(s0, uv + float2(0, -px.y)).rgb;
    float3 s = tex2D(s0, uv + float2(0,  px.y)).rgb;
    float3 w = tex2D(s0, uv + float2(-px.x, 0)).rgb;
    float3 e = tex2D(s0, uv + float2( px.x, 0)).rgb;
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, params.x);
    float3 wgt = amp * peak;
    float3 outc = (c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt);
    return float4(saturate(outc), 1);
}
)HLSL";

// FXAA 3.11 "quality" (Lottes): finds the whole edge span (up to 12 steps each way) and blends
// across it, so long near-horizontal edges get smooth instead of the console version's blur.
static const char* kFxaaQPS = R"HLSL(
sampler2D s0 : register(s0);
float4 rcpFrame : register(c0);
float L(float2 uv) { return dot(tex2Dlod(s0, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 rcp = rcpFrame.xy;
    float4 rgbM = tex2Dlod(s0, float4(uv, 0, 0));
    float lumaM = dot(rgbM.rgb, float3(0.299, 0.587, 0.114));
    float lumaS = L(uv + float2(0, rcp.y)), lumaE = L(uv + float2(rcp.x, 0));
    float lumaN = L(uv - float2(0, rcp.y)), lumaW = L(uv - float2(rcp.x, 0));
    float maxL = max(max(max(lumaS, lumaE), max(lumaN, lumaW)), lumaM);
    float minL = min(min(min(lumaS, lumaE), min(lumaN, lumaW)), lumaM);
    float range = maxL - minL;
    if (range < max(0.0312, maxL * 0.125)) return float4(rgbM.rgb, 1);
    float lumaNW = L(uv - rcp), lumaSE = L(uv + rcp);
    float lumaNE = L(uv + float2(rcp.x, -rcp.y)), lumaSW = L(uv + float2(-rcp.x, rcp.y));
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = uv;
    float2 offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float subpixE = subpixC * subpixC;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    bool lumaMLTZero = (lumaM - lumaNN * 0.5) < 0.0;
    float lumaEndN = L(posN) - lumaNN * 0.5, lumaEndP = L(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    [loop] for (int i = 0; i < 11; i++) {
        if (doneN && doneP) break;
        float st = i == 0 ? 1.5 : (i < 5 ? 2.0 : (i == 5 ? 4.0 : 8.0));
        if (!doneN) { posN -= offNP * st; lumaEndN = L(posN) - lumaNN * 0.5; doneN = abs(lumaEndN) >= gradientScaled; }
        if (!doneP) { posP += offNP * st; lumaEndP = L(posP) - lumaNN * 0.5; doneP = abs(lumaEndP) >= gradientScaled; }
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixF = subpixD * subpixE;
    float pixelOffset = -dst / max(spanLength, 1e-6) + 0.5;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixF * subpixF * 0.75);
    float2 p = uv;
    if (!horzSpan) p.x += pixelOffsetSubpix * lengthSign; else p.y += pixelOffsetSubpix * lengthSign;
    return float4(tex2Dlod(s0, float4(p, 0, 0)).rgb, 1);
}
)HLSL";

// Screen-space reflections for floors. World position from depth via the main view's
// view-projection (taken from the game's vertex shaders); rays marched in world space and projected
// back to the screen; only surfaces facing up (world Z) reflect.
static const char* kSSRPS = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sDepth : register(s1);
sampler2D sMat : register(s2);                   // r skin, g metal, b rough (snow, dirt, carpet... never reflect)
float4 rcpFrame : register(c0);
float4 dP  : register(c6);                       // depth source, as in the AO
float4 sw0 : register(c8);  float4 sw1 : register(c9);  float4 sw2 : register(c10); float4 sw3 : register(c11);
float4 ws0 : register(c12); float4 ws1 : register(c13); float4 ws2 : register(c14); float4 ws3 : register(c15);
float4 sp  : register(c16);                      // x floor strength, y ray length, z thickness, w up threshold
float4 sp2 : register(c17);                      // x metal strength, y material mask valid
float4 sp3 : register(c18);                      // x floor roughness, y metal roughness (blur, in fractions of the ray length)
float LinD(float2 uv)
{
    float4 t = tex2Dlod(sDepth, float4(uv, 0, 0));
    return dP.x > 0.5 ? 1.0 / max(dP.y + dP.z * t.r, 1e-6) : t.a;
}
float3 W(float2 uv, float d)
{
    float sx = uv.x * 2 - 1, sy = 1 - uv.y * 2;
    return (sx * d) * sw0.xyz + (sy * d) * sw1.xyz + d * sw2.xyz + sw3.xyz;
}
float3 S(float3 wp)
{
    float3 c = wp.x * ws0.xyz + wp.y * ws1.xyz + wp.z * ws2.xyz + ws3.xyz;   // (sx*w, sy*w, w)
    return float3(c.x / c.z * 0.5 + 0.5, 0.5 - c.y / c.z * 0.5, c.z);
}
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float3 col = tex2Dlod(sColor, float4(uv, 0, 0)).rgb;
    float d = LinD(uv);
    if (d <= 1 || d > 30000) return float4(col, 1);
    float3 mat = sp2.y > 0.5 ? tex2Dlod(sMat, float4(uv, 0, 0)).rgb : 0;
    float metal = mat.g, rough = mat.b;
    float3 p = W(uv, d);
    float2 b = rcpFrame.xy;
    // normal from depth: on each axis use the neighbour that continues the surface (smaller step),
    // so edges and small curved objects don't get a sideways normal
    float dl = LinD(uv - float2(b.x, 0)), dr = LinD(uv + float2(b.x, 0));
    float du = LinD(uv - float2(0, b.y)), dd = LinD(uv + float2(0, b.y));
    float3 ex = abs(dr - d) < abs(d - dl) ? W(uv + float2(b.x, 0), dr) - p : p - W(uv - float2(b.x, 0), dl);
    float3 ey = abs(dd - d) < abs(d - du) ? W(uv + float2(0, b.y), dd) - p : p - W(uv - float2(0, b.y), du);
    float3 n = normalize(cross(ex, ey));
    float3 cam = sw3.xyz;
    float3 V = normalize(p - cam);
    if (dot(n, V) > 0) n = -n;
    float up = saturate((n.z - sp.w) / max(1 - sp.w, 1e-3));
    float wf = sp.x * up * (1 - rough);
    float wm = metal * sp2.x;
    if (wf <= 0 && wm <= 0) return float4(col, 1);
    float3 Rd = reflect(V, n);
    float noise = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    float tMin = 2 + d * 0.01;                      // start a little off the surface: no self-hits
    float tPrev = tMin, tHit = -1;
    [loop] for (int i = 1; i <= 24; i++) {
        float t = (i - 1 + noise) / 24.0;
        t = tMin + t * t * sp.y;
        float3 s = S(p + Rd * t);
        if (s.x < 0 || s.x > 1 || s.y < 0 || s.y > 1 || s.z <= 1) break;
        float diff = s.z - LinD(s.xy);
        if (diff > 0 && diff < sp.z * (1 + s.z * 0.002)) { tHit = t; break; }
        tPrev = t;
    }
    if (tHit < 0) return float4(col, 1);
    float lo = tPrev, hi = tHit;
    [unroll] for (int r = 0; r < 5; r++) {
        float mid = 0.5 * (lo + hi);
        float3 sm = S(p + Rd * mid);
        if (sm.z - LinD(sm.xy) > 0) hi = mid; else lo = mid;
    }
    float3 sh = S(p + Rd * hi);
    if (length((sh.xy - uv) / b) < 3) return float4(col, 1);   // hit right next to the start: the surface itself
    // glossy blur: a small rotated disc whose size grows with the ray length (rougher = wider)
    float rough0 = lerp(sp3.x, sp3.y, metal);
    float rad = rough0 * hi / max(sh.z, 1);
    rad = min(rad, 0.04);
    float ang = noise * 6.2831853;
    float2 cs = float2(cos(ang), sin(ang));
    float3 rc = tex2Dlod(sColor, float4(sh.xy, 0, 0)).rgb;
    [unroll] for (int k2 = 0; k2 < 6; k2++) {
        float a2 = k2 * 1.0471976;
        float2 dir = float2(cs.x * cos(a2) - cs.y * sin(a2), cs.x * sin(a2) + cs.y * cos(a2));
        float2 o = sh.xy + dir * rad * float2(rcpFrame.x / rcpFrame.y, 1) * (0.5 + 0.5 * frac(k2 * 0.618 + noise));
        rc += tex2Dlod(sColor, float4(saturate(o), 0, 0)).rgb;
    }
    rc /= 7;
    float2 e = min(sh.xy, 1 - sh.xy);
    float edge = saturate(min(e.x, e.y) * 12);
    float fres = pow(1 - saturate(dot(-V, n)), 5);
    float fade = 1 - hi / (tMin + sp.y);
    // metals tint the reflection with their own colour
    float3 tint = saturate(col / max(max(col.r, max(col.g, col.b)), 0.04));
    rc *= lerp(1, lerp(1, tint, 0.7), metal);
    // floors: mostly at grazing angles (Fresnel); metal: at every angle but weaker head-on
    float amt = max(wf * lerp(0.08, 1.0, fres), wm * lerp(0.4, 1.0, fres)) * edge * fade;
    return float4(lerp(col, rc, saturate(amt)), 1);
}
)HLSL";

// Skin subsurface scattering (separable, after Jimenez et al.): a depth-aware 1D blur with a wider
// kernel for red than for green/blue, only on skin pixels (material mask .r), in approximate linear
// light. Run twice (horizontal, vertical). Taps that leave the skin or jump in depth fall back to the
// centre colour, so edges stay sharp.
static const char* kSSSPS = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sDepth : register(s1);
sampler2D sMat   : register(s2);
float4 dP : register(c6);
float4 sp : register(c7);   // xy one-pixel step along the blur direction, z scatter radius x focal length (px x depth), w strength
float LinD(float2 uv)
{
    float4 t = tex2Dlod(sDepth, float4(uv, 0, 0));
    return dP.x > 0.5 ? 1.0 / max(dP.y + dP.z * t.r, 1e-6) : t.a;
}
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c0 = tex2Dlod(sColor, float4(uv, 0, 0));
    float m = tex2Dlod(sMat, float4(uv, 0, 0)).r;
    if (m < 0.02) return c0;
    float d = LinD(uv);
    float r = min(sp.z / max(d, 1), 48);            // 2 sigma (red) in pixels
    float3 lc = c0.rgb * c0.rgb;
    float3 acc = lc, wsum = 1;
    const float3 isg = float3(1.0, 1.0 / (0.45 * 0.45), 1.0 / (0.25 * 0.25));
    [unroll] for (int i = 1; i <= 6; i++) {
        float x = i / 6.0;                           // 0..1 of 2 sigma
        float3 w = exp(-2 * x * x * isg);            // exp(-(2x)^2 / (2 sigma_c^2))
        [unroll] for (int sgn = -1; sgn <= 1; sgn += 2) {
            float2 o = uv + sp.xy * (sgn * x * r);
            float3 c = tex2Dlod(sColor, float4(o, 0, 0)).rgb;
            float mm = tex2Dlod(sMat, float4(o, 0, 0)).r;
            float follow = saturate(1 - abs(LinD(o) - d) * 0.25) * step(0.5, mm);
            acc += lerp(lc, c * c, follow) * w;
            wsum += w;
        }
    }
    float3 b = acc / wsum;
    return float4(sqrt(max(lerp(lc, b, saturate(sp.w * m)), 0)), c0.a);
}
)HLSL";

// Wide HDR bloom (Call of Duty: AW style): 13-tap downsamples with a soft threshold, tent upsamples.
static const char* kBloomDownPS = R"HLSL(
sampler2D s0 : register(s0);
float4 p1 : register(c1);   // xy source texel, z threshold (0 = none), w knee
float3 T(float2 uv) { return tex2D(s0, uv).rgb; }
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 t = p1.xy;
    float3 a = T(uv + t * float2(-2, -2)), b = T(uv + t * float2(0, -2)), c = T(uv + t * float2(2, -2));
    float3 d = T(uv + t * float2(-1, -1)), e = T(uv + t * float2(1, -1));
    float3 f = T(uv + t * float2(-2, 0)),  g = T(uv),                     h = T(uv + t * float2(2, 0));
    float3 i = T(uv + t * float2(-1, 1)),  j = T(uv + t * float2(1, 1));
    float3 k = T(uv + t * float2(-2, 2)),  l = T(uv + t * float2(0, 2)),  m = T(uv + t * float2(2, 2));
    float3 o = (d + e + i + j) * 0.125 + (a + b + f + g + b + c + g + h + f + g + k + l + g + h + l + m) * 0.03125;
    if (p1.z > 0) {
        float br = max(o.r, max(o.g, o.b));
        float rq = clamp(br - p1.z + p1.w, 0, 2 * p1.w);
        rq = rq * rq / (4 * p1.w + 1e-4);
        o *= max(rq, br - p1.z) / max(br, 1e-4);
        o = min(o, 32);
    }
    return float4(max(o, 0), 1);
}
)HLSL";
static const char* kBloomUpPS = R"HLSL(
sampler2D s0 : register(s0);   // lower level (upsampled)
sampler2D s1 : register(s1);   // this level (added)
float4 p1 : register(c1);      // xy lower-level texel
float3 T(float2 uv) { return tex2D(s0, uv).rgb; }
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 t = p1.xy;
    float3 s = 4 * T(uv) + 2 * (T(uv + float2(t.x, 0)) + T(uv - float2(t.x, 0)) + T(uv + float2(0, t.y)) + T(uv - float2(0, t.y)))
             + T(uv + t) + T(uv - t) + T(uv + float2(t.x, -t.y)) + T(uv + float2(-t.x, t.y));
    return float4(tex2D(s1, uv).rgb + s / 16, 1);
}
)HLSL";

// ID3DXBuffer vtable: [2]=Release [3]=GetBufferPointer
static void* BufPtr(void* b)    { return ((void*(WINAPI*)(void*))(*(void***)b)[3])(b); }
static void  BufRelease(void* b){ ((ULONG(WINAPI*)(void*))(*(void***)b)[2])(b); }

// SSAO. Scene depth comes from the alpha channel of UE3's HDR scene buffer
// (world-space distance, copied right after the base pass).
static const char* kAOPS = R"HLSL(
// GTAO (horizon-based, after Jimenez et al. 2016 / XeGTAO). View space is rebuilt from linear depth:
// either UE3's HDR alpha (16-bit) or the game's own depth buffer read through INTZ (24-bit, calibrated).
#define PI 3.14159265
sampler2D sDepth : register(s1);
float4 rcpFrame : register(c0);   // x=1/w y=1/h z=w w=h
float4 aoP0     : register(c2);   // x=tanHalfFovX y=tanHalfFovY z=radius w=intensity
float4 aoP1     : register(c3);   // y=maxDist z=fadeStart w=aspect
float4 aoP2     : register(c5);   // x=near fade distance
float4 dP       : register(c6);   // x=source (0 HDR alpha, 1 INTZ) y,z = 1/d = y + z*zbuf  w=normal baseline px
float LinD(float2 uv)
{
    float4 t = tex2Dlod(sDepth, float4(uv, 0, 0));
    return dP.x > 0.5 ? 1.0 / max(dP.y + dP.z * t.r, 1e-6) : t.a;
}
float3 P(float2 uv, float d) { return float3((uv.x * 2 - 1) * aoP0.x * d, (1 - uv.y * 2) * aoP0.y * d, d); }
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float d = LinD(uv);
    if (d >= aoP1.y || d <= 0) return 1;
    float3 p = P(uv, d);
    // normal: the side with the smaller depth step on each axis (keeps silhouettes clean)
    float2 b = dP.w * rcpFrame.xy;
    float dR = LinD(uv + float2(b.x, 0)), dL = LinD(uv - float2(b.x, 0));
    float dDn = LinD(uv + float2(0, b.y)), dUp = LinD(uv - float2(0, b.y));
    float3 tx = (abs(dR - d) < abs(d - dL)) ? P(uv + float2(b.x, 0), dR) - p : p - P(uv - float2(b.x, 0), dL);
    float3 ty = (abs(dDn - d) < abs(d - dUp)) ? P(uv + float2(0, b.y), dDn) - p : p - P(uv - float2(0, b.y), dUp);
    float3 n = normalize(cross(tx, ty));
    if (dot(n, p) > 0) n = -n;                                  // face the viewer (n.z alone flips near-level floors)
    float3 viewV = -normalize(p);
    float nv = dot(n, viewV);
    if (nv < 0.1) n = normalize(n + viewV * (0.1 - nv));       // a visible surface faces the camera

    float W = rcpFrame.z;
    float radPx = clamp(aoP0.z / (2.0 * aoP0.x * d) * W, 2.0, W * 0.08);
    float Reff = radPx / W * 2.0 * aoP0.x * d;                  // radius shrinks with the capped footprint
    float fMul = -1.0 / (0.615 * Reff), fAdd = (Reff * 0.385) / (0.615 * Reff) + 1.0;
    float noise  = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    float noise2 = frac(52.9829189 * frac(dot(vpos + float2(37, 17), float2(0.06711056, 0.00583715))));

    float vis = 0;
    [loop] for (int s = 0; s < 3; s++) {
        float phi = (s + noise) / 3.0 * PI;
        float2 dir = float2(cos(phi), sin(phi));
        float3 dir3 = float3(dir.x, -dir.y, 0);
        float3 ortho = dir3 - dot(dir3, viewV) * viewV;
        float3 axis = cross(dir3, viewV);
        float3 projN = n - axis * dot(n, axis);
        float projLen = length(projN) + 1e-6;
        float cosN = clamp(dot(projN, viewV) / projLen, -1, 1);
        float nang = sign(dot(ortho, projN)) * acos(cosN);
        float low0 = cos(nang + PI / 2), low1 = cos(nang - PI / 2);
        float hc0 = low0, hc1 = low1;
        [loop] for (int j = 0; j < 6; j++) {             // real loop: keeps the shader far below SM3's 512 slots
            float t = pow((j + noise2) / 6.0, 1.5);
            float2 o = dir * max(t * radPx, j + 1.0) * rcpFrame.xy;
            float2 s0 = uv + o, s1 = uv - o;
            float3 d0 = P(s0, LinD(s0)) - p, d1 = P(s1, LinD(s1)) - p;
            float l0 = length(d0) + 1e-6, l1 = length(d1) + 1e-6;
            float in0 = (s0.x > 0 && s0.x < 1 && s0.y > 0 && s0.y < 1) ? 1.0 : 0.0;
            float in1 = (s1.x > 0 && s1.x < 1 && s1.y > 0 && s1.y < 1) ? 1.0 : 0.0;
            float h0 = lerp(low0, dot(d0 / l0, viewV), saturate(l0 * fMul + fAdd) * in0);
            float h1 = lerp(low1, dot(d1 / l1, viewV), saturate(l1 * fMul + fAdd) * in1);
            hc0 = max(hc0, h0); hc1 = max(hc1, h1);
        }
        float a0 = -acos(clamp(hc1, -1, 1)), a1 = acos(clamp(hc0, -1, 1));
        a0 = nang + max(a0 - nang, -PI / 2);
        a1 = nang + min(a1 - nang,  PI / 2);
        float sinN = sin(nang);
        float arc0 = (cosN + 2 * a0 * sinN - cos(2 * a0 - nang)) / 4;
        float arc1 = (cosN + 2 * a1 * sinN - cos(2 * a1 - nang)) / 4;
        vis += projLen * (arc0 + arc1);
    }
    float ao = pow(saturate(vis / 3.0), 1.6 * aoP0.w);
    ao = lerp(ao, 1, saturate((d - aoP1.z) / max(aoP1.y - aoP1.z, 1)));   // far fade
    ao = lerp(1, ao, saturate((d - aoP2.x) / max(aoP2.x, 1)));            // near fade
    return float4(ao, ao, ao, 1);
}
)HLSL";

// Fallback if the GTAO shader fails to compile on a driver: 0.6.5's volumetric obscurance (HDR alpha only).
static const char* kAOFallbackPS = R"HLSL(
sampler2D sDepth : register(s1);
float4 rcpFrame : register(c0);
float4 aoP0     : register(c2);
float4 aoP1     : register(c3);
float4 aoP2     : register(c5);
float D(float2 uv) { return tex2Dlod(sDepth, float4(uv, 0, 0)).a; }
float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float d = D(uv);
    if (d >= aoP1.y || d <= 0) return 1;
    float R = min(aoP0.z, 0.08 * 2.0 * aoP0.x * d), R2 = R * R;
    float uvR = max(R / (2.0 * aoP0.x * d), 3.0 * rcpFrame.x);
    float noise = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
    float occ = 0, wsum = 0;
    [unroll] for (int i = 0; i < 16; i++) {
        float a = (i + noise) * 2.3999632;
        float2 off = float2(cos(a), sin(a) * aoP1.w) * (uvR * sqrt((i + 0.5) / 16.0));
        float2 suv = uv + off;
        float inside = (suv.x > 0 && suv.x < 1 && suv.y > 0 && suv.y < 1) ? 1.0 : 0.0;
        float2 wo = off * float2(2.0 * aoP0.x, 2.0 * aoP0.y) * d;
        float h = sqrt(max(R2 - dot(wo, wo), 0));
        float sd = D(suv);
        float o = saturate((d + h - sd) / max(2.0 * h, 1e-3));
        float valid = saturate((sd - (d - 2.0 * h)) / max(h, 1e-3));
        float w = inside * h * valid;
        occ += o * w; wsum += w;
    }
    float v = occ / max(wsum, 1e-3);
    float ao = 1 - saturate((v - 0.53) * 3.0 * aoP0.w);
    ao = lerp(ao, 1, saturate((d - aoP1.z) / max(aoP1.y - aoP1.z, 1)));
    ao = lerp(1, ao, saturate((d - aoP2.x) / max(aoP2.x, 1)));
    return float4(ao, ao, ao, 1);
}
)HLSL";

// Calibration: for a sparse grid, the INTZ depth-buffer value next to the HDR-alpha linear depth.
// The CPU fits 1/d = A + B*z (exact for a perspective projection).
static const char* kZCopyPS = R"HLSL(
sampler2D sZ : register(s0);
float4 main(float2 uv : TEXCOORD0) : COLOR0 { return float4(tex2Dlod(sZ, float4(uv, 0, 0)).r, 0, 0, 1); }
)HLSL";

static const char* kFitPS = R"HLSL(
sampler2D sZ : register(s0);
sampler2D sD : register(s1);
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(tex2Dlod(sZ, float4(uv, 0, 0)).r, tex2Dlod(sD, float4(uv, 0, 0)).a, 0, 1);
}
)HLSL";

static const char* kAOBlurPS = R"HLSL(
sampler2D sAO    : register(s0);
sampler2D sDepth : register(s1);
float4 dir : register(c1);        // xy = one-pixel step in uv
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).a;
    float sum = 0, wsum = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 suv = uv + dir.xy * i;
        float a  = tex2Dlod(sAO, float4(suv, 0, 0)).r;
        float sd = tex2Dlod(sDepth, float4(suv, 0, 0)).a;
        float w = exp(-(i * i) / 12.5) * saturate(1 - abs(sd - d0) / (d0 * 0.04 + 1));
        sum += a * w; wsum += w;
    }
    float r = sum / max(wsum, 1e-4);
    return float4(r, r, r, 1);
}
)HLSL";

static const char* kAOCompositePS = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sAO    : register(s1);
sampler2D sGlass : register(s2);  // window glass coverage, written by our glass shader (MRT)
sampler2D sFog   : register(s3);  // fog transmittance (1 = clear), written by our fog shader (MRT)
float4 opt : register(c4);        // x = debug view, y = highlight protection, z = glass mask, w = fog fade
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2D(sColor, uv).rgb;
    float ao = tex2D(sAO, uv).r;
    if (opt.x > 0.5) return float4(ao, ao, ao, 1);
    float l = dot(c, float3(0.299, 0.587, 0.114));
    ao = lerp(ao, 1, smoothstep(0.65, 1.0, l) * opt.y);   // soft: keeps lamps from greying without popping
    ao = lerp(ao, 1, saturate(tex2D(sGlass, uv).r) * opt.z); // seen through glass: the glass isn't occluded
    ao = lerp(1, ao, lerp(1, saturate(tex2D(sFog, uv).r), opt.w)); // fogged distance: AO fades with the fog
    return float4(c * ao, 1);
}
)HLSL";

struct PostFX {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* psFxaa = nullptr;
    IDirect3DPixelShader9* psFxaaQ = nullptr;
    IDirect3DPixelShader9* psSSR = nullptr;
    IDirect3DPixelShader9* psSSS = nullptr;
    IDirect3DPixelShader9* psSharp = nullptr;
    IDirect3DPixelShader9* psAO = nullptr;
    IDirect3DPixelShader9* psAOBlur = nullptr;
    IDirect3DPixelShader9* psAOComp = nullptr;
    IDirect3DTexture9* L0 = nullptr;     // copy of the image being processed
    IDirect3DTexture9* L1 = nullptr;     // ping-pong
    IDirect3DTexture9* L2 = nullptr;
    IDirect3DTexture9* aoA = nullptr;    // AO + blur ping-pong
    IDirect3DTexture9* aoB = nullptr;
    IDirect3DTexture9* depth = nullptr;  // copy of HDR scene buffer (alpha = depth)
    IDirect3DStateBlock9* sb = nullptr;
    UINT w = 0, h = 0;
    bool shadersTried = false;
    bool enabled = true;
    bool keyDown = false;
    int  aoMode = 1;                     // 0 off, 1 on, 2 debug view
    bool aoKeyDown = false;
    bool depthValid = false;             // depth copied this frame
    // precise depth (INTZ)
    IDirect3DPixelShader9* psFit = nullptr;
    IDirect3DPixelShader9* psGrade = nullptr;
    IDirect3DPixelShader9* psZCopy = nullptr;
    IDirect3DTexture9* zcopy = nullptr;  // R32F snapshot of the 24-bit depth taken at the base pass
    bool  zValid = false;                // snapshot taken this frame
    int   fitFail = 0;                   // calibration attempts without a usable result
    IDirect3DTexture9* intz = nullptr;   // the game's scene depth buffer as a texture (weak: the game owns it)
    IDirect3DTexture9* fitRT = nullptr;  // 64x36 G32R32F calibration target
    IDirect3DSurface9* fitSys = nullptr; // system-memory copy for the CPU fit
    float fitA = 0, fitB = 0;
    bool  fitValid = false;
    int   fitCounter = 0;
    bool  fitLogged = false;
    bool  aoIsGtao = false;              // false = fallback shader (no INTZ support)
    bool depthLogged = false;
} g_fx;

// master switch and AO mode live in g_fx at runtime; take their saved values
static void ApplySavedToggles()
{
    g_fx.enabled = cfg.fxEnabled != 0;
    g_fx.aoMode  = cfg.aoMode;
}

// Our shaders can be compiled with Windows' own d3dcompiler_47 (System32, Windows 10+): much newer
// than the game's 2009 D3DX compiler, which crashed on 0.16.0's glass shader. It still writes SM3
// bytecode with a constant table, and its ID3DBlob has the same layout as ID3DXBuffer. Under Wine the
// system d3dcompiler_47 is a reimplementation that rejects some SM3 code, so auto keeps the game's.
// One lock around every compile: the game creates (and we rebuild) shaders on more than one thread.
typedef HRESULT (WINAPI *PFN_D3DCompile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                         UINT, UINT, void**, void**);
static PFN_D3DCompile g_d3dCompile = nullptr;
static int g_compilerUsed = -1;                    // 1 = d3dcompiler_47, 2 = the game's D3DX
static CRITICAL_SECTION g_compileCs;
static void* CompileBlob(const char* src, const char* profile, const char* name)
{
    static bool init = false;
    if (!init) {
        init = true;
        InitializeCriticalSection(&g_compileCs);
        const bool want47 = cfg.compiler == 1 || (cfg.compiler == 0 && !g_wineVer[0]);
        if (want47) {
            char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); strncat(p, "\\d3dcompiler_47.dll", MAX_PATH - strlen(p) - 1);
            if (HMODULE m = LoadLibraryA(p)) g_d3dCompile = (PFN_D3DCompile)GetProcAddress(m, "D3DCompile");
            Log("compiler: %s", g_d3dCompile ? "Windows' d3dcompiler_47 for APRenewal's shaders" : "d3dcompiler_47 not found - using the game's D3DX");
        } else Log("compiler: the game's D3DX for APRenewal's shaders (%s)", cfg.compiler == 2 ? "Compiler=game" : "Wine");
    }
    EnterCriticalSection(&g_compileCs);
    void* code = nullptr; void* err = nullptr;
    HRESULT hr = E_FAIL;
    if (g_d3dCompile) {
        // backwards compatibility: accept the legacy D3D9 syntax our sources were written for
        hr = g_d3dCompile(src, strlen(src), name, nullptr, nullptr, "main", profile, 1u << 12, 0, &code, &err);
        if (FAILED(hr) || !code) {
            Log("compiler: d3dcompiler_47 rejected %s (0x%08lx): %s - trying the game's", name, hr, err ? (const char*)BufPtr(err) : "");
            if (code) { BufRelease(code); code = nullptr; }
            if (err) { BufRelease(err); err = nullptr; }
        } else g_compilerUsed = 1;
    }
    if (!code) {
        PFN_D3DXCompileShader compile = o_D3DXCompileShader;
        if (!compile) {
            HMODULE dx = GetModuleHandleA("d3dx9_42.dll");
            if (dx) compile = (PFN_D3DXCompileShader)GetProcAddress(dx, "D3DXCompileShader");
        }
        if (!compile) { LeaveCriticalSection(&g_compileCs); Log("postfx: D3DXCompileShader unavailable"); return nullptr; }
        hr = compile(src, (UINT)strlen(src), nullptr, nullptr, "main", profile, 0, &code, &err, nullptr);
        if (FAILED(hr) || !code) {
            Log("postfx: %s compile failed 0x%08lx: %s", name, hr, err ? (const char*)BufPtr(err) : "");
            if (code) { BufRelease(code); code = nullptr; }
        } else if (g_compilerUsed < 0) g_compilerUsed = 2;
    }
    if (err) BufRelease(err);
    LeaveCriticalSection(&g_compileCs);
    return code;
}

static IDirect3DPixelShader9* CompilePS(IDirect3DDevice9* dev, const char* src, const char* name)
{
    void* code = CompileBlob(src, "ps_3_0", name);
    if (!code) return nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    HRESULT hr = dev->CreatePixelShader((const DWORD*)BufPtr(code), &ps);
    Log("postfx: %s shader %s", name, SUCCEEDED(hr) ? "ready" : "CreatePixelShader failed");
    BufRelease(code);
    return ps;
}

static IDirect3DVertexShader9* CompileVS(IDirect3DDevice9* dev, const char* src, const char* name)
{
    void* code = CompileBlob(src, "vs_3_0", name);
    if (!code) return nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    HRESULT hr = dev->CreateVertexShader((const DWORD*)BufPtr(code), &vs);
    Log("postfx: %s shader %s", name, SUCCEEDED(hr) ? "ready" : "CreateVertexShader failed");
    BufRelease(code);
    return vs;
}

static void PostFXReleaseDefault()
{
    IDirect3DTexture9** texs[] = { &g_fx.L0, &g_fx.L1, &g_fx.L2, &g_fx.aoA, &g_fx.aoB, &g_fx.depth, &g_fx.fitRT, &g_fx.zcopy };
    if (g_fx.fitSys) { g_fx.fitSys->Release(); g_fx.fitSys = nullptr; }
    for (auto t : texs) if (*t) { (*t)->Release(); *t = nullptr; }
    if (g_fx.sb) { g_fx.sb->Release(); g_fx.sb = nullptr; }
    g_fx.w = g_fx.h = 0;
    g_fx.depthValid = false; g_fx.zValid = false;
}

static void DrawFullscreen(IDirect3DDevice9* dev, IDirect3DTexture9* src, IDirect3DSurface9* dst,
                           IDirect3DPixelShader9* ps)
{
    struct V { float x, y, z, u, v; };
    static const V quad[4] = {
        { -1.f,  1.f, 0, 0, 0 }, { 1.f,  1.f, 0, 1, 0 },
        { -1.f, -1.f, 0, 0, 1 }, { 1.f, -1.f, 0, 1, 1 },
    };
    dev->SetRenderTarget(0, dst);
    dev->SetTexture(0, src);
    dev->SetPixelShader(ps);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
}

// Per-Present housekeeping: toggle hotkey, lazy shader compile.
static bool PostFXReady(IDirect3DDevice9* dev)
{
    if (!cfg.fxaa && cfg.sharpen <= 0 && !cfg.aoEnable && !cfg.gradeEnable) return false;
    if (!g_fx.enabled) return false;
    if (!g_fx.shadersTried) {
        g_fx.shadersTried = true;
        g_fx.vs = CompileVS(dev, kQuadVS, "Quad");
        // everything is compiled up front so the in-game menu can switch effects live
        if (g_fx.vs) g_fx.psFxaa  = CompilePS(dev, kFxaaPS, "FXAA");
        if (g_fx.vs) g_fx.psFxaaQ = CompilePS(dev, kFxaaQPS, "FXAA 3.11 quality");
        if (g_fx.vs) g_fx.psSSR   = CompilePS(dev, kSSRPS, "screen-space reflections");
        if (g_fx.vs) g_fx.psSSS   = CompilePS(dev, kSSSPS, "skin subsurface scattering");
        if (g_fx.vs) g_fx.psSharp = CompilePS(dev, kSharpenPS, "Sharpen");
        if (g_fx.vs) g_fx.psGrade = CompilePS(dev, kGradePS, "Colour grade");
        if (g_fx.vs) {
            g_fx.psAO     = CompilePS(dev, kAOPS, "SSAO (GTAO)");
            g_fx.aoIsGtao = g_fx.psAO != nullptr;
            if (!g_fx.psAO) g_fx.psAO = CompilePS(dev, kAOFallbackPS, "SSAO (fallback)");
            g_fx.psAOBlur = CompilePS(dev, kAOBlurPS, "SSAO blur");
            g_fx.psAOComp = CompilePS(dev, kAOCompositePS, "SSAO composite");
            g_fx.psFit    = CompilePS(dev, kFitPS, "depth calibration");
            g_fx.psZCopy  = CompilePS(dev, kZCopyPS, "depth snapshot");
        }
    }
    return g_fx.psFxaa || g_fx.psSharp || (g_fx.psAO && g_fx.psAOBlur && g_fx.psAOComp);
}

static void PostFXHotkey()
{
    bool down = (GetAsyncKeyState(cfg.toggleKey) & 0x8000) != 0
             && !(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    if (down && !g_fx.keyDown && (!g_hwnd || GetForegroundWindow() == g_hwnd)) {
        g_fx.enabled = !g_fx.enabled;
        Log("postfx %s", g_fx.enabled ? "ON" : "OFF");
    }
    g_fx.keyDown = down;

    if (cfg.aoEnable) {
        bool aDown = (GetAsyncKeyState(cfg.aoKey) & 0x8000) != 0;
        if (aDown && !g_fx.aoKeyDown && (!g_hwnd || GetForegroundWindow() == g_hwnd)) {
            g_fx.aoMode = (g_fx.aoMode + 1) % 3;
            static const char* names[] = { "OFF", "ON", "DEBUG VIEW" };
            Log("ssao %s", names[g_fx.aoMode]);
        }
        g_fx.aoKeyDown = aDown;
    }
}

static bool EnsureTargets(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bd)
{
    if (bd.Width == g_fx.w && bd.Height == g_fx.h && g_fx.L0 && g_fx.sb) return true;
    IDirect3DTexture9* keepDepth = g_fx.depth; g_fx.depth = nullptr;   // depth has its own size check
    PostFXReleaseDefault();
    g_fx.depth = keepDepth;
    auto mk = [&](IDirect3DTexture9** t, D3DFORMAT f) {
        return SUCCEEDED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, t, nullptr));
    };
    bool ok = mk(&g_fx.L0, bd.Format) && mk(&g_fx.L1, bd.Format) && mk(&g_fx.L2, bd.Format);
    if (ok && g_fx.psAO) ok = mk(&g_fx.aoA, D3DFMT_A8R8G8B8) && mk(&g_fx.aoB, D3DFMT_A8R8G8B8);
    if (ok) ok = SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &g_fx.sb));
    if (!ok) { Log("postfx: resource creation failed, disabling"); PostFXReleaseDefault(); g_fx.enabled = false; return false; }
    g_fx.w = bd.Width; g_fx.h = bd.Height;
    Log("postfx: resources %ux%u fmt=%d fxaa=%d sharpen=%d ssao=%d", bd.Width, bd.Height, bd.Format,
        g_fx.psFxaa != nullptr, cfg.sharpen, g_fx.psAO != nullptr);
    return true;
}

// Copy the HDR scene buffer right after the base pass: its alpha holds scene
// depth, which later passes (lights, translucency) may disturb.
static void CaptureDepth(IDirect3DDevice9* dev, IDirect3DSurface9* hdr)
{
    if (!cfg.aoEnable || !g_fx.enabled || g_fx.aoMode == 0 || g_fx.depthValid || !g_fx.psAO) return;
    D3DSURFACE_DESC d; hdr->GetDesc(&d);
    if (g_fx.depth) {
        D3DSURFACE_DESC dd; g_fx.depth->GetLevelDesc(0, &dd);
        if (dd.Width != d.Width || dd.Height != d.Height) { g_fx.depth->Release(); g_fx.depth = nullptr; }
    }
    if (!g_fx.depth &&
        FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_fx.depth, nullptr))) {
        Log("ssao: depth texture creation failed - disabling SSAO");
        g_fx.aoMode = 0; return;
    }
    IDirect3DSurface9* dst = nullptr;
    g_fx.depth->GetSurfaceLevel(0, &dst);
    g_inPostFX = true;
    HRESULT hr = dev->StretchRect(hdr, nullptr, dst, nullptr, D3DTEXF_NONE);
    g_inPostFX = false;
    dst->Release();
    g_fx.depthValid = SUCCEEDED(hr);
    if (!g_fx.depthLogged) {
        g_fx.depthLogged = true;
        Log(SUCCEEDED(hr) ? "ssao: depth source captured (%p %ux%u fmt=%d)" : "ssao: depth copy FAILED (%p %ux%u fmt=%d)",
            (void*)hdr, d.Width, d.Height, d.Format);
    }
}

// Copy the game's 24-bit depth (INTZ) into an R32F texture right after the base pass. Later in the
// frame the game may clear or reuse that buffer (the trace shows a depth clear before the bloom chain),
// so both the calibration (at Present) and GTAO read this snapshot instead of the live buffer.
// Runs mid-scene: no BeginScene of our own.
static void ZSnapshot(IDirect3DDevice9* dev)
{
    g_fx.zValid = false;
    if (!cfg.aoPrecise || !g_fx.intz || !g_fx.psZCopy || !g_fx.vs || !g_fx.sb) return;
    D3DSURFACE_DESC zd; g_fx.intz->GetLevelDesc(0, &zd);
    if (g_fx.zcopy) {
        D3DSURFACE_DESC cd; g_fx.zcopy->GetLevelDesc(0, &cd);
        if (cd.Width != zd.Width || cd.Height != zd.Height) { g_fx.zcopy->Release(); g_fx.zcopy = nullptr; }
    }
    if (!g_fx.zcopy &&
        FAILED(dev->CreateTexture(zd.Width, zd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_fx.zcopy, nullptr))) {
        Log("depth: R32F snapshot target unavailable - using 16-bit depth"); cfg.aoPrecise = 0; return;
    }
    g_inPostFX = true;
    g_fx.sb->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr, *rt = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    g_fx.zcopy->GetSurfaceLevel(0, &rt);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(nullptr);              // can't sample a bound depth buffer
    dev->SetVertexShader(g_fx.vs);
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    const float hp[4] = { -1.0f / zd.Width, 1.0f / zd.Height, 0, 0 };
    dev->SetVertexShaderConstantF(0, hp, 1);
    const DWORD rs[][2] = { { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
        { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
        { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF } };
    for (auto& r : rs) dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    D3DVIEWPORT9 vp = { 0, 0, zd.Width, zd.Height, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    DrawFullscreen(dev, g_fx.intz, rt, g_fx.psZCopy);
    dev->SetTexture(0, nullptr);
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    g_fx.sb->Apply();
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    rt->Release();
    g_inPostFX = false;
    g_fx.zValid = true;
}

// screen <-> world from a view-projection (row-major, world -> clip): (sx*w, sy*w, w) = wp * M3 + t,
// so wp = ((sx*w, sy*w, w) - t) * inverse(M3); the camera is where w = 0 and x = y = 0.
static bool VpInverse(const float vp[4][4], float M[3][3], float A[3][3], float t[3], float cam[3])
{
    for (int i = 0; i < 3; i++) { M[i][0] = vp[i][0]; M[i][1] = vp[i][1]; M[i][2] = vp[i][3]; }
    t[0] = vp[3][0]; t[1] = vp[3][1]; t[2] = vp[3][3];
    const float det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                    + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (fabsf(det) <= 1e-20f) return false;
    const float id = 1.0f / det;
    A[0][0] =  (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * id; A[0][1] = -(M[0][1] * M[2][2] - M[0][2] * M[2][1]) * id; A[0][2] =  (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * id;
    A[1][0] = -(M[1][0] * M[2][2] - M[1][2] * M[2][0]) * id; A[1][1] =  (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * id; A[1][2] = -(M[0][0] * M[1][2] - M[0][2] * M[1][0]) * id;
    A[2][0] =  (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * id; A[2][1] = -(M[0][0] * M[2][1] - M[0][1] * M[2][0]) * id; A[2][2] =  (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * id;
    for (int j = 0; j < 3; j++) cam[j] = -(t[0] * A[0][j] + t[1] * A[1][j] + t[2] * A[2][j]);
    return true;
}
static bool MatMaskValid() { return cfg.matEnable && g_matMask && g_matMaskFrame == GlassMaskKey(); }

// Run SSAO / FXAA / sharpen on `target` in place. `inScene`: called between
// the game's BeginScene/EndScene (mid-frame), so we must not open our own scene.
static void PostFXRun(IDirect3DDevice9* dev, IDirect3DSurface9* target, bool inScene)
{
    if (!PostFXReady(dev)) return;
    D3DSURFACE_DESC bd; target->GetDesc(&bd);
    if (bd.MultiSampleType != D3DMULTISAMPLE_NONE) return;
    const bool doAO = cfg.aoEnable && g_fx.aoMode != 0 && g_fx.depthValid && g_fx.depth && g_fx.psAO && g_fx.psAOBlur && g_fx.psAOComp;
    const bool doFxaa = cfg.fxaa && g_fx.psFxaa != nullptr, doSharp = cfg.sharpen > 0 && g_fx.psSharp != nullptr;
    const bool doGrade = cfg.gradeEnable && g_fx.psGrade != nullptr;
    // previous frame's finished HDR scene for the glass reflections (half res is plenty)
    if (cfg.glassEnable && cfg.glassSsr > 0 && g_hdrSceneSurf && g_vpKey / 64u == g_frameNo) {
        D3DSURFACE_DESC hd; g_hdrSceneSurf->GetDesc(&hd);
        D3DSURFACE_DESC pd = {};
        if (g_prevHdrSurf) g_prevHdrSurf->GetDesc(&pd);
        if (!g_prevHdr || pd.Width != std::max(hd.Width / 2, 1u) || pd.Height != std::max(hd.Height / 2, 1u) || pd.Format != hd.Format) {
            if (g_prevHdrSurf) { g_prevHdrSurf->Release(); g_prevHdrSurf = nullptr; }
            if (g_prevHdr) { g_prevHdr->Release(); g_prevHdr = nullptr; }
            if (SUCCEEDED(dev->CreateTexture(std::max(hd.Width / 2, 1u), std::max(hd.Height / 2, 1u), 1, D3DUSAGE_RENDERTARGET, hd.Format,
                                             D3DPOOL_DEFAULT, &g_prevHdr, nullptr))) {
                g_prevHdr->GetSurfaceLevel(0, &g_prevHdrSurf);
                Log("glass: previous-frame copy %ux%u (fmt %d) for reflections", hd.Width / 2, hd.Height / 2, hd.Format);
            } else { g_prevHdr = nullptr; Log("glass: previous-frame copy could not be created"); }
        }
        if (g_prevHdrSurf && SUCCEEDED(dev->StretchRect(g_hdrSceneSurf, nullptr, g_prevHdrSurf, nullptr, D3DTEXF_LINEAR))) {
            memcpy(g_vpPrev, g_vp, sizeof g_vp);
            g_prevHdrFrame = g_frameNo;
        }
    }
    const bool matOk = MatMaskValid();
    const bool doSSS = matOk && cfg.sssStrength > 0 && g_fx.psSSS && g_fx.depthValid && g_fx.depth;
    const bool doSSR = cfg.ssr && (cfg.ssrStrength > 0 || (matOk && cfg.metalRefl > 0)) && g_fx.psSSR && g_fx.depthValid && g_fx.depth && g_vpKey / 64u == g_frameNo;
    {
        static int lastMat = -1;
        if ((int)matOk != lastMat && g_matTaggedDraws) { lastMat = matOk;
            Log("materials: mask %s (%ld tagged draws so far)", matOk ? "available - skin scattering / metal reflections on" : "not drawn this frame", g_matTaggedDraws); }
    }
    if (cfg.ssr && cfg.ssrStrength > 0 && !doSSR) {
        static unsigned lastWhy = 0; unsigned why = (!g_fx.psSSR) | (!g_fx.depthValid) << 1 | (!g_fx.depth) << 2 | (g_vpKey / 64u != g_frameNo) << 3;
        if (why != lastWhy) { lastWhy = why;
            Log("ssr: not running -%s%s%s%s", why & 1 ? " shader not compiled" : "", why & 2 ? " depth not calibrated" : "",
                why & 4 ? " no depth" : "", why & 8 ? " no view-projection this frame" : ""); }
    }
    if (!doAO && !doFxaa && !doSharp && !doGrade && !doSSR && !doSSS) return;
    if (!EnsureTargets(dev, bd)) return;
    if (doAO && !g_fx.aoA) return;

    g_inPostFX = true;
    g_fx.sb->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);

    IDirect3DSurface9 *s0 = nullptr, *s1 = nullptr, *s2 = nullptr, *sa = nullptr, *sb2 = nullptr;
    g_fx.L0->GetSurfaceLevel(0, &s0);
    g_fx.L1->GetSurfaceLevel(0, &s1);
    g_fx.L2->GetSurfaceLevel(0, &s2);
    if (doAO) { g_fx.aoA->GetSurfaceLevel(0, &sa); g_fx.aoB->GetSurfaceLevel(0, &sb2); }
    dev->StretchRect(target, nullptr, s0, nullptr, D3DTEXF_NONE);

    if (inScene || SUCCEEDED(dev->BeginScene())) {
        dev->SetDepthStencilSurface(nullptr);
        dev->SetVertexShader(g_fx.vs);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        const float hp[4] = { -1.0f / bd.Width, 1.0f / bd.Height, 0, 0 };
        dev->SetVertexShaderConstantF(0, hp, 1);
        const DWORD rs[][2] = {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
            { D3DRS_COLORWRITEENABLE, 0xF },
        };
        for (auto& r : rs) dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
        for (DWORD st = 0; st < 2; st++) {
            const DWORD filt = st == 0 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            dev->SetSamplerState(st, D3DSAMP_MINFILTER, filt);
            dev->SetSamplerState(st, D3DSAMP_MAGFILTER, filt);
            dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        D3DVIEWPORT9 vp = { 0, 0, bd.Width, bd.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        const float rcp[4] = { 1.0f / bd.Width, 1.0f / bd.Height, (float)bd.Width, (float)bd.Height };
        const float prm[4] = { cfg.sharpen / 100.0f, 0, 0, 0 };
        dev->SetPixelShaderConstantF(0, rcp, 1);
        dev->SetPixelShaderConstantF(1, prm, 1);

        int remaining = (doAO ? 1 : 0) + (doSSR ? 1 : 0) + (doGrade ? 1 : 0) + (doFxaa ? 1 : 0) + (doSharp ? 1 : 0);
        IDirect3DTexture9* cur = g_fx.L0;
        // next output: the real target for the last pass, otherwise whichever ping-pong buffer isn't `cur`
        auto nextOut = [&](IDirect3DTexture9** asTex) -> IDirect3DSurface9* {
            if (--remaining == 0) { *asTex = nullptr; return target; }
            if (cur == g_fx.L1) { *asTex = g_fx.L2; return s2; }
            *asTex = g_fx.L1; return s1;
        };
        IDirect3DTexture9* outTex = nullptr;

        if (doAO) {
            const bool liveFov = cfg.aoFovAuto && g_gameFov > 10.0f && GetTickCount() - g_gameFovTick < 2000;
            const float tx = tanf((liveFov ? g_gameFov : (float)cfg.aoFov) * 0.5f * 3.14159265f / 180.0f);
            const float c2[4] = { tx, tx * bd.Height / bd.Width, (float)cfg.aoRadius, cfg.aoIntensity / 100.0f };
            const float c3[4] = { 0.2f, (float)cfg.aoMaxDist, cfg.aoMaxDist * 0.6f, (float)bd.Width / bd.Height };
            const bool glassNow = g_glassMask && g_glassMaskFrame == GlassMaskKey();   // only this frame's final scene
            const bool fogNow = g_fogMask && g_fogMaskFrame == GlassMaskKey();
            const float c4[4] = { g_fx.aoMode == 2 ? 1.0f : 0.0f, cfg.aoProtect / 100.0f, glassNow ? cfg.aoGlass / 100.0f : 0.0f,
                                  fogNow ? cfg.aoFog / 100.0f : 0.0f };
            const float c5[4] = { (float)cfg.aoNear, 0, 0, 0 };
            dev->SetPixelShaderConstantF(5, c5, 1);
            const bool precise = g_fx.aoIsGtao && cfg.aoPrecise && g_fx.zcopy && g_fx.zValid && g_fx.fitValid;
            const float c6[4] = { precise ? 1.0f : 0.0f, g_fx.fitA, g_fx.fitB, precise ? 1.0f : 2.0f };
            dev->SetPixelShaderConstantF(6, c6, 1);
            dev->SetPixelShaderConstantF(2, c2, 1);
            dev->SetPixelShaderConstantF(3, c3, 1);
            dev->SetPixelShaderConstantF(4, c4, 1);
            dev->SetTexture(1, precise ? (IDirect3DBaseTexture9*)g_fx.zcopy : g_fx.depth);
            DrawFullscreen(dev, nullptr, sa, g_fx.psAO);                       // AO -> aoA
            dev->SetTexture(1, g_fx.depth);                                    // blur uses the HDR-alpha copy
            const float dh[4] = { 1.0f / bd.Width, 0, 0, 0 }, dv[4] = { 0, 1.0f / bd.Height, 0, 0 };
            dev->SetPixelShaderConstantF(1, dh, 1);
            DrawFullscreen(dev, g_fx.aoA, sb2, g_fx.psAOBlur);                // blur H -> aoB
            dev->SetPixelShaderConstantF(1, dv, 1);
            DrawFullscreen(dev, g_fx.aoB, sa, g_fx.psAOBlur);                 // blur V -> aoA
            dev->SetPixelShaderConstantF(1, prm, 1);
            dev->SetTexture(1, g_fx.aoA);
            dev->SetTexture(2, glassNow ? (IDirect3DBaseTexture9*)g_glassMask : nullptr);
            dev->SetTexture(3, fogNow ? (IDirect3DBaseTexture9*)g_fogMask : nullptr);
            IDirect3DSurface9* o = nextOut(&outTex);
            DrawFullscreen(dev, cur, o, g_fx.psAOComp);                       // color * AO
            dev->SetTexture(1, nullptr);
            dev->SetTexture(2, nullptr);
            dev->SetTexture(3, nullptr);
            if (outTex) cur = outTex;
        }
        const bool precise = g_fx.aoIsGtao && cfg.aoPrecise && g_fx.zcopy && g_fx.zValid && g_fx.fitValid;
        if (doSSS) {
            float focal = 0.6f * bd.Height;                          // ~ 80 degree view if the matrix is missing
            if (g_vpKey / 64u == g_frameNo)
                focal = sqrtf(g_vp[0][1] * g_vp[0][1] + g_vp[1][1] * g_vp[1][1] + g_vp[2][1] * g_vp[2][1]) * 0.5f * bd.Height;
            const float d6[4] = { precise ? 1.0f : 0.0f, g_fx.fitA, g_fx.fitB, 0 };
            dev->SetPixelShaderConstantF(6, d6, 1);
            dev->SetTexture(1, precise ? (IDirect3DBaseTexture9*)g_fx.zcopy : g_fx.depth);
            dev->SetTexture(2, g_matMask);
            for (int pass = 0; pass < 2; pass++) {
                const float c7[4] = { pass ? 0.0f : 1.0f / bd.Width, pass ? 1.0f / bd.Height : 0.0f,
                                      1.0f * std::max(cfg.sssWidth, 10) / 100.0f * focal, std::max(cfg.sssStrength, 0) / 100.0f };
                dev->SetPixelShaderConstantF(7, c7, 1);
                IDirect3DSurface9* o = nextOut(&outTex);
                DrawFullscreen(dev, cur, o, g_fx.psSSS);
                if (outTex) cur = outTex;
            }
            dev->SetTexture(1, nullptr); dev->SetTexture(2, nullptr);
            static bool logged = false;
            if (!logged) { logged = true; Log("postfx: skin subsurface scattering running (focal %.0f px)", focal); }
        }
        if (doSSR) {
            float M[3][3], t[3], A[3][3], cam[3];
            if (VpInverse(g_vp, M, A, t, cam)) {
                float c[19][4] = {};
                c[6][0] = precise ? 1.0f : 0.0f; c[6][1] = g_fx.fitA; c[6][2] = g_fx.fitB;
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) c[8 + i][j] = A[i][j];
                for (int j = 0; j < 3; j++) c[11][j] = cam[j];
                for (int i = 0; i < 3; i++) { c[12 + i][0] = M[i][0]; c[12 + i][1] = M[i][1]; c[12 + i][2] = M[i][2]; }
                c[15][0] = t[0]; c[15][1] = t[1]; c[15][2] = t[2];
                c[16][0] = cfg.ssrStrength / 100.0f; c[16][1] = (float)std::max(cfg.ssrLength, 100);
                c[16][2] = 40.0f; c[16][3] = std::min(std::max(cfg.ssrFloor, 0), 99) / 100.0f;
                dev->SetPixelShaderConstantF(6, c[6], 1);
                c[17][0] = matOk ? std::max(cfg.metalRefl, 0) / 100.0f : 0.0f; c[17][1] = matOk ? 1.0f : 0.0f;
                c[18][0] = std::max(cfg.ssrRough, 0) / 100.0f * 0.25f; c[18][1] = std::max(cfg.metalRough, 0) / 100.0f * 0.25f;
                if (!cfg.ssr || cfg.ssrStrength <= 0) c[16][0] = 0;
                dev->SetPixelShaderConstantF(8, c[8], 11);
                dev->SetTexture(1, precise ? (IDirect3DBaseTexture9*)g_fx.zcopy : g_fx.depth);
                dev->SetTexture(2, matOk ? (IDirect3DBaseTexture9*)g_matMask : nullptr);
                IDirect3DSurface9* o = nextOut(&outTex);
                DrawFullscreen(dev, cur, o, g_fx.psSSR);
                dev->SetTexture(1, nullptr); dev->SetTexture(2, nullptr);
                if (outTex) cur = outTex;
                static bool logged = false;
                if (!logged) { logged = true; Log("postfx: screen-space reflections running (camera at %.0f %.0f %.0f)", c[11][0], c[11][1], c[11][2]); }
            } else {
                const float off[3][4] = { { 0, 1, 1, 1.5f }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };   // threshold above 1, no metal: colour unchanged
                dev->SetPixelShaderConstantF(16, off[0], 3);
                IDirect3DSurface9* o = nextOut(&outTex);   // keep the chain intact
                DrawFullscreen(dev, cur, o, g_fx.psSSR);
                if (outTex) cur = outTex;
            }
        }
        if (doGrade) {
            const float g0[4] = { powf(2.0f, cfg.gradeExposure / 100.0f), cfg.gradeContrast / 100.0f,
                                  cfg.gradeVibrance / 100.0f, cfg.gradeSaturation / 100.0f };
            const float g1[4] = { cfg.gradeBlack / 1000.0f, cfg.gradeTemp / 100.0f, 0, 0 };
            dev->SetPixelShaderConstantF(2, g0, 1);
            dev->SetPixelShaderConstantF(3, g1, 1);
            IDirect3DSurface9* o = nextOut(&outTex);
            DrawFullscreen(dev, cur, o, g_fx.psGrade);
            if (outTex) cur = outTex;
        }
        if (doFxaa) {
            IDirect3DSurface9* o = nextOut(&outTex);
            DrawFullscreen(dev, cur, o, cfg.fxaaQuality && g_fx.psFxaaQ ? g_fx.psFxaaQ : g_fx.psFxaa);
            if (outTex) cur = outTex;
        }
        if (doSharp) {
            IDirect3DSurface9* o = nextOut(&outTex);
            DrawFullscreen(dev, cur, o, g_fx.psSharp);
            if (outTex) cur = outTex;
        }
        dev->SetTexture(0, nullptr);
        if (!inScene) dev->EndScene();
    }

    // restore (render target first: SetRenderTarget resets the viewport, the state block puts it back)
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    g_fx.sb->Apply();
    IDirect3DSurface9* rel[] = { oldRT, oldDS, s0, s1, s2, sa, sb2 };
    for (auto r : rel) if (r) r->Release();
    g_inPostFX = false;
}

// --------------------------------------------------------------------------
// Before-HUD PostFX
//
// From frame captures: the world goes HDR scene -> bloom chain (quarter-res
// targets) -> one full-res LDR target (same size/format as the backbuffer).
// After that the frame either goes straight to the backbuffer (gameplay) or
// the UI pass starts (image copied back to HDR, widgets drawn, UI post).
// So: once a quarter-res target has been drawn to, the next full-res
// backbuffer-format target is the finished world; when the game switches
// away from it, we run PostFX on it in place - before any UI pixel exists.
// --------------------------------------------------------------------------
static IDirect3DSurface9* g_bbPtr = nullptr;   // identity only, not AddRef'd
static UINT      g_bbW = 0, g_bbH = 0;
static D3DFORMAT g_bbFmt = D3DFMT_UNKNOWN;
static IDirect3DSurface9* g_curRT = nullptr;   // identity only
static int  g_curRTDraws = 0;
static bool g_curRTSmall = false, g_sawSmallRT = false, g_fxDoneThisFrame = false;
static bool g_curRTIsHDR = false;
static bool g_multiSceneLogged = false;
static bool g_sawHDR = false;                  // the base pass (full-res HDR) has been drawn this frame
static char g_frameSeq[1024];                  // compact RT sequence of the current frame (diagnostics)
static int  g_frameSeqLen = 0;
static bool g_skipLogged = false;
static int  g_statFrames = 0, g_statFx = 0, g_statDepth = 0;
static DWORD g_statStart = 0;
static int  g_lastStatFrames = 0, g_lastStatFx = 0, g_lastStatDepth = 0;
// how each frame got (or didn't get) its effects: 0 world target on switch, 1 world target at Present,
// 2 no-bloom fallback, 3 skipped 3D frame, 4 no 3D (pure UI / loading)
static int  g_route[6] = {}, g_lastRoute[6] = {};   // [5] = effects switched off
static int  g_frameRoute = 4;
static IDirect3DSurface9* g_worldRT = nullptr; // identity only
static bool g_worldLogged = false;

static void BeforeHudOnSetRT(IDirect3DDevice9* dev, IDirect3DSurface9* next)
{
    if (next == g_curRT) return;
    // leaving g_curRT
    // A depth pre-pass (many draws into a full-res LDR target, before any HDR) starts a NEW SCENE.
    // Some frames render the world twice: first into a scene-capture texture (reflections / screens),
    // then the real view. Everything is reset so depth and the world target come from the last scene.
    if (g_curRT && g_curRTDraws >= 80 && g_curRT != g_bbPtr && g_bbW) {
        D3DSURFACE_DESC pd; g_curRT->GetDesc(&pd);
        if (pd.Width == g_bbW && pd.Height == g_bbH && pd.Format == g_bbFmt && (g_sawHDR || g_sawSmallRT)) {
            g_sawHDR = false; g_sawSmallRT = false; g_worldRT = nullptr;
            g_fx.depthValid = false; g_fx.zValid = false; g_fxDoneThisFrame = false;
            g_scenesThisFrame++;
            if (!g_multiSceneLogged) {
                g_multiSceneLogged = true;
                Log("postfx: frame renders a second scene (scene capture?) - using the last one");
            }
            g_curRTDraws = 0;   // the pre-pass target is not the world image
        }
    }
    // Order within a frame: [misc small passes] -> base pass (full-res HDR) -> lights/translucency ->
    // bloom chain (small targets) -> finished world (full-res LDR). Small targets only count as the
    // bloom chain once the base pass has happened; some scenes render small targets before it.
    if (g_curRT && g_curRTDraws > 0 && g_curRTIsHDR) {
        if (!g_sawHDR) {
            if (!g_worldEverSeen) { g_worldEverSeen = true; Log("movie: first 3D scene rendered - movies play normally from now on"); }
            CaptureDepth(dev, g_curRT);                  // first full-res HDR pass of the scene = base pass
            g_hdrSceneSurf = g_curRT;                    // the scene's HDR colour (HDR output reads it)
            g_fx.intz = nullptr;
            IDirect3DSurface9* ds = nullptr;
            if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) {
                for (auto& e : g_intzTab) if (e.surf == ds) g_fx.intz = e.tex;
                ds->Release();
            }
            if (g_fx.depthValid) ZSnapshot(dev);
        }
        g_sawHDR = true;
    }
    if (g_curRT && g_curRTDraws > 0 && g_curRTSmall && g_sawHDR) g_sawSmallRT = true;
    if (g_worldRT && g_curRT == g_worldRT) {
        if (g_curRTDraws > 0 && !g_fxDoneThisFrame && cfg.beforeHud && g_fx.enabled) {
            if (!g_worldLogged) {
                g_worldLogged = true;
                Log("postfx: before-HUD target found (%p %ux%u) - applying before the UI", (void*)g_worldRT, g_bbW, g_bbH);
            }
            g_inPostFX = true;
            PostFXRun(dev, g_worldRT, true);
            g_inPostFX = false;
            g_fxDoneThisFrame = true;
            g_frameRoute = 0;
        }
        // HDR: the world goes straight to the backbuffer next (gameplay) - expand its highlights
        // right after that copy, before any UI is drawn over it
        if (g_curRTDraws > 0 && next && next == g_bbPtr && g_curRT != g_bbPtr && g_hdrActive) g_hdrArm = true;
        g_worldRT = nullptr;
    }
    // record the segment we're leaving (diagnostics for skipped frames)
    if (g_curRT && g_curRTDraws > 0 && g_frameSeqLen < (int)sizeof(g_frameSeq) - 40) {
        D3DSURFACE_DESC ld; g_curRT->GetDesc(&ld);
        g_frameSeqLen += snprintf(g_frameSeq + g_frameSeqLen, sizeof(g_frameSeq) - g_frameSeqLen, "%s%ux%u/%d:%d ",
                                  g_curRT == g_bbPtr ? "BB " : "", ld.Width, ld.Height, ld.Format, g_curRTDraws);
    }
    // entering next
    g_curRT = next;
    g_curRTDraws = 0;
    g_curRTSmall = false;
    g_curRTIsHDR = false;
    if (next && g_bbW) {
        D3DSURFACE_DESC d; next->GetDesc(&d);
        g_curRTSmall = d.Width <= g_bbW / 3;   // bloom chain is quarter-res; 1024x1024 capture textures don't count
        g_curRTIsHDR = d.Format == D3DFMT_A16B16G16R16F && d.Width == g_bbW && d.Height == g_bbH && next != g_bbPtr;
        if (g_sawSmallRT && !g_fxDoneThisFrame && !g_worldRT &&
            d.Width == g_bbW && d.Height == g_bbH && d.Format == g_bbFmt)
            g_worldRT = next;   // usually the LDR world target; can be the backbuffer itself when UE3
                                // skips a post step and writes the final pass straight to the screen
    }
}

// ==========================================================================
// Extras: supersampling downscale, HDR output, shared fullscreen pass helper
// ==========================================================================
static IDirect3DVertexShader9* g_xvs = nullptr;
static IDirect3DPixelShader9 *g_psDown = nullptr, *g_psHdrFit = nullptr, *g_psHdrExpand = nullptr, *g_psHdrEncode = nullptr, *g_psHdrPattern = nullptr;
static bool g_hdrPattern = false;   // menu: replace the frame with the HDR test pattern
static bool g_hdrPatternHi = false; // menu: bottom row shows steps above paper white
static bool g_xTried = false;
static IDirect3DStateBlock9* g_xsb = nullptr;
static IDirect3DTexture9 *g_xCopy = nullptr, *g_xScene = nullptr, *g_xFitRT = nullptr;
static IDirect3DSurface9* g_xFitSys = nullptr;

static void ExtrasRelease()
{
    IDirect3DTexture9** t[] = { &g_xCopy, &g_xScene, &g_xFitRT };
    for (auto p : t) if (*p) { (*p)->Release(); *p = nullptr; }
    if (g_xFitSys) { g_xFitSys->Release(); g_xFitSys = nullptr; }
    if (g_xsb) { g_xsb->Release(); g_xsb = nullptr; }
}

static bool ExtrasReady(IDirect3DDevice9* dev)
{
    if (!g_xTried) {
        g_xTried = true;
        g_xvs = CompileVS(dev, kQuadVS, "Quad (extras)");
        if (g_xvs) {
            g_psDown      = CompilePS(dev, kDownPS, "supersampling downscale");
            g_psHdrFit    = CompilePS(dev, kHdrFitPS, "HDR exposure fit");
            g_psHdrExpand = CompilePS(dev, kHdrExpandPS, "HDR highlight expansion");
            g_psHdrEncode = CompilePS(dev, kHdrEncodePS, "HDR scRGB encode");
            g_psHdrPattern = CompilePS(dev, kHdrPatternPS, "HDR test pattern");
        }
    }
    return g_xvs != nullptr;
}

// texture of size w x h and format f in *t (recreated when it doesn't match)
static bool EnsureRT(IDirect3DDevice9* dev, IDirect3DTexture9** t, UINT w, UINT h, D3DFORMAT f)
{
    if (*t) {
        D3DSURFACE_DESC d; (*t)->GetLevelDesc(0, &d);
        if (d.Width == w && d.Height == h && d.Format == f) return true;
        (*t)->Release(); *t = nullptr;
    }
    return SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, t, nullptr));
}

static bool CopyToTex(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DTexture9** t)
{
    D3DSURFACE_DESC d; src->GetDesc(&d);
    if (!EnsureRT(dev, t, d.Width, d.Height, d.Format)) return false;
    IDirect3DSurface9* dst = nullptr; (*t)->GetSurfaceLevel(0, &dst);
    const bool was = g_inPostFX; g_inPostFX = true;
    HRESULT hr = dev->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_NONE);
    g_inPostFX = was;
    dst->Release();
    return SUCCEEDED(hr);
}

// One fullscreen pass into `dst`, drawing only the vw x vh corner. s0/s1 textures, c1/c2 constants.
static void XPass(IDirect3DDevice9* dev, IDirect3DSurface9* dst, UINT vw, UINT vh,
                  IDirect3DBaseTexture9* t0, bool lin0, IDirect3DBaseTexture9* t1, bool lin1,
                  IDirect3DPixelShader9* ps, const float* c1, const float* c2, bool inScene, const float* c3 = nullptr)
{
    if (!g_xvs || !ps) return;
    if (!g_xsb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_xsb))) return;
    const bool was = g_inPostFX; g_inPostFX = true;
    g_xsb->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    if (inScene || SUCCEEDED(dev->BeginScene())) {
        dev->SetRenderTarget(0, dst);
        dev->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp = { 0, 0, vw, vh, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->SetVertexShader(g_xvs);
        dev->SetPixelShader(ps);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        const float hp[4] = { -1.0f / vw, 1.0f / vh, 0, 0 };
        dev->SetVertexShaderConstantF(0, hp, 1);
        const DWORD rs[][2] = { { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
            { D3DRS_COLORWRITEENABLE, 0xF } };
        for (auto& r : rs) dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
        IDirect3DBaseTexture9* tx[2] = { t0, t1 };
        const bool lin[2] = { lin0, lin1 };
        for (DWORD st = 0; st < 2; st++) {
            const DWORD f = lin[st] ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            dev->SetSamplerState(st, D3DSAMP_MINFILTER, f);
            dev->SetSamplerState(st, D3DSAMP_MAGFILTER, f);
            dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(st, tx[st]);
        }
        if (c1) dev->SetPixelShaderConstantF(1, c1, 1);
        if (c2) dev->SetPixelShaderConstantF(2, c2, 1);
        if (c3) dev->SetPixelShaderConstantF(3, c3, 1);
        struct V { float x, y, z, u, v; };
        static const V quad[4] = { { -1.f, 1.f, 0, 0, 0 }, { 1.f, 1.f, 0, 1, 0 }, { -1.f, -1.f, 0, 0, 1 }, { 1.f, -1.f, 0, 1, 1 } };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        if (!inScene) dev->EndScene();
    }
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    g_xsb->Apply();
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    g_inPostFX = was;
}

// Windows' "SDR content brightness" for the monitor the game is on (what the desktop uses as white)
static float g_winSdrWhite = 0;
static void QueryWindowsSdrWhite()
{
    if (!g_hwnd) return;
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
    DISPLAYCONFIG_PATH_INFO* paths = (DISPLAYCONFIG_PATH_INFO*)calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO* modes = (DISPLAYCONFIG_MODE_INFO*)calloc(nm, sizeof *modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof src;
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice)) continue;
            struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; ULONG SDRWhiteLevel; } wl = {};
            wl.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;      // DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL
            wl.header.size = sizeof wl;
            wl.header.adapterId = paths[i].targetInfo.adapterId;
            wl.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel)
                g_winSdrWhite = wl.SDRWhiteLevel * 80.0f / 1000.0f;
            break;
        }
    }
    free(paths); free(modes);
    Log("hdr: Windows SDR content brightness %.0f nits%s", g_winSdrWhite, g_winSdrWhite ? "" : " (unknown - using 200)");
}
// Which GPU drives the game's monitor, and is Windows HDR really on there? A monitor plugged into
// the iGPU while the dGPU renders goes through a cross-adapter copy, which can drop the HDR colour
// space - HDR values then get clipped at SDR white (neon, clipped look).
static char g_hdrDiag[256] = "";
static void DiagnoseHdrOutput()
{
    HMODULE dx = LoadLibraryA("dxgi.dll");
    if (!dx || !g_hwnd) return;
    typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void**);
    PFN_CF1 cf = (PFN_CF1)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    if (!cf || FAILED(cf(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return;
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 ad; a->GetDesc1(&ad);
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od; o->GetDesc(&od);
            if (od.Monitor == mon) {
                IDXGIOutput6* o6 = nullptr;
                DXGI_OUTPUT_DESC1 d1 = {};
                const bool have6 = SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && o6 &&
                                   SUCCEEDED(o6->GetDesc1(&d1));
                const bool hdrOn = have6 && d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                char gpu[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, gpu, sizeof gpu, nullptr, nullptr);
                snprintf(g_hdrDiag, sizeof g_hdrDiag, "monitor on %s, Windows HDR %s", gpu,
                         !have6 ? "unknown" : hdrOn ? "ON" : "OFF");
                Log("hdr: %s (colour space %d, %.0f nits peak)", g_hdrDiag, have6 ? (int)d1.ColorSpace : -1, have6 ? d1.MaxLuminance : 0.0f);
                if (have6 && !hdrOn) Log("hdr: Windows HDR is off for this monitor - HDR output will be clipped to SDR");
                if (o6) o6->Release();
            }
            o->Release();
        }
        a->Release();
    }
    f->Release();
}

static float HdrPaperNits()
{
    if (cfg.hdrReference || cfg.hdrPaper == 0) return g_winSdrWhite > 0 ? g_winSdrWhite : 200.0f;
    return (float)cfg.hdrPaper;
}

static float HdrPeakNits()
{
    float p = cfg.hdrPeak > 0 ? (float)cfg.hdrPeak : (g_hdrMaxNits > 100 ? g_hdrMaxNits : 1000.0f);
    return p < HdrPaperNits() ? HdrPaperNits() : p;
}

// Exposure fit: median of display/scene luminance over mid-tones (below the game's tonemap shoulder)
static void HdrFit(IDirect3DDevice9* dev)
{
    const UINT FW = 64, FH = 36;
    if (!g_psHdrFit || !EnsureRT(dev, &g_xFitRT, FW, FH, D3DFMT_G32R32F)) return;
    if (!g_xFitSys && FAILED(dev->CreateOffscreenPlainSurface(FW, FH, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &g_xFitSys, nullptr))) return;
    IDirect3DSurface9* rt = nullptr; g_xFitRT->GetSurfaceLevel(0, &rt);
    XPass(dev, rt, FW, FH, g_xCopy, false, g_xScene, false, g_psHdrFit, nullptr, nullptr, true);
    const bool ok = SUCCEEDED(dev->GetRenderTargetData(rt, g_xFitSys));
    rt->Release();
    D3DLOCKED_RECT lr;
    if (!ok || FAILED(g_xFitSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    static float r[64 * 36];
    int n = 0;
    for (UINT y = 0; y < FH; y++) {
        const float* row = (const float*)((const BYTE*)lr.pBits + y * lr.Pitch);
        for (UINT x = 0; x < FW; x++) {
            const float yl = row[x * 2], yh = row[x * 2 + 1];
            if (yl > 0.02f && yl < 0.4f && yh > 1e-5f && yh < 1e5f) r[n++] = yl / yh;
        }
    }
    g_xFitSys->UnlockRect();
    if (n < 40) return;
    std::nth_element(r, r + n / 2, r + n);
    const float k = r[n / 2];
    static bool logged = false;
    g_hdrK = g_hdrK > 0 ? expf(logf(g_hdrK) * 0.7f + logf(k) * 0.3f) : k;
    if (!logged) { logged = true; Log("hdr: exposure fitted (%.4g from %d samples)", k, n); }
}

// Right after the game copies the finished world into the (fp16) backbuffer, before any UI:
// grow highlights from the game's own HDR scene buffer.
static void HdrExpand(IDirect3DDevice9* dev)
{
    if (!g_hdrActive || !cfg.hdrEnable || cfg.hdrHighlights <= 0 || !g_hdrSceneSurf || !g_bbPtr) return;
    if (!ExtrasReady(dev) || !g_psHdrExpand) return;
    D3DSURFACE_DESC bd, sd; g_bbPtr->GetDesc(&bd); g_hdrSceneSurf->GetDesc(&sd);
    if (bd.Width != sd.Width || bd.Height != sd.Height) return;
    if (!CopyToTex(dev, g_bbPtr, &g_xCopy) || !CopyToTex(dev, g_hdrSceneSurf, &g_xScene)) return;
    if (g_hdrK <= 0 || (g_hdrFrames++ % 10) == 0) HdrFit(dev);
    if (g_hdrK <= 0) return;
    if (cfg.hdrReference) return;
    const float c1[4] = { g_hdrK, HdrPeakNits() / HdrPaperNits(), cfg.hdrHighlights / 100.0f, 0.55f };
    XPass(dev, g_bbPtr, bd.Width, bd.Height, g_xCopy, false, g_xScene, false, g_psHdrExpand, c1, nullptr, true);
}

// At Present: HDR encode (gamma -> scRGB), then the supersampling downscale. Returns the source
// rectangle to present (null = whole backbuffer).
static const RECT* PresentExtras(IDirect3DDevice9* dev)
{
    if (!g_hdrActive && !g_ssaa) return nullptr;
    if (!ExtrasReady(dev)) return nullptr;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return nullptr;
    D3DSURFACE_DESC bd; bb->GetDesc(&bd);
    // downscale first (gamma-encoded, filtered in linear light), then encode for HDR
    static RECT src;
    const RECT* ret = nullptr;
    UINT iw = bd.Width, ih = bd.Height;
    if (g_ssaa && g_psDown && g_outW && g_outH && bd.Width > g_outW && CopyToTex(dev, bb, &g_xCopy)) {
        const float dp[4] = { 1.0f / bd.Width, 1.0f / bd.Height, (float)bd.Width / g_outW, (float)bd.Height / g_outH };
        const float dq[4] = { 1.0f, 0, 0, 0 };
        XPass(dev, bb, g_outW, g_outH, g_xCopy, true, nullptr, false, g_psDown, dp, dq, false);
        src = { 0, 0, (LONG)g_outW, (LONG)g_outH };
        ret = &src;
        iw = g_outW; ih = g_outH;
    }
    if (g_hdrActive && g_psHdrEncode && CopyToTex(dev, bb, &g_xCopy)) {
        const bool ref = cfg.hdrReference || g_hdrPattern;
        const float e[4] = { HdrPaperNits(), HdrPeakNits(), g_hdrPQ ? 1.0f : 0.0f,
                             (ref || cfg.hdrCurve == 0) ? 1.0f : 0.0f };
        const float e2[4] = { (float)iw / bd.Width, (float)ih / bd.Height, g_hdrPattern ? 1.0f : g_gammaExp,
                              (cfg.hdrReference || g_hdrPattern) ? 1.0f : cfg.hdrSaturation / 100.0f };
        const float e3[4] = { cfg.hdrCompensate ? 1.0f : 0.0f, 0, 0, 0 };
        XPass(dev, bb, iw, ih, g_xCopy, false, nullptr, false, g_psHdrEncode, e, e2, false, e3);
    }
    bb->Release();
    return ret;
}

// DXVK's D3D9 extension interfaces (dxvk/src/d3d9/d3d9_interfaces.h)
static const GUID IID_D3D9VkExtInterface = { 0x65b55086, 0xe3e3, 0x4c3e, { 0xb3, 0xa0, 0x86, 0x81, 0x5c, 0xce, 0x2c, 0x4c } };
static const GUID IID_D3D9VkExtSwapchain = { 0x13776e93, 0x4aa9, 0x430a, { 0xa4, 0xec, 0xfe, 0x9e, 0x28, 0x11, 0x81, 0xd5 } };
struct D3D9VkExtOutputMetadata_ { float primaries[8]; float minLum, maxLum, maxFullFrameLum; };
struct IVkExtInterface_ : IUnknown { virtual void STDMETHODCALLTYPE UnlockAdditionalFormats() = 0; };
struct IVkExtSwapchain_ : IUnknown {
    virtual BOOL    STDMETHODCALLTYPE CheckColorSpaceSupport(int cs) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace(int cs) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(const void* md) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentOutputDesc(D3D9VkExtOutputMetadata_* d) = 0;
    virtual void    STDMETHODCALLTYPE UnlockAdditionalFormats() = 0;
};
static const int VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT_ = 1000104002;
static const int VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_ = 0;
static const int VK_COLOR_SPACE_HDR10_ST2084_EXT_ = 1000104008;

// Native Windows: the DXVK path never got brighter than SDR white on the tested PC (0.9.7 highlight
// test), so HDR is a Proton/gamescope feature unless [HDR] AllowWindows=1.
static bool HdrAllowed() { return g_wineVer[0] || cfg.hdrWindows; }

static void HdrUnlock(IDirect3D9* d3d)
{
    if (!cfg.hdrEnable || !d3d) return;
    if (!HdrAllowed()) {
        static bool once; if (!once) { once = true; Log("hdr: off on native Windows (no gain over SDR here; [HDR] AllowWindows=1 to try anyway)"); }
        return;
    }
    IVkExtInterface_* ext = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(IID_D3D9VkExtInterface, (void**)&ext)) && ext) {
        ext->UnlockAdditionalFormats();
        ext->Release();
        g_hdrUnlocked = true;
        Log("hdr: DXVK detected - fp16 backbuffer unlocked");
    } else {
        Log("hdr: HDR output needs DXVK as RealD3D9 (this d3d9 has no DXVK extension interface) - staying SDR");
    }
}

// DXVK reports HDR colour spaces as supported even when Vulkan can't present them: it then falls
// back to an SDR surface and clips our HDR values channel by channel. Ask the Vulkan loader directly.
static char g_vkPath[MAX_PATH] = "";
static int  g_vkHdrExt = -1;   // VK_EXT_swapchain_colorspace offered by the loader: -1 unknown, 0 no, 1 yes
static void CheckVulkanHdr()
{
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) return;
    GetModuleFileNameA(vk, g_vkPath, MAX_PATH);
    typedef int (__stdcall *PFN_Enum)(const char*, unsigned*, void*);
    PFN_Enum en = (PFN_Enum)GetProcAddress(vk, "vkEnumerateInstanceExtensionProperties");
    if (!en) return;
    struct Ext { char name[256]; unsigned ver; };
    unsigned n = 0;
    if (en(nullptr, &n, nullptr) != 0 || !n) return;
    Ext* e = (Ext*)calloc(n, sizeof(Ext));
    g_vkHdrExt = 0;
    if (en(nullptr, &n, e) >= 0)
        for (unsigned i = 0; i < n; i++) if (!strcmp(e[i].name, "VK_EXT_swapchain_colorspace")) g_vkHdrExt = 1;
    free(e);
    Log("hdr: Vulkan loader %s - VK_EXT_swapchain_colorspace %s", g_vkPath, g_vkHdrExt ? "available" : "NOT available");
}

static void HdrSetupSwapchain(IDirect3DDevice9* dev)
{
    g_hdrActive = false;
    if (!g_hdrForced) return;
    if (g_vkHdrExt < 0) CheckVulkanHdr();
    if (g_vkHdrExt == 0) {
        Log("hdr: this Vulkan setup can't present HDR (DXVK would clip it to SDR) - staying SDR. "
            "Check for an old vulkan-1.dll next to the game and update the GPU driver.");
        return;
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (FAILED(dev->GetSwapChain(0, &sc)) || !sc) return;
    IVkExtSwapchain_* ext = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_D3D9VkExtSwapchain, (void**)&ext)) && ext) {
        // scRGB only: HDR10 with the fp16 backbuffer stalled presentation in DXVK (game froze)
        const int order[1] = { VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT_ };
        for (int cs : order) {
            if (!ext->CheckColorSpaceSupport(cs) || FAILED(ext->SetColorSpace(cs))) continue;
            g_hdrActive = true;
            g_hdrPQ = cs == VK_COLOR_SPACE_HDR10_ST2084_EXT_;
            QueryWindowsSdrWhite();
            DiagnoseHdrOutput();
            break;
        }
        if (g_hdrActive) {
            D3D9VkExtOutputMetadata_ md = {};
            if (SUCCEEDED(ext->GetCurrentOutputDesc(&md))) g_hdrMaxNits = md.maxLum;
            Log("hdr: %s output active (display reports %.0f nits peak, %.0f full-frame)",
                g_hdrPQ ? "HDR10 (PQ, Rec.2020)" : "scRGB", md.maxLum, md.maxFullFrameLum);
        } else {
            ext->SetColorSpace(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR_);
            Log("hdr: the display/driver offers no HDR colour space - %s. Staying SDR",
                g_wineVer[0] ? "on Linux/Deck: HDR on in gamescope (Deck OLED: Settings > Display) and DXVK_HDR=1 in launch options"
                             : "is HDR on in Windows?");
        }
        ext->Release();
    }
    sc->Release();
}

// --------------------------------------------------------------------------
// Supersampling plumbing: the game believes its window is the render size
// --------------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_GetClientRect)(HWND, LPRECT);
typedef BOOL (WINAPI *PFN_PtConv)(HWND, LPPOINT);
typedef LPWSTR (WINAPI *PFN_GetCommandLineW)();
static PFN_GetClientRect o_GetClientRect = nullptr;
static PFN_PtConv o_ScreenToClient = nullptr, o_ClientToScreen = nullptr;
static PFN_GetCommandLineW o_GetCommandLineW = nullptr;
static wchar_t g_cmdLine[4096];

static bool SsaaWnd(HWND h) { return g_ssaa && h && h == g_hwnd && g_outW && g_outH; }
static BOOL WINAPI hk_GetClientRect(HWND h, LPRECT r)
{
    BOOL ok = o_GetClientRect(h, r);
    if (ok && r && SsaaWnd(h)) { r->left = r->top = 0; r->right = g_ppW; r->bottom = g_ppH; }
    return ok;
}
static BOOL WINAPI hk_ScreenToClient(HWND h, LPPOINT p)
{
    BOOL ok = o_ScreenToClient(h, p);
    if (ok && p && SsaaWnd(h)) { p->x = MulDiv(p->x, g_ppW, g_outW); p->y = MulDiv(p->y, g_ppH, g_outH); }
    return ok;
}
static BOOL WINAPI hk_ClientToScreen(HWND h, LPPOINT p)
{
    if (p && SsaaWnd(h)) { p->x = MulDiv(p->x, g_outW, g_ppW); p->y = MulDiv(p->y, g_outH, g_ppH); }
    return o_ClientToScreen(h, p);
}
static LPWSTR WINAPI hk_GetCommandLineW() { return g_cmdLine; }
// The game builds its command line from msvcr80's _acmdln, which msvcr80 fills from GetCommandLineA.
typedef LPSTR (WINAPI *PFN_GetCommandLineA)();
static PFN_GetCommandLineA o_crtGetCommandLineA = nullptr;
static char g_acmd[4096];
static UINT g_ssReqW = 0, g_ssReqH = 0;          // render size we ask the game for
static LPSTR WINAPI hk_crtGetCommandLineA() { return g_acmd; }

// The game only accepts resolutions the adapter lists: add the supersampling one.
typedef UINT    (STDMETHODCALLTYPE *PFN_GetAdapterModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
static PFN_GetAdapterModeCount o_GetAdapterModeCount = nullptr;
static PFN_EnumAdapterModes o_EnumAdapterModes = nullptr;
static UINT STDMETHODCALLTYPE hk_GetAdapterModeCount(IDirect3D9* d, UINT a, D3DFORMAT f)
{
    UINT n = o_GetAdapterModeCount(d, a, f);
    return (g_ssReqW && n) ? n + 1 : n;
}
static HRESULT STDMETHODCALLTYPE hk_EnumAdapterModes(IDirect3D9* d, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE* m)
{
    const UINT n = o_GetAdapterModeCount(d, a, f);
    if (!g_ssReqW || i < n || !m || !n) return o_EnumAdapterModes(d, a, f, i, m);
    HRESULT hr = o_EnumAdapterModes(d, a, f, n - 1, m);     // copy refresh/format from the last real mode
    if (SUCCEEDED(hr)) { m->Width = g_ssReqW; m->Height = g_ssReqH; }
    static bool logged = false;
    if (!logged) { logged = true; Log("ssaa: listed %ux%u among the adapter's display modes", g_ssReqW, g_ssReqH); }
    return hr;
}

// Called from DllMain (before the game reads its command line): UE3 takes -ResX/-ResY from it.
static void InstallSupersampling()
{
    if (cfg.renderScale <= 100) return;
    if (!cfg.borderless) { Log("ssaa: RenderScale needs Borderless=1 - ignored"); return; }
    HMODULE exe = GetModuleHandleA(nullptr);
    const int w = (GetSystemMetrics(SM_CXSCREEN) * cfg.renderScale / 100) & ~1;
    const int h = (GetSystemMetrics(SM_CYSCREEN) * cfg.renderScale / 100) & ~1;
    g_ssReqW = w; g_ssReqH = h;
    if (void** s = FindIAT(exe, "kernel32.dll", "GetCommandLineW")) {
        swprintf(g_cmdLine, 4096, L"%ls -ResX=%d -ResY=%d", GetCommandLineW(), w, h);
        o_GetCommandLineW = (PFN_GetCommandLineW)PatchIAT(s, (void*)hk_GetCommandLineW);
    }
    snprintf(g_acmd, sizeof g_acmd, "%s -ResX=%d -ResY=%d", GetCommandLineA(), w, h);
    bool crtOk = false;
    if (HMODULE crt = GetModuleHandleA("msvcr80.dll")) {
        if (void** s = FindIAT(crt, "kernel32.dll", "GetCommandLineA")) {      // msvcr80 not initialised yet
            o_crtGetCommandLineA = (PFN_GetCommandLineA)PatchIAT(s, (void*)hk_crtGetCommandLineA); crtOk = true;
        }
        if (char** p = (char**)GetProcAddress(crt, "_acmdln")) if (*p) { *p = g_acmd; crtOk = true; }   // already initialised
    }
    Log("ssaa: command line for the game: %s (%s)", g_acmd, crtOk ? "ok" : "msvcr80 not found");
    if (void** s = FindIAT(exe, "user32.dll", "GetClientRect"))  o_GetClientRect  = (PFN_GetClientRect)PatchIAT(s, (void*)hk_GetClientRect);
    if (void** s = FindIAT(exe, "user32.dll", "ScreenToClient")) o_ScreenToClient = (PFN_PtConv)PatchIAT(s, (void*)hk_ScreenToClient);
    if (void** s = FindIAT(exe, "user32.dll", "ClientToScreen")) o_ClientToScreen = (PFN_PtConv)PatchIAT(s, (void*)hk_ClientToScreen);
    Log("ssaa: RenderScale %d%% -> asking the game for %dx%d (cmdline %s, window hooks %s)", cfg.renderScale, w, h,
        o_GetCommandLineW ? "ok" : "MISSING", (o_GetClientRect && o_ScreenToClient && o_ClientToScreen) ? "ok" : "partial");
}

// --------------------------------------------------------------------------
// OpenAL: the game loads OpenAL32.dll at runtime. With OpenAL Soft in its place we map the
// game's legacy device name and ask for HRTF (binaural 3D audio for headphones).
// --------------------------------------------------------------------------
typedef void* (__cdecl *PFN_alcOpenDevice)(const char*);
typedef void* (__cdecl *PFN_alcCreateContext)(void*, const int*);
typedef BOOL  (__cdecl *PFN_alcResetDeviceSOFT)(void*, const int*);
typedef void  (__cdecl *PFN_alcGetIntegerv)(void*, int, int, int*);
typedef const char* (__cdecl *PFN_alcGetString)(void*, int);
typedef FARPROC (WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);
static PFN_GetProcAddress o_GetProcAddress = nullptr;
static PFN_alcOpenDevice o_alcOpenDevice = nullptr;
static PFN_alcCreateContext o_alcCreateContext = nullptr;
static PFN_alcResetDeviceSOFT g_alcReset = nullptr;
static PFN_alcGetIntegerv g_alcGetIntegerv = nullptr;
static PFN_alcGetString g_alcGetString = nullptr;
static HMODULE g_alMod = nullptr;
static bool g_alSoft = false;
static void* g_alDev = nullptr;
static int g_alAttrs[64];
static char g_alDevName[128] = "", g_alHrtfName[128] = "";
static int g_alHrtfStatus = -1;
static int g_alHrtfApplied = -1;
enum { ALC_HRTF_SOFT_ = 0x1992, ALC_HRTF_STATUS_SOFT_ = 0x1993, ALC_HRTF_SPECIFIER_SOFT_ = 0x1995,
       ALC_OUTPUT_MODE_SOFT_ = 0x19AC, ALC_STEREO_HRTF_SOFT_ = 0x19B2, ALC_ALL_DEVICES_SPECIFIER_ = 0x1013 };

static void AlBuildAttrs(const int* in)
{
    int n = 0;
    if (in) for (int i = 0; in[i] && n < 56; i += 2) {
        if (in[i] == ALC_HRTF_SOFT_ || in[i] == ALC_OUTPUT_MODE_SOFT_) continue;
        g_alAttrs[n++] = in[i]; g_alAttrs[n++] = in[i + 1];
    }
    if (g_alSoft && cfg.hrtf != 2) {
        g_alAttrs[n++] = ALC_HRTF_SOFT_; g_alAttrs[n++] = cfg.hrtf ? 1 : 0;
        if (cfg.hrtf) { g_alAttrs[n++] = ALC_OUTPUT_MODE_SOFT_; g_alAttrs[n++] = ALC_STEREO_HRTF_SOFT_; }
    }
    g_alAttrs[n] = 0;
}

static void AlQueryStatus()
{
    if (!g_alDev || !g_alSoft || !g_alcGetIntegerv) return;
    int st = -1; g_alcGetIntegerv(g_alDev, ALC_HRTF_STATUS_SOFT_, 1, &st);
    g_alHrtfStatus = st;
    const char* nm = g_alcGetString ? g_alcGetString(g_alDev, ALC_HRTF_SPECIFIER_SOFT_) : nullptr;
    snprintf(g_alHrtfName, sizeof g_alHrtfName, "%s", nm ? nm : "");
    g_alHrtfApplied = cfg.hrtf;
}

static void* __cdecl hk_alcOpenDevice(const char* name)
{
    const char* use = name;
    if (g_alSoft && name && (!_stricmp(name, "Generic Software") || !_stricmp(name, "Generic Hardware"))) use = nullptr;
    void* dev = o_alcOpenDevice(use);
    const char* real = (dev && g_alcGetString) ? g_alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER_) : nullptr;
    snprintf(g_alDevName, sizeof g_alDevName, "%s", real ? real : (name ? name : "default"));
    Log("audio: alcOpenDevice('%s'%s) -> %p %s", name ? name : "(default)", use != name ? " -> default device" : "",
        dev, g_alDevName);
    return dev;
}

static void* __cdecl hk_alcCreateContext(void* dev, const int* attrs)
{
    AlBuildAttrs(attrs);
    void* ctx = o_alcCreateContext(dev, g_alAttrs);
    if (ctx) { g_alDev = dev; AlQueryStatus(); }
    Log("audio: context %p - %s, HRTF %s (status %d, '%s')", ctx, g_alSoft ? "OpenAL Soft" : "not OpenAL Soft",
        cfg.hrtf == 2 ? "left to OpenAL" : cfg.hrtf ? "requested" : "off", g_alHrtfStatus, g_alHrtfName);
    return ctx;
}

// menu: apply a changed HRTF setting live
static void AlApplyHrtf()
{
    if (!g_alDev || !g_alSoft || !g_alcReset) return;
    int base[64]; memcpy(base, g_alAttrs, sizeof base);
    AlBuildAttrs(base);
    const BOOL ok = g_alcReset(g_alDev, g_alAttrs);
    AlQueryStatus();
    Log("audio: device reset for HRTF=%d -> %d (status %d)", cfg.hrtf, ok, g_alHrtfStatus);
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC p = o_GetProcAddress(m, name);
    if (!p || !m || !HIWORD((DWORD_PTR)name)) return p;
    if (name[0] != 'a' || name[1] != 'l' || name[2] != 'c') return p;
    if (m != g_alMod) {
        g_alMod = m;
        g_alSoft = o_GetProcAddress(m, "alsoft_get_version") != nullptr;
        g_alcReset = (PFN_alcResetDeviceSOFT)o_GetProcAddress(m, "alcResetDeviceSOFT");
        g_alcGetIntegerv = (PFN_alcGetIntegerv)o_GetProcAddress(m, "alcGetIntegerv");
        g_alcGetString = (PFN_alcGetString)o_GetProcAddress(m, "alcGetString");
        char path[MAX_PATH] = ""; GetModuleFileNameA(m, path, MAX_PATH);
        Log("audio: OpenAL library %s (%s)", path, g_alSoft ? "OpenAL Soft - HRTF available" : "not OpenAL Soft - no HRTF");
    }
    if (!strcmp(name, "alcOpenDevice"))    { o_alcOpenDevice = (PFN_alcOpenDevice)p; return (FARPROC)hk_alcOpenDevice; }
    if (!strcmp(name, "alcCreateContext")) { o_alcCreateContext = (PFN_alcCreateContext)p; return (FARPROC)hk_alcCreateContext; }
    return p;
}

static void InstallAudioHook()
{
    if (void** s = FindIAT(GetModuleHandleA(nullptr), "kernel32.dll", "GetProcAddress"))
        o_GetProcAddress = (PFN_GetProcAddress)PatchIAT(s, (void*)hk_GetProcAddress);
    Log("audio: GetProcAddress hook %s", o_GetProcAddress ? "ok" : "MISSING");
}

// --------------------------------------------------------------------------
// Render trace (Ctrl + ToggleKey): logs one frame's render-target switches,
// clears, copies and draw counts. Used to find where the 3D scene ends and
// the HUD begins, so PostFX can run before the HUD in a later version.
// --------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRT)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DPUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIPUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetPS)(IDirect3DDevice9*, IDirect3DPixelShader9*);

static PFN_SetRT o_SetRT; static PFN_Clear o_Clear; static PFN_StretchRect o_StretchRect;
static PFN_DP o_DP; static PFN_DIP o_DIP; static PFN_DPUP o_DPUP; static PFN_DIPUP o_DIPUP; static PFN_SetPS o_SetPS;

static volatile bool g_traceArmed = false;
static bool g_wantCapture = false;
static bool g_tracing = false, g_traceKeyDown = false;
static IDirect3DSurface9* g_traceBB = nullptr;
static int g_seg = 0, g_segDraws = 0, g_segUP = 0, g_segPS = 0, g_frameDraws = 0;
static char g_segDesc[160];

static void DescribeSurface(IDirect3DSurface9* s, char* out, size_t n)
{
    if (!s) { snprintf(out, n, "(null)"); return; }
    D3DSURFACE_DESC d; s->GetDesc(&d);
    snprintf(out, n, "%p %ux%u fmt=%d%s", (void*)s, d.Width, d.Height, d.Format, s == g_traceBB ? " [BACKBUFFER]" : "");
}

static void TraceFlushSegment()
{
    if (g_segDraws || g_segUP)
        Log("  seg %d  RT0=%s  draws=%d (UP=%d) psChanges=%d", g_seg, g_segDesc, g_segDraws, g_segUP, g_segPS);
    g_frameDraws += g_segDraws;
    g_segDraws = g_segUP = g_segPS = 0;
}

static inline void TraceDraw(bool up)
{
    if (g_inPostFX) return;
    g_curRTDraws++;
    if (g_tracing) { g_segDraws++; if (up) g_segUP++; }
}

// Image capture (Ctrl+Shift+ToggleKey): during a trace, save each render target
// as it is left, so we can see which buffer holds what (scene, UI, bloom...).
typedef HRESULT (WINAPI *PFN_D3DXSaveSurfaceToFileA)(LPCSTR, DWORD, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
static bool g_capturing = false;
static bool g_captureKeyDown = false;
static int  g_captureCount = 0;
static IDirect3DSurface9* g_segSurf = nullptr;   // current RT0 during a trace (AddRef'd)
static char g_captureDir[MAX_PATH];

static void CaptureSegment()
{
    if (!g_capturing || !g_segSurf || (g_segDraws == 0 && g_segUP == 0) || g_captureCount >= 120) return;
    static PFN_D3DXSaveSurfaceToFileA save = nullptr;
    if (!save) {
        HMODULE dx = GetModuleHandleA("d3dx9_42.dll");
        if (dx) save = (PFN_D3DXSaveSurfaceToFileA)GetProcAddress(dx, "D3DXSaveSurfaceToFileA");
        if (!save) { Log("  capture: D3DXSaveSurfaceToFileA unavailable"); g_capturing = false; return; }
    }
    D3DSURFACE_DESC d; g_segSurf->GetDesc(&d);
    char path[MAX_PATH];
    // float / 16-bit-per-channel targets lose data in PNG (values >1 clamp) -> DDS keeps them exact
    const bool wide = d.Format == D3DFMT_A16B16G16R16F || d.Format == D3DFMT_R32F || d.Format == D3DFMT_G16R16 ||
                      d.Format == D3DFMT_A16B16G16R16 || d.Format == D3DFMT_A32B32G32R32F || d.Format == D3DFMT_R16F ||
                      d.Format == D3DFMT_G16R16F || d.Format == D3DFMT_G32R32F;
    const bool dds = cfg.captureFloatDDS && wide;
    snprintf(path, MAX_PATH, "%s\\seg%02d_%ux%u_fmt%d%s.%s", g_captureDir, g_seg, d.Width, d.Height, d.Format,
             g_segSurf == g_traceBB ? "_BACKBUFFER" : "", dds ? "dds" : "png");
    g_inPostFX = true;                         // keep D3DX's own device calls out of the trace
    HRESULT hr = save(path, dds ? 4 /*D3DXIFF_DDS*/ : 3 /*D3DXIFF_PNG*/, g_segSurf, nullptr, nullptr);
    if (FAILED(hr) && !dds) {                  // fall back to DDS (lossless, any format)
        path[strlen(path) - 3] = 0; strcat(path, "dds");
        hr = save(path, 4 /*D3DXIFF_DDS*/, g_segSurf, nullptr, nullptr);
    }
    g_inPostFX = false;
    g_captureCount++;
    if (FAILED(hr)) Log("  capture seg %d failed 0x%08lx", g_seg, hr);
}

static void SetSegSurf(IDirect3DSurface9* s)
{
    if (g_segSurf) g_segSurf->Release();
    g_segSurf = s;
    if (g_segSurf) g_segSurf->AddRef();
}

// Shadow resolution: the game's shadow depth buffer is a square R32F render target (2048 on the
// user's PC, MaxShadowResolution in the ini doesn't raise it). With [Shadows] Scale it and its depth
// buffer are created Scale x larger and every viewport / scissor / clear rect set while it is bound
// is scaled to match. The game's shadow UVs are normalised, so sampling needs no change.
static UINT g_shadowSize = 0, g_shadowBig = 0;   // the game's size and ours
static int  g_shScaleRun = 1;                    // scale fixed at device creation (menu changes apply next launch)
static int  g_rt0Scale = 1;                      // RT0 is the enlarged shadow map
static bool IsDepthFmt(D3DFORMAT f)
{
    return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16 || f == D3DFMT_D32 || f == D3DFMT_D24FS8 ||
           f == (D3DFORMAT)MAKEFOURCC('I','N','T','Z') || f == (D3DFORMAT)MAKEFOURCC('D','F','2','4') ||
           f == (D3DFORMAT)MAKEFOURCC('D','F','1','6');
}
// a square shadow-sized resource we should enlarge
static bool ShadowCandidate(UINT w, UINT h, D3DFORMAT f, bool rt, bool ds)
{
    if (g_shScaleRun <= 1 || g_inPostFX || w != h || w < 512 || w > 4096) return false;
    if (rt && f == D3DFMT_R32F) {
        if (!g_shadowSize) g_shadowSize = w;
        return w == g_shadowSize;
    }
    // depth buffers: the shadow one may be created before the colour target - any square one >= 1024
    return ds && IsDepthFmt(f) && (g_shadowSize ? w == g_shadowSize : w >= 1024);
}
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                       IDirect3DTexture9**, HANDLE*);
static PFN_CreateTexture o_CreateTexture = nullptr;
static HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f,
                                                  D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    if (!sh && pool == D3DPOOL_DEFAULT &&
        ShadowCandidate(w, h, f, (usage & D3DUSAGE_RENDERTARGET) != 0, (usage & D3DUSAGE_DEPTHSTENCIL) != 0)) {
        const UINT k = (UINT)g_shScaleRun;
        HRESULT hr = o_CreateTexture(d, w * k, h * k, lv, usage, f, pool, out, sh);
        if (SUCCEEDED(hr)) {
            if (f == D3DFMT_R32F) g_shadowBig = w * k;
            Log("shadow: %s texture %ux%u (fmt %d) created at %ux%u", f == D3DFMT_R32F ? "shadow map" : "depth", w, h, f, w * k, h * k);
            return hr;
        }
        Log("shadow: %ux%u at %ux scale failed (0x%08lx) - normal size", w, h, k, hr);
        if (f == D3DFMT_R32F) g_shScaleRun = 1;
    }
    return o_CreateTexture(d, w, h, lv, usage, f, pool, out, sh);
}
static bool IsBigShadow(IDirect3DSurface9* s)
{
    if (!s || !g_shadowBig) return false;
    D3DSURFACE_DESC dd;
    return SUCCEEDED(s->GetDesc(&dd)) && dd.Format == D3DFMT_R32F && dd.Width == g_shadowBig && dd.Height == g_shadowBig;
}
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetViewport)(IDirect3DDevice9*, D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetScissor)(IDirect3DDevice9*, const RECT*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetScissor)(IDirect3DDevice9*, RECT*);
static PFN_SetViewport o_SetViewport = nullptr;
static PFN_GetViewport o_GetViewport = nullptr;
static PFN_SetScissor  o_SetScissor = nullptr;
static PFN_GetScissor  o_GetScissor = nullptr;
static HRESULT STDMETHODCALLTYPE hk_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp)
{
    if (g_rt0Scale > 1 && vp && !g_inPostFX) {
        D3DVIEWPORT9 v = *vp; const DWORD k = g_rt0Scale;
        v.X *= k; v.Y *= k; v.Width *= k; v.Height *= k;
        return o_SetViewport(d, &v);
    }
    return o_SetViewport(d, vp);
}
static HRESULT STDMETHODCALLTYPE hk_GetViewport(IDirect3DDevice9* d, D3DVIEWPORT9* vp)
{
    HRESULT hr = o_GetViewport(d, vp);
    if (SUCCEEDED(hr) && vp && g_rt0Scale > 1 && !g_inPostFX) {
        const DWORD k = g_rt0Scale;
        vp->X /= k; vp->Y /= k; vp->Width /= k; vp->Height /= k;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetScissor(IDirect3DDevice9* d, const RECT* r)
{
    if (g_rt0Scale > 1 && r && !g_inPostFX) {
        const LONG k = g_rt0Scale;
        RECT x = { r->left * k, r->top * k, r->right * k, r->bottom * k };
        return o_SetScissor(d, &x);
    }
    return o_SetScissor(d, r);
}
static HRESULT STDMETHODCALLTYPE hk_GetScissor(IDirect3DDevice9* d, RECT* r)
{
    HRESULT hr = o_GetScissor(d, r);
    if (SUCCEEDED(hr) && r && g_rt0Scale > 1 && !g_inPostFX) {
        const LONG k = g_rt0Scale;
        r->left /= k; r->top /= k; r->right /= k; r->bottom /= k;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetRT(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* s)
{
    if (idx == 0) {
        const int was = g_rt0Scale;
        g_rt0Scale = IsBigShadow(s) ? g_shScaleRun : 1;
        static bool logged = false;
        if (g_rt0Scale > 1 && was == 1 && !logged) { logged = true; Log("shadow: rendering into the %ux%u shadow map", g_shadowBig, g_shadowBig); }
    }
    if (idx == 0 && !g_inPostFX) BeforeHudOnSetRT(d, s);
    if (g_tracing && !g_inPostFX) {
        if (idx == 0) {
            CaptureSegment();
            SetSegSurf(s);
            TraceFlushSegment();
            g_seg++;
            DescribeSurface(s, g_segDesc, sizeof g_segDesc);
        } else if (s) {
            char b[160]; DescribeSurface(s, b, sizeof b);
            Log("  SetRenderTarget[%lu] = %s", idx, b);
        }
    }
    return o_SetRT(d, idx, s);
}
static HRESULT STDMETHODCALLTYPE hk_Clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD st)
{
    if (g_rt0Scale > 1 && n && r && !g_inPostFX) {
        std::vector<D3DRECT> x(r, r + n);
        for (auto& q : x) { q.x1 *= g_rt0Scale; q.y1 *= g_rt0Scale; q.x2 *= g_rt0Scale; q.y2 *= g_rt0Scale; }
        return o_Clear(d, n, x.data(), f, c, z, st);
    }
    if (g_tracing && !g_inPostFX)
        Log("  clear flags=0x%lx color=%08lx (seg %d, after %d draws)", f, (unsigned long)c, g_seg, g_segDraws);
    return o_Clear(d, n, r, f, c, z, st);
}
static HRESULT STDMETHODCALLTYPE hk_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const RECT* rb, D3DTEXTUREFILTERTYPE f)
{
    if (g_tracing && !g_inPostFX) {
        char x[160], y[160]; DescribeSurface(a, x, sizeof x); DescribeSurface(b, y, sizeof y);
        Log("  StretchRect %s -> %s (seg %d, after %d draws)", x, y, g_seg, g_segDraws);
    }
    return o_StretchRect(d, a, ra, b, rb, f);
}
// The game sets a gamma ramp for its brightness slider. DXVK applies ramps through an SDR lookup
// table that clamps at SDR white - with HDR output that clips every bright colour channel by
// channel (heavy oversaturation, HUD included). With HDR on, we keep the ramp ourselves as an
// exponent and apply it in the HDR encode instead.
typedef void (STDMETHODCALLTYPE *PFN_SetGammaRamp)(IDirect3DDevice9*, UINT, DWORD, const D3DGAMMARAMP*);
static PFN_SetGammaRamp o_SetGammaRamp = nullptr;
static void STDMETHODCALLTYPE hk_SetGammaRamp(IDirect3DDevice9* d, UINT sc, DWORD fl, const D3DGAMMARAMP* r)
{
    if (!g_hdrActive || !r) { o_SetGammaRamp(d, sc, fl, r); return; }
    double sum = 0; int n = 0;
    const WORD* ch[3] = { r->red, r->green, r->blue };
    for (int c = 0; c < 3; c++)
        for (int i = 26; i <= 230; i++) {
            const double x = i / 255.0, y = ch[c][i] / 65535.0;
            if (y > 1e-4 && y < 0.9999) { sum += log(y) / log(x); n++; }
        }
    float g = n ? (float)(sum / n) : 1.0f;
    if (g < 0.3f) g = 0.3f;
    if (g > 3.0f) g = 3.0f;
    if (fabsf(g - g_gammaExp) > 0.005f) Log("hdr: game gamma ramp kept out of the HDR path (applied as exponent %.3f)", g);
    g_gammaExp = g;
    static D3DGAMMARAMP identity;
    static bool init = false;
    if (!init) { init = true; for (int i = 0; i < 256; i++) identity.red[i] = identity.green[i] = identity.blue[i] = (WORD)(i * 257); }
    o_SetGammaRamp(d, sc, fl, &identity);
}


// --------------------------------------------------------------------------
// Game shader patching. The game never compiles shaders: it creates them from its cooked caches.
// Every shader still carries its constant table (CTAB), so we recognise the ones we care about
// by constant names at CreatePixelShader, and either swap in our own version (compiled with the
// exact same registers) at SetPixelShader, or adjust their constants around each draw.
// --------------------------------------------------------------------------
struct CtabConst { std::string name; int set; int reg; int count; int type = 0; };   // type: D3DXPT_* (14 = cube sampler)   // set: 0 bool 1 int 2 float 3 sampler
static bool ParseCtab(const DWORD* code, std::vector<CtabConst>& out)
{
    if (!code || ((code[0] & 0xFFFF0000) != 0xFFFF0000 && (code[0] & 0xFFFF0000) != 0xFFFE0000)) return false;   // ps or vs
    for (const DWORD* t = code + 1; t < code + 65536; ) {
        const DWORD tok = *t;
        if (tok == 0x0000FFFF) return false;
        if ((tok & 0xFFFF) != 0xFFFE) return false;          // comments come first; stop at the first instruction
        const DWORD len = (tok >> 16) & 0x7FFF;
        if (len >= 7 && t[1] == MAKEFOURCC('C','T','A','B')) {
            const BYTE* c = (const BYTE*)(t + 2);
            const DWORD n = *(const DWORD*)(c + 12), off = *(const DWORD*)(c + 16);
            for (DWORD i = 0; i < n && i < 256; i++) {
                const BYTE* e = c + off + i * 20;
                CtabConst k;
                k.name  = (const char*)(c + *(const DWORD*)e);
                k.set   = *(const WORD*)(e + 4);
                k.reg   = *(const WORD*)(e + 6);
                k.count = *(const WORD*)(e + 8);
                const DWORD ti = *(const DWORD*)(e + 12);
                if (ti && ti + 4 <= (len - 1) * 4) k.type = *(const WORD*)(c + ti + 2);
                out.push_back(k);
            }
            return true;
        }
        t += 1 + len;
    }
    return false;
}
static const CtabConst* CtabFind(const std::vector<CtabConst>& v, const char* name)
{
    for (auto& k : v) if (k.name == name) return &k;
    return nullptr;
}

enum { SK_BLEND_BLOOM = 1, SK_BLEND_DOF = 2, SK_SHADOW = 3, SK_FOG = 4, SK_MBLUR = 5, SK_BLOOMGATHER = 6, SK_CHARLIGHT = 7,
       SK_GLASS = 8, SK_GLASSDIST = 9, SK_COUNT = 10 };
struct GameShader {
    int kind = 0;
    IDirect3DPixelShader9* repl = nullptr;   // our replacement (blend shaders)
    int paramReg = -1;                       // our extra constant (blend shaders)
    int offReg[3] = { -1, -1, -1 }, offCnt[3] = {};   // shadow sample offsets (UV units of the 2048 map)
    int sizeReg = -1;                        // ShadowBufferSize
    // effects: registers of the constants we scale (-1 = absent)
    int fogScaleReg = -1;
    int velStaticReg = -1, velDynReg = -1, edgeTableReg = -1, unsharpReg = -1;
    int bloomThrReg = -1;
    // shadows: our soft filter, built on first use ([hardware PCF?][quality 1..3])
    std::vector<CtabConst> ct;
    IDirect3DPixelShader9* soft[2][4] = {};
    bool softTried[2][4] = {};
    int shadowTexReg = -1;
    bool softOk = false;                     // a variant we understand (modulated or plain uniform)
    // character / dynamic object lighting
    int shReg[7] = { -1, -1, -1, -1, -1, -1, -1 };   // c_SHCoefficients_Ar Ag Ab Br Bg Bb Cc
    int skyUpReg = -1, skyLoReg = -1, ambReg = -1;
    int distReg = -1;                        // glass refraction: UniformScalar_1 (normal offset scale)
    int sceneTexReg = -1;                    // final blend: SceneColorTexture sampler (source of our bloom)
    int matTag = 0;                          // 1 skin, 2 metal (base pass, by bytecode hash)
    IDirect3DPixelShader9* replSsr = nullptr; // glass: the replacement with screen-space reflections
    IDirect3DPixelShader9* tagged = nullptr; // the game's bytecode + 'mov oC1, tag' (material mask)
    int uses = 0;
};
static std::unordered_map<IDirect3DPixelShader9*, GameShader> g_gameShaders;
static GameShader* g_curGS = nullptr;            // info for the game shader bound right now
static bool  g_ownShaderCreate = false;
static float g_psc[256][4];                      // mirror of the pixel shader float constants
static int   g_shCount[SK_COUNT] = {};           // recognised shaders per kind
static int   g_shUsed[SK_COUNT] = {};            // ... and seen drawing
static int   g_shReplFail = 0;

// Final HDR -> LDR pass. Same maths as the game's AP_BloomBlend / DOFAndBloomBlend shaders, except
// that the hard clip (saturate) before the game's gamma becomes a smooth highlight roll-off.
static const char* kBlendCommonHLSL = R"HLSL(
sampler2D apBloomTex : register(s10);   // APRenewal's wide bloom (half res of the scene)
float3 apModernBloom(float2 uv) { return tex2D(apBloomTex, uv).rgb * apTM4.x; }
// apTM:  x roll-off on, y knee, z white, w hue preservation
// apTM2: x sRGB weight, y gamma-2.2 weight (rest = the game's square root), z bloom intensity, w DOF strength
// apTM3: xy texel size of the quarter-res bloom/DOF buffers, z bloom smoothing, w DOF smoothing
float3 apRolloffCurve(float3 x, float k, float s, float w)
{
    float3 u = max(x - k, 0) / s;
    float3 y = k + s * saturate(u * (1 + u / (w * w)) / (1 + u));   // extended Reinhard: slope 1 at the knee, 1 at w
    return x < k ? x : y;
}
float3 apRolloff(float3 x)
{
    x = max(x, 0);
    float k = apTM.y, W = max(apTM.z, k + 0.05), s = 1 - k, w = (W - k) / s;
    float3 pc = apRolloffCurve(x, k, s, w);                         // per channel: very bright light goes to white
    float m = max(max(x.r, x.g), x.b);
    float3 hp = x * (apRolloffCurve(m.xxx, k, s, w).x / max(m, 1e-5));   // max channel: keeps the hue
    return saturate(lerp(pc, hp, apTM.w));
}
float3 apApplySat(float3 c)
{
    float l = dot(c, SceneScaledLuminanceWeights.rgb);
    c = c * SceneShadowsAndDesaturation.a + l;
    return (c - SceneShadowsAndDesaturation.rgb) * SceneInverseHighLights.rgb;
}
float3 apFinish(float3 c)
{
    float3 lin = (GammaOverlayColor.rgb + c) * GammaColorScaleAndInverse.rgb;
    float3 o = lerp(saturate(lin), apRolloff(lin), apTM.x);
    float3 srgb = o <= 0.0031308 ? o * 12.92 : 1.055 * pow(max(o, 1e-6), 1.0 / 2.4) - 0.055;
    float3 enc = sqrt(o) * (1 - apTM2.x - apTM2.y) + srgb * apTM2.x + pow(max(o, 1e-6), 1.0 / 2.2) * apTM2.y;
    o = lerp(o, enc, GammaColorScaleAndInverse.a);
    // brightness, Photoshop-style contrast, cheap contrast, colour filter (the game's order)
    o = o * HSBContrast.x;
    o = (o - HSBContrast.y) * HSBContrast.z;
    o = lerp(o, o * o, HSBContrast.w);
    float3 sat = saturate(o);
    const float3 LUM = float3(0.299, 0.587, 0.114);
    float oldY = max(1.0 / 255.0, dot(LUM, sat));
    float3 n = lerp(sat, sat / oldY * FilterColor.rgb, FilterColor.a);
    n = lerp(n, FilterColor.rgb, FilterColor.a * 0.3);
    float newY = max(0.0001, dot(LUM, n));
    n = n * (oldY / newY);
    return lerp(n, float3(1, 1, 1), smoothstep(0.95, 1.0, oldY));
}
float3 apBloomTap(float2 uv)
{
    float4 p = tex2D(LensFilterImage, uv);
    return p.rgb * (p.a * p.a) * 4.0;          // the game's RGBE-ish bloom packing
}
float3 apBloom(float2 uv)
{
    float2 t = apTM3.xy;
    float3 one = apBloomTap(uv);
    // 3x3 tent over the quarter-res buffer: no more blocky, shimmering bloom edges
    float3 tent = 4 * one
        + 2 * (apBloomTap(uv + float2(t.x, 0)) + apBloomTap(uv - float2(t.x, 0)) + apBloomTap(uv + float2(0, t.y)) + apBloomTap(uv - float2(0, t.y)))
        + apBloomTap(uv + t) + apBloomTap(uv - t) + apBloomTap(uv + float2(t.x, -t.y)) + apBloomTap(uv + float2(-t.x, t.y));
    return lerp(one, tent / 16, apTM3.z) * apTM2.z;
}
float4 apTent(sampler2D s, float2 uv)
{
    float2 t = apTM3.xy;
    float4 c = 4 * tex2D(s, uv)
        + 2 * (tex2D(s, uv + float2(t.x, 0)) + tex2D(s, uv - float2(t.x, 0)) + tex2D(s, uv + float2(0, t.y)) + tex2D(s, uv - float2(0, t.y)))
        + tex2D(s, uv + t) + tex2D(s, uv - t) + tex2D(s, uv + float2(t.x, -t.y)) + tex2D(s, uv + float2(-t.x, t.y));
    return lerp(tex2D(s, uv), c / 16, apTM3.w);
}
)HLSL";
static const char* kBlendBloomHLSL = R"HLSL(
void main(float2 UV : TEXCOORD0, float2 SceneUV : TEXCOORD1, out float4 OutColor : COLOR0)
{
    float3 c = tex2D(SceneColorTexture, SceneUV).rgb + apBloom(UV) + apModernBloom(SceneUV);
    OutColor = float4(apFinish(apApplySat(c)), 0);
}
)HLSL";
static const char* kBlendDofHLSL = R"HLSL(
float apUnfocused(float d)
{
    float rd = d - PackedParameters.r;
    float mx = rd < 0 ? MinMaxBlurClamp.x : MinMaxBlurClamp.y;
    return min(mx, pow(saturate(max(0.0001, abs(rd) - PackedParameters.a) * PackedParameters.g), PackedParameters.b));
}
void main(float2 UV : TEXCOORD0, float2 SceneUV : TEXCOORD1, out float4 OutColor : COLOR0)
{
    float4 f = tex2D(SceneColorTexture, SceneUV);           // colour + depth in alpha
    f.w = min(f.w, 65000);
    float coc = saturate(apUnfocused(f.w) * apTM2.w);
    float4 back = apTent(NewDOF_noBleedBlurMap, SceneUV);
    float4 fore = apTent(NewDOF_bleedBlurMap, SceneUV);
    float3 cLarge = f.a < PackedParameters.r ? f.rgb : back.rgb;
    float2 px = c_finalColorScaleLow.xy;
    float3 cSmall = 0.25 * (tex2D(SceneColorTexture, SceneUV + px * float2( 0.5, -1.5)).rgb +
                           tex2D(SceneColorTexture, SceneUV + px * float2(-1.5,  0.5)).rgb +
                           tex2D(SceneColorTexture, SceneUV + px * float2(-0.5,  1.5)).rgb +
                           tex2D(SceneColorTexture, SceneUV + px * float2( 1.5,  0.5)).rgb);
    float3 dof = lerp(f.rgb, cSmall, saturate(coc / 0.33));
    dof = lerp(dof, cLarge, saturate((coc - 0.33) / (1.0 - 0.33)));
    dof = lerp(dof, fore.rgb, saturate(smoothstep(0.0, 0.5, fore.a)) * saturate(apTM2.w));
    float3 c = apApplySat(dof + apBloom(UV) + apModernBloom(SceneUV));
    float2 v = SceneUV - 0.5;
    c *= 1 - saturate((2 * dot(v, v) - apVignetteMaskParameters.x) * apVignetteMaskParameters.y);
    OutColor = float4(apFinish(c), 0);
}
)HLSL";

static IDirect3DPixelShader9* BuildReplacement(IDirect3DDevice9* dev, const std::vector<CtabConst>& ct, const char* body,
                                               int& paramReg, const char* name, const char* common = nullptr, const char* defines = nullptr)
{
    std::string src;
    int maxC = -1;
    char line[160];
    for (auto& k : ct) {
        if (k.set == 3) snprintf(line, sizeof line, "%s %s : register(s%d);\n", k.type == 14 ? "samplerCUBE" : k.type == 13 ? "sampler3D" : "sampler2D",
                                 k.name.c_str(), k.reg);
        else if (k.set == 2) {
            if (k.count > 1) snprintf(line, sizeof line, "float4 %s[%d] : register(c%d);\n", k.name.c_str(), k.count, k.reg);
            else snprintf(line, sizeof line, "float4 %s : register(c%d);\n", k.name.c_str(), k.reg);
            maxC = std::max(maxC, k.reg + k.count - 1);
        } else continue;
        src += line;
    }
    paramReg = maxC + 1;
    snprintf(line, sizeof line, "float4 apTM : register(c%d);\nfloat4 apTM2 : register(c%d);\nfloat4 apTM3 : register(c%d);\nfloat4 apTM4 : register(c%d);\n",
             paramReg, paramReg + 1, paramReg + 2, paramReg + 3);
    src += line;
    if (paramReg + 4 + 12 <= 224) {   // extra per-draw values (glass reflections: matrices, camera)
        snprintf(line, sizeof line, "float4 apX[12] : register(c%d);\n", paramReg + 4);
        src += line;
    }
    if (defines) src = std::string(defines) + src;
    src += common ? common : kBlendCommonHLSL;
    src += body;
    void* code = CompileBlob(src.c_str(), "ps_3_0", name);
    if (!code) return nullptr;
    // our registers must be a subset of the game's (plus apTM) - otherwise the game's values land wrong
    std::vector<CtabConst> mine;
    ParseCtab((const DWORD*)BufPtr(code), mine);
    for (auto& k : mine) {
        if (!CtabFind(ct, k.name.c_str()) && k.name.compare(0, 2, "ap") == 0) {
            // ours: constants above the game's, samplers from s8 up
            if ((k.set == 2 && k.reg >= paramReg) || (k.set == 3 && k.reg >= 8)) continue;
            Log("shaders: %s: our %s landed on a game register - keeping the game's shader", name, k.name.c_str());
            BufRelease(code); return nullptr;
        }
        const CtabConst* o = CtabFind(ct, k.name.c_str());
        if (!o || o->set != k.set || o->reg != k.reg) {
            Log("shaders: %s register mismatch on %s - keeping the game's shader", name, k.name.c_str());
            BufRelease(code); return nullptr;
        }
    }
    IDirect3DPixelShader9* ps = nullptr;
    g_ownShaderCreate = true;
    HRESULT hr = dev->CreatePixelShader((const DWORD*)BufPtr(code), &ps);
    g_ownShaderCreate = false;
    BufRelease(code);
    return SUCCEEDED(hr) ? ps : nullptr;
}

// Soft shadows: the game's shadow projection with its PCF replaced by a rotated Vogel-disc filter.
// Each tap is hardware PCF (bilinear depth compare) when the shadow map is a depth texture.
// Defines (set per shader): HW, NTAPS, MODSHADOW, ATTEN, SPOT, CUBE (coverage^3 like the branching
// variants), CLAMPUV. apTM.x = radius in shadow-map UV.
static const char* kShadowCommonHLSL = R"HLSL(
float apIGN(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
// UE3 compiles with row-major packing: register i = matrix row i, and mul(v, M) = sum v_i * row_i
float4 apMul(float4 v, float4 m0, float4 m1, float4 m2, float4 m3) { return v.x * m0 + v.y * m1 + v.z * m2 + v.w * m3; }
float apTap(float2 uv, float z)
{
#if CLAMPUV
    uv = clamp(uv, apTextureCoordsClampParameter.xy, apTextureCoordsClampParameter.zw);
#endif
#if HW
    return tex2Dproj(ShadowDepthTexture, float4(uv, z, 1)).r;
#else
    return z < tex2Dlod(ShadowDepthTexture, float4(uv, 0, 0)).r ? 1.0 : 0.0;
#endif
}
)HLSL";
static const char* kShadowBodyHLSL = R"HLSL(
void main(float4 ScreenPosition : TEXCOORD0, float2 vp : VPOS, out float4 OutColor : COLOR0)
{
    float2 suv = ScreenPosition.xy / ScreenPosition.w * ScreenPositionScaleBias.xy + ScreenPositionScaleBias.wz;
    float W = tex2D(SceneColorTexture, suv).a;                       // depth lives in scene alpha
    float4 p = float4(ScreenPosition.xy / ScreenPosition.w * W, W, 1);
    float4 sp = apMul(p, ScreenToShadowMatrix[0], ScreenToShadowMatrix[1], ScreenToShadowMatrix[2], ScreenToShadowMatrix[3]);
    sp.xy /= sp.w;
    sp.z = min(sp.z, 0.999);
    float rot = apIGN(vp) * 6.2831853;
    float sum = 0;
    [unroll] for (int i = 0; i < NTAPS; i++) {
        float r = sqrt((i + 0.5) / NTAPS);
        float s, c;
        sincos(i * 2.3999632 + rot, s, c);
        sum += apTap(sp.xy + float2(c, s) * (r * apTM.x), sp.z);
    }
    float cov = sum / NTAPS;
#if CUBE
    cov = cov * cov * cov;
#else
    cov = cov * cov;
#endif
#if MODSHADOW
    float att = 1;
#if ATTEN
    float4 wp = apMul(p, ScreenToWorld[0], ScreenToWorld[1], ScreenToWorld[2], ScreenToWorld[3]);
    float4 lv = float4(LightPosition.xyz - wp.xyz, 0) * LightPosition.w;
    float d = max(0, length(lv) * FalloffParameters.y + FalloffParameters.z);
    att = pow(saturate(1 - d * d), FalloffParameters.x);
#if SPOT
    float sa = saturate((dot(normalize(lv.xyz), -SpotDirection.xyz) - SpotAngles.x) * SpotAngles.y);
    att *= sa * sa;
#endif
#endif
    float4 shadowed = lerp(float4(1, 1, 1, 1), ShadowModulateColor, att);
    OutColor = lerp(shadowed, float4(1, 1, 1, 1), cov);
#else
    OutColor = cov;
#endif
}
)HLSL";

static IDirect3DPixelShader9* BuildSoftShadow(IDirect3DDevice9* dev, GameShader& gs, bool hw, int q)
{
    static const int taps[4] = { 0, 16, 32, 48 };
    const auto& ct = gs.ct;
    char defs[320];
    snprintf(defs, sizeof defs,
             "#define HW %d\n#define NTAPS %d\n#define MODSHADOW %d\n#define ATTEN %d\n#define SPOT %d\n#define CUBE %d\n#define CLAMPUV %d\n",
             hw ? 1 : 0, taps[q], CtabFind(ct, "ShadowModulateColor") ? 1 : 0,
             (CtabFind(ct, "LightPosition") && CtabFind(ct, "ScreenToWorld") && CtabFind(ct, "FalloffParameters")) ? 1 : 0,
             (CtabFind(ct, "SpotDirection") && CtabFind(ct, "SpotAngles")) ? 1 : 0,
             CtabFind(ct, "EdgeSampleOffsets") ? 1 : 0, CtabFind(ct, "apTextureCoordsClampParameter") ? 1 : 0);
    char name[64]; snprintf(name, sizeof name, "soft shadow (%s, %d taps)", hw ? "hardware PCF" : "manual", taps[q]);
    int reg = -1;
    IDirect3DPixelShader9* ps = BuildReplacement(dev, ct, kShadowBodyHLSL, reg, name, kShadowCommonHLSL, defs);
    if (ps && reg != gs.paramReg) gs.paramReg = reg;
    Log("shaders: %s %s", name, ps ? "built" : "FAILED - game's filter kept for this light");
    return ps;
}

// Window glass. The game's two glass materials (found with the census, identified by bytecode hash)
// rebuilt from their disassembly: normal-mapped cubemap reflection masked by Texture2D_1.x, opacity
// Texture2D_1.w x APTranslucentOpacity, tint UniformVector_0, diffuse Texture2D_2 x ambient, fog.
// The "fresnel" one also has pow(1 - N.V, UniformScalar_2) between UniformScalar_4 and _3.
// apTM: x opacity scale, y reflection strength, z our Fresnel amount, w game's luminance weighting
// apTM2: x normal-map strength
static const char* kGlassBodyHLSL = R"HLSL(
// GLASSSSR: screen-space reflections from the previous frame. apX[0..3] this frame's view-projection
// rows, apX[4..7] the previous frame's, apX[8] camera xyz + strength, apX[9] (depth fit A, B, max
// distance), apX[10] x thickness. s12 = this frame's 24-bit depth snapshot, s13 = last frame's HDR scene.
// Written without flow control or helper functions (fixed unrolled steps, masks instead of breaks):
// 0.16.0's version with loops/breaks/early returns crashed the game's D3DX compiler.
#if GLASSSSR
sampler2D apZTex : register(s12);
sampler2D apPrevTex : register(s13);
#endif
void main(float4 tx : COLOR0, float3 tz : COLOR1, float2 uv : TEXCOORD0, float4 fog : TEXCOORD4, float3 camv : TEXCOORD6,
          out float4 OutColor : COLOR0, out float4 OutMask : COLOR1)
{
#if DUALNORMAL
    float4 m = tex2D(Texture2D_2, uv);
#else
    float4 m = tex2D(Texture2D_1, uv);
#endif
    float3 a = normalize((tx.xyz - 0.5) * 2);
    float3 b = normalize((tz - 0.5) * 2);
    float3 bin = normalize(((tx.w - 0.5) * 2) * cross(a, b));
#if DUALNORMAL
    float3 n0 = tex2D(Texture2D_0, uv).xyz * 2 - 1, n1 = tex2D(Texture2D_1, uv).xyz * 2 - 1;
    float3 n = float3((n0.xy * UniformScalar_1.x + n1.xy * UniformScalar_3.x) * apTM2.x, n0.z + n1.z);
#else
    float3 n = tex2D(Texture2D_0, uv).xyz * 2 - 1;
    n.xy *= UniformScalar_1.x * apTM2.x;
#endif
    n = normalize(n);
    float3 V = normalize(camv);
    float ndv = dot(n, V);
    float3 R = 2 * ndv * n - V;
    float3 L = R.x * b + R.y * bin + R.z * a;
    float3 W = L.x * LocalToWorldMatrix[0].xyz + L.y * LocalToWorldMatrix[1].xyz + L.z * LocalToWorldMatrix[2].xyz;
    float3 env = texCUBE(TextureCube_0, W).rgb;
#if GAMEFRESNEL
    float p = 0.9999 - ndv >= 0 ? pow(max(1 - ndv, 1e-4), UniformScalar_2.x) : 0;
    env *= p * (UniformScalar_3.x - UniformScalar_4.x) + UniformScalar_4.x;
#endif
    float lum = dot(env, float3(0.299, 0.587, 0.114));
    float3 refl = env * lerp(1, lum, apTM.w);
    // angle-dependent reflection: weaker head-on, much stronger at grazing angles (Schlick-like)
    float fr = pow(saturate(1 - abs(ndv)), 4);
    refl *= apTM.y * lerp(1, 0.35 + 2.5 * fr, apTM.z);
#if GLASSSSR
    {
        // the glass pixel in world space: the vertex shader's camera vector is (camera - position) in the
        // tangent basis, unnormalised, so taking it back through the basis and LocalToWorld gives the offset
        float3 lcv = camv.x * b + camv.y * bin + camv.z * a;
        float3 wcv = lcv.x * LocalToWorldMatrix[0].xyz + lcv.y * LocalToWorldMatrix[1].xyz + lcv.z * LocalToWorldMatrix[2].xyz;
        float3 gwp = apX[8].xyz - wcv;
        float3 Rw = normalize(W);
        // per-pixel jitter from the glass pixel's screen position (apX[10].zw = screen size)
        float4 gc = gwp.x * apX[0] + gwp.y * apX[1] + gwp.z * apX[2] + apX[3];
        float2 gpx = float2(gc.x / max(gc.w, 1) * 0.5 + 0.5, 0.5 - gc.y / max(gc.w, 1) * 0.5) * apX[10].zw;
        float jit = frac(52.9829189 * frac(dot(floor(gpx), float2(0.06711056, 0.00583715))));
        float alive = 1, found = 0, hitT = 0, lastMiss = 4;
        [unroll] for (int si = 1; si <= 16; si++) {
            float st = (si - 1 + jit) / 16.0;
            st = st * st * apX[9].z + 4;
            float3 rp = gwp + Rw * st;
            float4 rc4 = rp.x * apX[0] + rp.y * apX[1] + rp.z * apX[2] + apX[3];
            float rw = max(rc4.w, 1);
            float2 suv = float2(rc4.x / rw * 0.5 + 0.5, 0.5 - rc4.y / rw * 0.5);
            alive *= step(0, suv.x) * step(suv.x, 1) * step(0, suv.y) * step(suv.y, 1) * step(1, rc4.w);
            float zd = 1.0 / max(apX[9].x + apX[9].y * tex2Dlod(apZTex, float4(saturate(suv), 0, 0)).r, 1e-6);
            float dz = rc4.w - zd;
            float hit = (1 - found) * alive * step(0, dz) * step(dz, apX[10].x * (1 + rc4.w * 0.002));
            hitT += hit * st;
            found += hit;
            lastMiss = lerp(lastMiss, st, (1 - found) * alive);
        }
        // refine between the last miss and the hit (fixed 4 halvings, no branches)
        float rlo = lastMiss, rhi = max(hitT, lastMiss);
        [unroll] for (int ri = 0; ri < 4; ri++) {
            float rm = 0.5 * (rlo + rhi);
            float3 mp = gwp + Rw * rm;
            float4 mc = mp.x * apX[0] + mp.y * apX[1] + mp.z * apX[2] + apX[3];
            float mw = max(mc.w, 1);
            float2 muv = saturate(float2(mc.x / mw * 0.5 + 0.5, 0.5 - mc.y / mw * 0.5));
            float mz = 1.0 / max(apX[9].x + apX[9].y * tex2Dlod(apZTex, float4(muv, 0, 0)).r, 1e-6);
            float above = step(0, mc.w - mz);
            rhi = lerp(rhi, rm, above);
            rlo = lerp(rm, rlo, above);
        }
        hitT = lerp(hitT, rhi, found);
        float3 hp = gwp + Rw * hitT;
        float4 pc = hp.x * apX[4] + hp.y * apX[5] + hp.z * apX[6] + apX[7];
        float pw = max(pc.w, 1);
        float2 puv = float2(pc.x / pw * 0.5 + 0.5, 0.5 - pc.y / pw * 0.5);
        float2 pe = saturate(min(puv, 1 - puv) * 10);
        float conf = found * step(1, pc.w) * min(pe.x, pe.y) * saturate(1 - hitT / (apX[9].z + 4)) * step(0.001, apX[8].w);
        float3 srgb = tex2Dlod(apPrevTex, float4(saturate(puv), 0, 0)).rgb;
        float fre = 0.04 + 0.96 * pow(saturate(1 - abs(ndv)), 5);
        refl = lerp(refl, srgb * apX[8].w * lerp(0.6, 2.5, fre), conf * lerp(0.4, 1.0, saturate(fre * 4)));
    }
#endif
    float mask = m.x;
    float3 mr = mask * refl;
    float3 emis = mr + UniformVector_0.rgb;
#if DUALNORMAL
    float3 dif = (tex2D(Texture2D_3, uv).rgb * (1 - mr) + mr) * (1 - UniformVector_0.rgb);
#else
    float3 dif = (tex2D(Texture2D_2, uv).rgb * (1 - mr) + mr) * (1 - UniformVector_0.rgb);
#endif
    float3 col = dif * AmbientColorAndSkyFactor.rgb + emis;
    float alpha = m.w * APTranslucentOpacity.x * apTM.x;
    // a reflection replaces what's seen through the glass: bright reflections add opacity
    alpha = saturate(alpha + dot(mr, float3(0.299, 0.587, 0.114)) * apTM.z * 0.5);
    OutColor = float4(col * fog.w + fog.rgb, alpha);
    OutMask = float4(1, 1, 1, saturate(alpha * 4));   // glass coverage for the AO pass (blended: near-invisible glass keeps AO)
}
)HLSL";

static uint32_t ShaderHash(const DWORD* code, UINT* lenOut = nullptr)
{
    const DWORD* t = code + 1;
    for (int guard = 0; guard < 1 << 20; guard++) {
        const DWORD tok = *t;
        if (tok == 0x0000FFFF) { t++; break; }
        if ((tok & 0xFFFF) == 0xFFFE) t += 1 + ((tok >> 16) & 0x7FFF);
        else t += 1 + ((tok >> 24) & 0xF);
    }
    const UINT len = (UINT)((const BYTE*)t - (const BYTE*)code);
    uint32_t h = 2166136261u;
    for (UINT i = 0; i < len; i++) { h ^= ((const BYTE*)code)[i]; h *= 16777619u; }
    if (lenOut) *lenOut = len;
    return h;
}
// Marker file around the risky glass-reflection compile (D3DX crashed on 0.16.0's version): present at
// startup with this version's name = the compile crashed last time, so the variant is skipped.
static int g_glassSsrBlocked = -1;
static void GlassSsrPath(char* out) { PathJoin(out, g_dir, "APRenewal_glass_compile.lock"); }
static bool GlassSsrBlocked()
{
    if (g_glassSsrBlocked < 0) {
        char p[MAX_PATH]; GlassSsrPath(p);
        char buf[64] = "";
        if (FILE* f = fopen(p, "rb")) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
        g_glassSsrBlocked = !strcmp(buf, "APRenewal 0.17.0") ? 1 : 0;
        if (g_glassSsrBlocked) Log("glass: the reflection shader crashed the compiler last time - glass reflections stay cubemap-only (delete %s to retry)", p);
        else if (buf[0]) DeleteFileA(p);
    }
    return g_glassSsrBlocked == 1;
}
static void GlassSsrMarker(bool on)
{
    char p[MAX_PATH]; GlassSsrPath(p);
    if (on) { if (FILE* f = fopen(p, "wb")) { fputs("APRenewal 0.17.0", f); fclose(f); } }
    else DeleteFileA(p);
}

// Material mask: the game's own bytecode with two extra instructions - "def cK, tag" after the
// header and "mov oC1, cK" before the end token - so the base pass also writes which pixels are skin
// or metal to a second render target. cK is above every constant the shader uses (CTAB and defs).
static IDirect3DPixelShader9* BuildTaggedShader(IDirect3DDevice9* dev, const DWORD* fn, const std::vector<CtabConst>& ct, int tag)
{
    if ((fn[0] & 0xFFFF0000) != 0xFFFF0000 || (fn[0] & 0xFFFF) < 0x0300) return nullptr;   // ps_3_0 only
    int maxC = -1;
    for (auto& k : ct) if (k.set == 2) maxC = std::max(maxC, k.reg + k.count - 1);
    size_t first = 0, end = 0;
    const DWORD* t = fn + 1;
    for (int guard = 0; guard < 1 << 18; guard++) {
        const DWORD tok = *t;
        if (tok == 0x0000FFFF) { end = t - fn; break; }
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        if (!first) first = t - fn;
        const DWORD op = tok & 0xFFFF, len = (tok >> 24) & 0xF;
        if (len >= 1) {
            const DWORD d = t[1];
            const DWORD type = ((d >> 28) & 7) | ((d >> 8) & 0x18);
            if (op == 0x51 && type == 2) maxC = std::max(maxC, (int)(d & 0x7FF));          // def cN
            if (op != 0x1F && op != 0x51 && op != 0x2F && op != 0x30 && type == 8 && (d & 0x7FF) == 1) return nullptr;  // already writes oC1
            // relative constant addressing could reach any register: leave such shaders alone
            for (DWORD k = 1; k <= len; k++) if (t[k] & 0x2000) {
                const DWORD ty = ((t[k] >> 28) & 7) | ((t[k] >> 8) & 0x18);
                if (ty == 2 && op != 0x51 && op != 0x2F && op != 0x30) return nullptr;
            }
        }
        t += 1 + len;
    }
    const int K = maxC + 1;
    if (!first || !end || K > 223) return nullptr;
    std::vector<DWORD> v(fn, fn + first);
    const float val[4] = { tag == 1 ? 1.0f : 0.0f, tag == 2 ? 1.0f : 0.0f, tag == 3 ? 1.0f : 0.0f, 1.0f };
    v.push_back(0x05000051); v.push_back(0xA00F0000u | (DWORD)K);
    for (int i = 0; i < 4; i++) { DWORD w; memcpy(&w, &val[i], 4); v.push_back(w); }
    v.insert(v.end(), fn + first, fn + end);
    v.push_back(0x02000001); v.push_back(0x800F0801u); v.push_back(0xA0E40000u | (DWORD)K);
    v.push_back(0x0000FFFF);
    IDirect3DPixelShader9* ps = nullptr;
    g_ownShaderCreate = true;
    HRESULT hr = dev->CreatePixelShader(v.data(), &ps);
    g_ownShaderCreate = false;
    return SUCCEEDED(hr) ? ps : nullptr;
}
static int MaterialTag(uint32_t h)
{
    if (std::binary_search(kSkinHashes, kSkinHashes + sizeof kSkinHashes / sizeof kSkinHashes[0], h)) return 1;
    if (std::binary_search(kMetalHashes, kMetalHashes + sizeof kMetalHashes / sizeof kMetalHashes[0], h)) return 2;
    if (std::binary_search(kRoughHashes, kRoughHashes + sizeof kRoughHashes / sizeof kRoughHashes[0], h)) return 3;
    return 0;
}
static int g_matTagCount[4] = {}, g_matTagFail = 0;

static bool ReplActive(const GameShader* gs)
{
    if (!gs || !gs->repl) return false;
    if (gs->kind == SK_GLASS) return cfg.glassEnable != 0;
    if (gs->kind == SK_FOG) return cfg.aoEnable && cfg.aoFog > 0;
    return cfg.fxBlend != 0;
}

// The game's one-layer height fog (HeightFogPixelShader OneLayerMain), same maths, plus its
// transmittance (1 = no fog) on a second target so the AO can fade with the fog. apTM.x picks how
// the mask is written for the fog pass's blend mode (0: ONE/SRCALPHA style, 1: plain or alpha blend).
static const char* kFogBodyHLSL = R"HLSL(
void main(float4 tc : TEXCOORD0, float4 sv : TEXCOORD1, out float4 OutColor : COLOR0, out float4 OutMask : COLOR1)
{
    float d = clamp(tex2D(SceneColorTexture, tc.xy).a, 1, 65535);
    float din = max(d - FogStartDistance.x, 0);
    float3 wp = sv.xyz * d;
    float wz = abs(wp.z) <= 0.001 ? 0.001 : wp.z;
    float2 hp = saturate(tc.zw / wz);
    float ld = din * abs(hp.y - hp.x);
    float neg = FogDistanceScale.x * ld;
    float sc = ld < FogExtinctionDistance.x ? exp2(neg) : 0.0;
    OutColor = float4((sc - 1) * FogInScattering.rgb, sc);
    OutMask = apTM.x < 0.5 ? float4(0, 0, 0, sc) : float4(sc, sc, sc, 1);
    clip(-neg - 0.001);
}
)HLSL";

// Shader census: one frame's pixel shaders with draw counts and constant names (Diagnostics).
static volatile bool g_censusArm = false;
static bool g_censusOn = false;
static IDirect3DPixelShader9* g_curGamePS = nullptr;
static std::unordered_map<IDirect3DPixelShader9*, int> g_census;
static void CensusDump()
{
    Log("census: %u pixel shaders drew this frame", (unsigned)g_census.size());
    for (auto& e : g_census) {
        UINT sz = 0;
        if (FAILED(e.first->GetFunction(nullptr, &sz)) || !sz || sz > (1u << 20)) continue;
        std::vector<DWORD> code(sz / 4 + 1);
        if (FAILED(e.first->GetFunction(code.data(), &sz))) continue;
        uint32_t h = 2166136261u;
        for (UINT i = 0; i < sz; i++) { h ^= ((const BYTE*)code.data())[i]; h *= 16777619u; }
        std::vector<CtabConst> ct;
        ParseCtab(code.data(), ct);
        std::string names;
        for (auto& k : ct) { if (names.size() > 300) { names += " ..."; break; } names += k.name; names += ' '; }
        auto it = g_gameShaders.find(e.first);
        Log("census:   ps %p hash %08x %4d draws %s| %s", (void*)e.first, h, e.second,
            it != g_gameShaders.end() ? "[patched] " : "", names.c_str());
    }
    g_census.clear();
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreatePS)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
static PFN_CreatePS o_CreatePS = nullptr;
static HRESULT STDMETHODCALLTYPE hk_CreatePS(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out)
{
    HRESULT hr = o_CreatePS(d, fn, out);
    if (FAILED(hr) || g_ownShaderCreate || !out || !*out) return hr;
    std::vector<CtabConst> ct;
    if (!ParseCtab(fn, ct)) return hr;
    GameShader gs;
    const uint32_t hash = ShaderHash(fn);
    const int tag = MaterialTag(hash);
    if (hash == 0xcb2f41c2u || hash == 0x23836016u || hash == 0xcc79d4cdu) {
        const bool fres = hash == 0x23836016u, dual = hash == 0xcc79d4cdu;
        gs.kind = SK_GLASS;
        const char* name = fres ? "window glass (fresnel)" : dual ? "door glass" : "window glass";
        char defs[160];
        snprintf(defs, sizeof defs, "#define GAMEFRESNEL %d\n#define DUALNORMAL %d\n#define GLASSSSR 0\n", fres ? 1 : 0, dual ? 1 : 0);
        gs.repl = BuildReplacement(d, ct, kGlassBodyHLSL, gs.paramReg, name, "", defs);
        // the reflection variant is compiled separately, behind a marker file: if the game's compiler
        // ever crashes on it, the next launch sees the marker and leaves it out
        if (gs.repl && cfg.glassSsr > 0 && !GlassSsrBlocked()) {
            snprintf(defs, sizeof defs, "#define GAMEFRESNEL %d\n#define DUALNORMAL %d\n#define GLASSSSR 1\n", fres ? 1 : 0, dual ? 1 : 0);
            GlassSsrMarker(true);
            int reg = -1;
            gs.replSsr = BuildReplacement(d, ct, kGlassBodyHLSL, reg, name, "", defs);
            GlassSsrMarker(false);
            if (gs.replSsr && reg != gs.paramReg) { gs.replSsr->Release(); gs.replSsr = nullptr; }
            Log("glass: %s with screen-space reflections %s", name, gs.replSsr ? "ready" : "FAILED to build - cubemap reflections only");
        }
        if (!gs.repl) g_shReplFail++;
        Log("shaders: window glass material %08x recognised - %s", hash, gs.repl ? "improved version ready" : "replacement FAILED, game shader kept");
    } else if (hash == 0x3e0261ffu || hash == 0x98160836u || hash == 0x855d48c7u) {
        gs.kind = SK_GLASSDIST;
        if (auto k = CtabFind(ct, hash == 0x855d48c7u ? "UniformScalar_5" : "UniformScalar_1")) gs.distReg = k->reg;
    } else if (CtabFind(ct, "HSBContrast") && CtabFind(ct, "GammaColorScaleAndInverse") && CtabFind(ct, "LensFilterImage") &&
        CtabFind(ct, "SceneColorTexture") && CtabFind(ct, "FilterColor")) {
        const bool dof = CtabFind(ct, "NewDOF_noBleedBlurMap") && CtabFind(ct, "NewDOF_bleedBlurMap") &&
                         CtabFind(ct, "PackedParameters") && CtabFind(ct, "MinMaxBlurClamp") &&
                         CtabFind(ct, "apVignetteMaskParameters") && CtabFind(ct, "c_finalColorScaleLow");
        const bool plain = !CtabFind(ct, "PackedParameters") && !CtabFind(ct, "NewDOF_noBleedBlurMap");
        if (!dof && !plain) { Log("shaders: unknown final-blend variant (%u constants) - left alone", (unsigned)ct.size()); return hr; }
        gs.kind = dof ? SK_BLEND_DOF : SK_BLEND_BLOOM;
        gs.sceneTexReg = CtabFind(ct, "SceneColorTexture")->reg;
        gs.repl = BuildReplacement(d, ct, dof ? kBlendDofHLSL : kBlendBloomHLSL, gs.paramReg, dof ? "final blend (DOF)" : "final blend");
        if (!gs.repl) g_shReplFail++;
        Log("shaders: game final blend%s recognised (%p) - %s", dof ? " with DOF" : "", (void*)*out,
            gs.repl ? "highlight roll-off version ready" : "replacement FAILED, game shader kept");
    } else if (CtabFind(ct, "FogDistanceScale") && CtabFind(ct, "FogInScattering") && CtabFind(ct, "FogStartDistance")) {
        gs.kind = SK_FOG;
        gs.fogScaleReg = CtabFind(ct, "FogDistanceScale")->reg;
        // one-layer variant (the one the game uses): rebuilt so it can also write fog transmittance
        if (!CtabFind(ct, "FogMinStartDistance") && CtabFind(ct, "FogInScattering")->count == 1 && CtabFind(ct, "SceneColorTexture")) {
            gs.repl = BuildReplacement(d, ct, kFogBodyHLSL, gs.paramReg, "height fog", "", nullptr);
            Log("shaders: height fog (one layer) %s", gs.repl ? "rebuilt - AO can fade with the fog" : "rebuild FAILED - game's kept");
        }
    } else if (CtabFind(ct, "VelocityBuffer") && CtabFind(ct, "PrevViewProjMatrix")) {
        gs.kind = SK_MBLUR;
        if (auto k = CtabFind(ct, "StaticVelocityParameters"))  gs.velStaticReg = k->reg;
        if (auto k = CtabFind(ct, "DynamicVelocityParameters")) gs.velDynReg = k->reg;
        if (auto k = CtabFind(ct, "apOffsetAndWeightTable"))    gs.edgeTableReg = k->reg;
        if (auto k = CtabFind(ct, "apUnsharpMaskAmount"))       gs.unsharpReg = k->reg;
    } else if (CtabFind(ct, "BloomThreshold") && CtabFind(ct, "SceneColorTexture") && ct.size() == 2) {
        gs.kind = SK_BLOOMGATHER;
        gs.bloomThrReg = CtabFind(ct, "BloomThreshold")->reg;
    } else if (CtabFind(ct, "c_SHCoefficients_Ar") || (CtabFind(ct, "UpperSkyColor") && CtabFind(ct, "LowerSkyColor"))) {
        gs.kind = SK_CHARLIGHT;
        static const char* shn[7] = { "c_SHCoefficients_Ar", "c_SHCoefficients_Ag", "c_SHCoefficients_Ab", "c_SHCoefficients_Br",
                                      "c_SHCoefficients_Bg", "c_SHCoefficients_Bb", "c_SHCoefficients_Cc" };
        for (int i = 0; i < 7; i++) if (auto k = CtabFind(ct, shn[i])) gs.shReg[i] = k->reg;
        if (auto k = CtabFind(ct, "UpperSkyColor")) gs.skyUpReg = k->reg;
        if (auto k = CtabFind(ct, "LowerSkyColor")) gs.skyLoReg = k->reg;
        if (auto k = CtabFind(ct, "AmbientColorAndSkyFactor")) gs.ambReg = k->reg;
    } else if (CtabFind(ct, "ScreenToShadowMatrix") && CtabFind(ct, "ShadowDepthTexture")) {
        gs.kind = SK_SHADOW;
        gs.ct = ct;
        gs.shadowTexReg = CtabFind(ct, "ShadowDepthTexture")->reg;
        // the soft filter covers what we can reproduce: modulated shadows (all light types) and the
        // plain uniform projection; others (e.g. Beast area lights) keep the game's filter
        gs.softOk = CtabFind(ct, "SceneColorTexture") && CtabFind(ct, "ScreenPositionScaleBias") &&
                    (CtabFind(ct, "ShadowModulateColor") || !CtabFind(ct, "EdgeSampleOffsets"));
        const char* names[3] = { "SampleOffsets", "RefiningSampleOffsets", "EdgeSampleOffsets" };
        for (int i = 0; i < 3; i++)
            if (const CtabConst* k = CtabFind(ct, names[i])) if (k->set == 2) { gs.offReg[i] = k->reg; gs.offCnt[i] = k->count; }
        if (const CtabConst* k = CtabFind(ct, "ShadowBufferSize")) if (k->set == 2) gs.sizeReg = k->reg;
        if (gs.offReg[0] < 0 && gs.offReg[1] < 0 && gs.offReg[2] < 0) return hr;
    } else if (!tag) return hr;
    if (tag) {
        gs.matTag = tag;
        gs.tagged = BuildTaggedShader(d, fn, ct, tag);
        if (gs.tagged) g_matTagCount[tag]++; else g_matTagFail++;
        if (g_matTagCount[1] + g_matTagCount[2] + g_matTagCount[3] + g_matTagFail <= 3 || !gs.tagged)
            Log("materials: %s shader %08x %s", tag == 1 ? "skin" : tag == 2 ? "metal" : "rough", hash, gs.tagged ? "tagged" : "could not be tagged");
    }
    (*out)->AddRef();   // keep the game's shader alive so its pointer is never reused for another one
    g_gameShaders[*out] = gs;
    g_shCount[gs.kind]++;
    static const char* kn[SK_COUNT] = { "", "final blend", "final blend (DOF)", "shadow projection", "height fog", "motion blur / edge AA", "bloom gather", "character lighting", "window glass", "glass refraction" };
    if (gs.kind >= SK_FOG && gs.kind != SK_CHARLIGHT) Log("shaders: %s recognised (%p)", kn[gs.kind], (void*)*out);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_SetPSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
static PFN_SetPSConstF o_SetPSConstF = nullptr;
static HRESULT STDMETHODCALLTYPE hk_SetPSConstF(IDirect3DDevice9* d, UINT reg, const float* v, UINT n)
{
    if (v && reg < 256 && !g_inPostFX) memcpy(g_psc[reg], v, std::min<UINT>(n, 256 - reg) * 16);   // the game's values only
    return o_SetPSConstF(d, reg, v, n);
}

// Vertex shaders: which register holds ViewProjectionMatrix (world -> clip, row-major), for
// reconstructing world positions from depth (reflections).
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVS)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVS)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
static PFN_CreateVS o_CreateVS = nullptr;
static PFN_SetVS o_SetVS = nullptr;
static PFN_SetVSConstF o_SetVSConstF = nullptr;
static std::unordered_map<IDirect3DVertexShader9*, int> g_vsVP;
static HRESULT STDMETHODCALLTYPE hk_CreateVS(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** out)
{
    HRESULT hr = o_CreateVS(d, fn, out);
    if (SUCCEEDED(hr) && out && *out && !g_inPostFX && !g_ownShaderCreate) {
        std::vector<CtabConst> ct;
        if (ParseCtab(fn, ct))
            if (const CtabConst* k = CtabFind(ct, "ViewProjectionMatrix"))
                if (k->set == 2 && k->count == 4) { (*out)->AddRef(); g_vsVP[*out] = k->reg; }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_SetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* vs)
{
    if (!g_inPostFX) {
        auto it = vs ? g_vsVP.find(vs) : g_vsVP.end();
        g_curVSvp = it != g_vsVP.end() ? it->second : -1;
    }
    return o_SetVS(d, vs);
}
static HRESULT STDMETHODCALLTYPE hk_SetVSConstF(IDirect3DDevice9* d, UINT reg, const float* v, UINT n)
{
    if (v && reg < 256 && !g_inPostFX) memcpy(g_vsc[reg], v, std::min<UINT>(n, 256 - reg) * 16);
    return o_SetVSConstF(d, reg, v, n);
}

// Occlusion culling: UE3 hides objects whose previous-frame occlusion query found no visible
// pixels. Some objects (distant trees behind walls) have bounds that make that test fail, so they
// pop in late. With [Game] NoOcclusionCulling every occlusion query reports "visible".
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateQuery)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_QGetData)(IDirect3DQuery9*, void*, DWORD, DWORD);
static PFN_CreateQuery o_CreateQuery = nullptr;
static PFN_QGetData o_QGetData = nullptr;
static long g_occForced = 0;
static HRESULT STDMETHODCALLTYPE hk_QGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD flags)
{
    HRESULT hr = o_QGetData(q, data, size, flags);
    if (hr == S_OK && cfg.noOcclusion && data && size >= sizeof(DWORD) && q->GetType() == D3DQUERYTYPE_OCCLUSION && !g_inPostFX) {
        DWORD* px = (DWORD*)data;
        if (*px < 64) { *px = 64; g_occForced++; }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateQuery(IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** out)
{
    HRESULT hr = o_CreateQuery(d, t, out);
    if (SUCCEEDED(hr) && out && *out && t == D3DQUERYTYPE_OCCLUSION && !o_QGetData) {
        void** vt = *(void***)*out;
        o_QGetData = (PFN_QGetData)vt[7];
        DWORD old;
        VirtualProtect(&vt[7], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vt[7] = (void*)hk_QGetData;
        VirtualProtect(&vt[7], sizeof(void*), old, &old);
        Log("game: occlusion query hook installed (%s)", cfg.noOcclusion ? "objects never culled by occlusion" : "inactive");
    }
    return hr;
}

// Before a draw with a recognised game shader: feed our values. After: put the game's back.
static int g_preDrawLo = -1, g_preDrawHi = -1;
static IDirect3DDevice9* g_preDrawDev = nullptr;
// write one changed register of the game's (restored after the draw)
static void PdSet(int reg, const float* v, int n = 1)
{
    if (reg < 0 || reg + n > 256) return;
    o_SetPSConstF(g_preDrawDev, reg, v, n);
    g_preDrawLo = g_preDrawLo < 0 ? reg : std::min(g_preDrawLo, reg);
    g_preDrawHi = std::max(g_preDrawHi, reg + n - 1);
}
static bool BlendReplaced() { return cfg.fxBlend != 0; }
// APRenewal bloom: 6 downsamples of the HDR scene (soft threshold on the first), tent upsamples back.
static IDirect3DPixelShader9 *g_psBloomDown = nullptr, *g_psBloomUp = nullptr;
static bool g_bloomTried = false;
static IDirect3DTexture9* g_blD[7] = {};
static IDirect3DTexture9* g_blU[7] = {};
static void BloomRelease()
{
    for (int i = 0; i < 7; i++) {
        if (g_blD[i]) { g_blD[i]->Release(); g_blD[i] = nullptr; }
        if (g_blU[i]) { g_blU[i]->Release(); g_blU[i] = nullptr; }
    }
}
static IDirect3DTexture9* BuildModernBloom(IDirect3DDevice9* d, IDirect3DBaseTexture9* src)
{
    if (!src || src->GetType() != D3DRTYPE_TEXTURE || !ExtrasReady(d)) return nullptr;
    if (!g_bloomTried) {
        g_bloomTried = true;
        g_psBloomDown = CompilePS(d, kBloomDownPS, "bloom downsample");
        g_psBloomUp   = CompilePS(d, kBloomUpPS, "bloom upsample");
    }
    if (!g_psBloomDown || !g_psBloomUp) return nullptr;
    D3DSURFACE_DESC sd;
    if (FAILED(((IDirect3DTexture9*)src)->GetLevelDesc(0, &sd))) return nullptr;
    const int N = 6;
    UINT w[7], h[7];
    w[0] = sd.Width; h[0] = sd.Height;
    for (int i = 1; i <= N; i++) {
        w[i] = std::max(1u, w[i - 1] / 2); h[i] = std::max(1u, h[i - 1] / 2);
        if (!EnsureRT(d, &g_blD[i], w[i], h[i], D3DFMT_A16B16G16R16F)) return nullptr;
        if (i < N && !EnsureRT(d, &g_blU[i], w[i], h[i], D3DFMT_A16B16G16R16F)) return nullptr;
    }
    const float thr = cfg.bloomModernThr / 100.0f;
    IDirect3DBaseTexture9* prev = src;
    for (int i = 1; i <= N; i++) {
        IDirect3DSurface9* dst = nullptr; g_blD[i]->GetSurfaceLevel(0, &dst);
        const float c1[4] = { 1.0f / w[i - 1], 1.0f / h[i - 1], i == 1 ? std::max(thr, 0.01f) : 0.0f, i == 1 ? std::max(thr, 0.01f) * 0.5f : 0.0f };
        XPass(d, dst, w[i], h[i], prev, true, nullptr, false, g_psBloomDown, c1, nullptr, true);
        dst->Release();
        prev = g_blD[i];
    }
    IDirect3DTexture9* low = g_blD[N];
    for (int i = N - 1; i >= 1; i--) {
        IDirect3DSurface9* dst = nullptr; g_blU[i]->GetSurfaceLevel(0, &dst);
        const float c1[4] = { 1.0f / w[i + 1], 1.0f / h[i + 1], 0, 0 };
        XPass(d, dst, w[i], h[i], low, true, g_blD[i], true, g_psBloomUp, c1, nullptr, true);
        dst->Release();
        low = g_blU[i];
    }
    static bool logged = false;
    if (!logged) { logged = true; Log("shaders: APRenewal bloom built (%ux%u .. %ux%u)", w[1], h[1], w[N], h[N]); }
    return g_blU[1];
}
// Binds a per-scene mask (same size/format as RT0, cleared once per scene) as render target 1.
static bool BindMaskRT(IDirect3DDevice9* d, IDirect3DTexture9** tex, IDirect3DSurface9** surf, unsigned* key, D3DCOLOR clear, const char* what)
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC rd; rt->GetDesc(&rd); rt->Release();
    if (g_ppW && (rd.Width != g_ppW || rd.Height != g_ppH)) return false;   // scene captures / reflections: not the AO's view
    D3DSURFACE_DESC md = {};
    if (*surf) (*surf)->GetDesc(&md);
    if (!*tex || md.Width != rd.Width || md.Height != rd.Height || md.Format != rd.Format) {
        if (*surf) { (*surf)->Release(); *surf = nullptr; }
        if (*tex) { (*tex)->Release(); *tex = nullptr; }
        if (FAILED(d->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, tex, nullptr))) {
            *tex = nullptr; Log("shaders: %s mask creation failed", what); return false;
        }
        (*tex)->GetSurfaceLevel(0, surf);
        Log("shaders: %s mask %ux%u (fmt %d) for AO", what, rd.Width, rd.Height, rd.Format);
    }
    if (*key != GlassMaskKey()) { d->ColorFill(*surf, nullptr, clear); *key = GlassMaskKey(); }
    o_SetRT(d, 1, *surf);
    return true;
}
static void PreDraw(IDirect3DDevice9* d)
{
    if (g_inPostFX) return;                       // our own passes (they may run inside a game draw)
    g_preDrawLo = g_preDrawHi = -1;
    if (g_curVSvp >= 0 && g_curRTIsHDR && g_curVSvp + 4 <= 256) {   // main-view view-projection (reflections)
        memcpy(g_vp, g_vsc[g_curVSvp], sizeof g_vp);
        g_vpKey = GlassMaskKey();
        static bool vpLogged = false;
        if (!vpLogged) { vpLogged = true;
            Log("ssr: view-projection found in the game's vertex shader (c%d): r0 %.3f %.3f %.3f %.3f  r3 %.1f %.1f %.1f %.1f",
                g_curVSvp, g_vp[0][0], g_vp[0][1], g_vp[0][2], g_vp[0][3], g_vp[3][0], g_vp[3][1], g_vp[3][2], g_vp[3][3]); }
    }
    if (g_censusOn && !g_inPostFX && g_curGamePS) g_census[g_curGamePS]++;
    if (g_curGS && g_curGS->tagged && cfg.matEnable && g_curRTIsHDR) {
        g_matMaskBound = BindMaskRT(d, &g_matMask, &g_matMaskSurf, &g_matMaskFrame, 0, "material");
        if (g_matMaskBound && !g_matTaggedDraws++) Log("materials: first skin/metal draw tagged");
    }
    GameShader* gs = g_curGS;
    if (!gs || g_inPostFX || !o_SetPSConstF) return;
    g_preDrawDev = d;
    if (!gs->uses++) {
        static const char* kn[SK_COUNT] = { "", "final blend", "final blend (DOF)", "shadow projection", "height fog", "motion blur / edge AA", "bloom gather", "character lighting", "window glass", "glass refraction" };
        if (gs->kind != SK_CHARLIGHT || !g_shUsed[SK_CHARLIGHT]) Log("shaders: %s %p in use", kn[gs->kind], (void*)gs);
        g_shUsed[gs->kind]++;
    }
    auto clamp01 = [](int v) { return std::min(std::max(v, 0), 100) / 100.0f; };
    switch (gs->kind) {
    case SK_BLEND_BLOOM: case SK_BLEND_DOF: {
        if (!gs->repl || !BlendReplaced() || gs->paramReg < 0 || gs->paramReg > 252) return;
        if (cfg.bloomModern > 0 && gs->sceneTexReg >= 0) {
            IDirect3DBaseTexture9* scene = nullptr;
            if (SUCCEEDED(d->GetTexture(gs->sceneTexReg, &scene)) && scene) {
                GameShader* keep = g_curGS;
                IDirect3DTexture9* bl = BuildModernBloom(d, scene);
                g_curGS = keep;
                scene->Release();
                if (bl) {
                    d->SetTexture(10, bl);
                    d->SetSamplerState(10, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    d->SetSamplerState(10, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    d->SetSamplerState(10, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                    d->SetSamplerState(10, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                    d->SetSamplerState(10, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                    d->SetSamplerState(10, D3DSAMP_SRGBTEXTURE, FALSE);
                    g_bloomBound = true;
                }
            }
        }
        const float bw = g_ppW ? 1.0f / (g_ppW / 4 + 2) : 1.0f / 642, bh = g_ppH ? 1.0f / (g_ppH / 4 + 2) : 1.0f / 362;
        const float p[4][4] = {
            { cfg.tmEnable ? 1.0f : 0.0f, cfg.tmKnee / 100.0f, cfg.tmWhite / 100.0f, cfg.tmHue / 100.0f },
            { cfg.gammaMode == 1 ? 1.0f : 0.0f, cfg.gammaMode == 2 ? 1.0f : 0.0f, std::max(cfg.bloomIntensity, 0) / 100.0f, std::max(cfg.dofStrength, 0) / 100.0f },
            { bw, bh, cfg.bloomSmooth ? 1.0f : 0.0f, cfg.dofSmooth ? 1.0f : 0.0f },
            { g_bloomBound ? std::max(cfg.bloomModern, 0) / 100.0f * 0.3f : 0.0f, 0, 0, 0 } };
        o_SetPSConstF(d, gs->paramReg, &p[0][0], 4);   // our own registers - nothing of the game's to restore
        return;
    }
    case SK_SHADOW: {
        if (cfg.shadowFilter > 0 && gs->softOk) {
            // which kind of shadow map is bound: depth texture = hardware PCF, R32F = manual compare
            bool hw = false;
            IDirect3DBaseTexture9* bt = nullptr;
            if (gs->shadowTexReg >= 0 && SUCCEEDED(d->GetTexture(gs->shadowTexReg, &bt)) && bt) {
                if (bt->GetType() == D3DRTYPE_TEXTURE) {
                    D3DSURFACE_DESC td;
                    if (SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &td))) hw = IsDepthFmt(td.Format);
                }
                bt->Release();
            }
            const int q = cfg.shadowFilter;
            if (!gs->softTried[hw][q]) { gs->softTried[hw][q] = true; gs->soft[hw][q] = BuildSoftShadow(d, *gs, hw, q); }
            if (IDirect3DPixelShader9* ps = gs->soft[hw][q]) {
                o_SetPS(d, ps);     // re-bound before every draw: the game believes its own shader is set
                const float mapPx = (float)(g_shadowBig ? g_shadowBig : (g_shadowSize ? g_shadowSize : 2048));
                const float base  = (float)(g_shadowSize ? g_shadowSize : 2048);
                const float r[4] = { std::max(cfg.shadowRadius, 1) / 10.0f / base, mapPx, 0, 0 };
                o_SetPSConstF(d, gs->paramReg, r, 1);
                return;
            }
        }
        if (!g_shadowBig || !g_shadowSize || g_shScaleRun <= 1) return;
        const float k = (float)g_shScaleRun;
        const float f = (1.0f / k) + (1.0f - 1.0f / k) * clamp01(cfg.shadowSoft);
        float tmp[64][4];
        for (int i = 0; i < 3; i++) {
            const int r = gs->offReg[i], n = gs->offCnt[i];
            if (r < 0 || n <= 0 || r + n > 256 || n > 64) continue;
            for (int j = 0; j < n; j++) for (int c = 0; c < 4; c++) tmp[j][c] = g_psc[r + j][c] * f;
            PdSet(r, &tmp[0][0], n);
        }
        if (gs->sizeReg >= 0 && gs->sizeReg < 256) {
            const float sz[4] = { g_psc[gs->sizeReg][0] * k, g_psc[gs->sizeReg][1] * k, g_psc[gs->sizeReg][2], g_psc[gs->sizeReg][3] };
            PdSet(gs->sizeReg, sz);
        }
        return;
    }
    case SK_FOG: {
        if (ReplActive(gs) && gs->paramReg >= 0 && gs->paramReg < 254) {
            // fog transmittance for the AO: how the mask must be written depends on the fog pass's blending
            DWORD dst = 0; d->GetRenderState(D3DRS_DESTBLEND, &dst);
            static bool logged = false;
            if (!logged) { logged = true; DWORD src = 0, en = 0; d->GetRenderState(D3DRS_SRCBLEND, &src); d->GetRenderState(D3DRS_ALPHABLENDENABLE, &en);
                           Log("shaders: fog pass blend %s src %lu dst %lu", en ? "on" : "off", src, dst); }
            const float fp[4] = { dst == D3DBLEND_SRCALPHA ? 0.0f : 1.0f, 0, 0, 0 };
            o_SetPSConstF(d, gs->paramReg, fp, 1);
            g_fogMaskBound = BindMaskRT(d, &g_fogMask, &g_fogMaskSurf, &g_fogMaskFrame, 0xFFFFFFFF, "fog");
        }
        if (cfg.fogDensity == 100 || gs->fogScaleReg < 0) return;
        const float m = std::max(cfg.fogDensity, 0) / 100.0f;   // exp2(scale * distance): scale 0 = no fog
        const float* o = g_psc[gs->fogScaleReg];
        const float v[4] = { o[0] * m, o[1] * m, o[2] * m, o[3] * m };
        PdSet(gs->fogScaleReg, v);
        return;
    }
    case SK_MBLUR: {
        if (cfg.motionBlur != 100) {
            const float m = std::max(cfg.motionBlur, 0) / 100.0f;   // velocity scale; the clamp (zw) stays
            if (gs->velStaticReg >= 0) { const float* o = g_psc[gs->velStaticReg]; const float v[4] = { o[0] * m, o[1] * m, o[2], o[3] }; PdSet(gs->velStaticReg, v); }
            if (gs->velDynReg >= 0)    { const float* o = g_psc[gs->velDynReg];    const float v[4] = { o[0] * m, o[1] * m, o[2], o[3] }; PdSet(gs->velDynReg, v); }
        }
        if (cfg.edgeAA != 100 && gs->edgeTableReg >= 0) {      // table[0].w = weight of the edge blur
            const float* o = g_psc[gs->edgeTableReg];
            const float v[4] = { o[0], o[1], o[2], o[3] * std::max(cfg.edgeAA, 0) / 100.0f };
            PdSet(gs->edgeTableReg, v);
        }
        if (cfg.gameSharpen != 100 && gs->unsharpReg >= 0) {
            const float* o = g_psc[gs->unsharpReg];
            const float v[4] = { o[0] * std::max(cfg.gameSharpen, 0) / 100.0f, o[1], o[2], o[3] };
            PdSet(gs->unsharpReg, v);
        }
        return;
    }
    case SK_CHARLIGHT: {
        if (!cfg.charEven && !cfg.charFill) return;
        const float e = 1.0f - clamp01(cfg.charEven), f = 1.0f + std::max(cfg.charFill, 0) / 100.0f;
        // irradiance-volume SH (D3DX layout): A* = linear xyz + constant w, B* / C = quadratic terms.
        // "Even" scales the directional terms down (dark side lifted, bright side lowered), "fill"
        // raises the constant term.
        for (int i = 0; i < 7; i++) {
            const int r = gs->shReg[i];
            if (r < 0) continue;
            const float* o = g_psc[r];
            float v[4];
            if (i < 3) { v[0] = o[0] * e; v[1] = o[1] * e; v[2] = o[2] * e; v[3] = o[3] * f; }
            else       { v[0] = o[0] * e; v[1] = o[1] * e; v[2] = o[2] * e; v[3] = o[3] * e; }
            PdSet(r, v);
        }
        if (gs->skyUpReg >= 0 && gs->skyLoReg >= 0) {   // sky hemisphere: pull upper/lower towards their mean
            const float* u = g_psc[gs->skyUpReg]; const float* l = g_psc[gs->skyLoReg];
            float nu[4], nl[4];
            for (int c = 0; c < 4; c++) {
                const float m = 0.5f * (u[c] + l[c]);
                nu[c] = (m + (u[c] - m) * e) * (c < 3 ? f : 1.0f);
                nl[c] = (m + (l[c] - m) * e) * (c < 3 ? f : 1.0f);
            }
            PdSet(gs->skyUpReg, nu); PdSet(gs->skyLoReg, nl);
        }
        return;
    }
    case SK_GLASS: {
        if (!ReplActive(gs) || gs->paramReg < 0 || gs->paramReg > 253) return;
        const float p[2][4] = {
            { std::max(cfg.glassClarity, 0) / 100.0f, std::max(cfg.glassRefl, 0) / 100.0f, clamp01(cfg.glassFresnel), clamp01(cfg.glassReflContrast) },
            { std::max(cfg.glassBump, 0) / 100.0f, 0, 0, 0 } };
        o_SetPSConstF(d, gs->paramReg, &p[0][0], 2);
        // screen-space reflections from the previous frame (needs this frame's matrix + 24-bit depth)
        if (gs->paramReg + 16 <= 224) {
            float x[12][4] = {};
            float M[3][3], A[3][3], t[3], cam[3];
            const bool ok = cfg.glassSsr > 0 && g_prevHdr && g_prevHdrFrame + 1 == g_frameNo && g_vpKey / 64u == g_frameNo &&
                            g_fx.zcopy && g_fx.zValid && g_fx.fitValid && VpInverse(g_vp, M, A, t, cam);
            if (ok) {
                memcpy(x[0], g_vp, sizeof g_vp);
                memcpy(x[4], g_vpPrev, sizeof g_vpPrev);
                x[8][0] = cam[0]; x[8][1] = cam[1]; x[8][2] = cam[2]; x[8][3] = cfg.glassSsr / 100.0f;
                x[9][0] = g_fx.fitA; x[9][1] = g_fx.fitB; x[9][2] = 2000.0f;
                x[10][0] = 30.0f; x[10][2] = (float)(g_ppW ? g_ppW : g_bbW); x[10][3] = (float)(g_ppH ? g_ppH : g_bbH);
                d->SetTexture(12, g_fx.zcopy);
                d->SetTexture(13, g_prevHdr);
                for (DWORD sm = 12; sm <= 13; sm++) {
                    d->SetSamplerState(sm, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                    d->SetSamplerState(sm, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                    d->SetSamplerState(sm, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                    d->SetSamplerState(sm, D3DSAMP_MINFILTER, sm == 12 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                    d->SetSamplerState(sm, D3DSAMP_MAGFILTER, sm == 12 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                    d->SetSamplerState(sm, D3DSAMP_SRGBTEXTURE, FALSE);
                }
                g_glassSsrBound = true;
                static bool logged = false;
                if (!logged) { logged = true; Log("glass: screen-space reflections from the previous frame running"); }
            }
            o_SetPSConstF(d, gs->paramReg + 4, &x[0][0], 12);
        }
        if (cfg.aoGlass > 0 && cfg.aoEnable)
            g_glassMaskBound = BindMaskRT(d, &g_glassMask, &g_glassMaskSurf, &g_glassMaskFrame, 0, "glass");
        return;
    }
    case SK_GLASSDIST: {
        if (cfg.glassRefract == 100 || gs->distReg < 0) return;
        const float* o = g_psc[gs->distReg];
        const float v[4] = { o[0] * std::max(cfg.glassRefract, 0) / 100.0f, o[1], o[2], o[3] };
        PdSet(gs->distReg, v);
        return;
    }
    case SK_BLOOMGATHER: {
        if (cfg.bloomThreshold == 0 || gs->bloomThrReg < 0) return;
        const float* o = g_psc[gs->bloomThrReg];
        const float t = std::min(std::max(o[0] + cfg.bloomThreshold / 100.0f, 0.0f), 3.9f);
        const float v[4] = { t, 1.0f / (4.0f - t), o[2], o[3] };   // (threshold, 1 / (4 - threshold))
        PdSet(gs->bloomThrReg, v);
        return;
    }
    }
}
static void PostDrawRestore(IDirect3DDevice9* d)
{
    if (g_inPostFX) return;
    if (g_glassMaskBound) { o_SetRT(d, 1, nullptr); g_glassMaskBound = false; }
    if (g_matMaskBound) { o_SetRT(d, 1, nullptr); g_matMaskBound = false; }
    if (g_glassSsrBound) { d->SetTexture(12, nullptr); d->SetTexture(13, nullptr); g_glassSsrBound = false; }
    if (g_fogMaskBound) { o_SetRT(d, 1, nullptr); g_fogMaskBound = false; }
    if (g_bloomBound) { d->SetTexture(10, nullptr); g_bloomBound = false; }
    if (g_preDrawLo < 0) return;
    o_SetPSConstF(d, g_preDrawLo, g_psc[g_preDrawLo], g_preDrawHi - g_preDrawLo + 1);
    g_preDrawLo = g_preDrawHi = -1;
}

static void AfterDraw(IDirect3DDevice9* d)
{
    PostDrawRestore(d);
    if (g_hdrArm && !g_inPostFX && g_curRT == g_bbPtr) { g_hdrArm = false; HdrExpand(d); }
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{ TraceDraw(false); PreDraw(d); HRESULT hr = o_DP(d, t, a, b); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{ TraceDraw(false); PreDraw(d); HRESULT hr = o_DIP(d, t, a, b, c, e, f); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, const void* v, UINT s)
{ TraceDraw(true); PreDraw(d); HRESULT hr = o_DPUP(d, t, a, v, s); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i, D3DFORMAT fm, const void* v, UINT s)
{ TraceDraw(true); PreDraw(d); HRESULT hr = o_DIPUP(d, t, a, b, c, i, fm, v, s); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
{
    if (g_inPostFX) return o_SetPS(d, ps);       // our own passes: leave the game's bookkeeping alone
    if (g_tracing) g_segPS++;
    g_curGS = nullptr;
    if (!g_inPostFX) g_curGamePS = ps;
    if (ps && !g_inPostFX && !g_gameShaders.empty()) {
        auto it = g_gameShaders.find(ps);
        if (it != g_gameShaders.end()) {
            g_curGS = &it->second;
            if (ReplActive(g_curGS)) return o_SetPS(d, g_curGS->replSsr && cfg.glassSsr > 0 ? g_curGS->replSsr : g_curGS->repl);
            if (g_curGS->tagged && cfg.matEnable) return o_SetPS(d, g_curGS->tagged);
        }
    }
    return o_SetPS(d, ps);
}

static void HookTraceVtbl(IDirect3DDevice9* dev)
{
    void* p;
    if ((p = PatchVtbl(dev, 34, (void*)hk_StretchRect))) o_StretchRect = (PFN_StretchRect)p;
    if ((p = PatchVtbl(dev, 37, (void*)hk_SetRT)))       o_SetRT = (PFN_SetRT)p;
    if ((p = PatchVtbl(dev, 43, (void*)hk_Clear)))       o_Clear = (PFN_Clear)p;
    g_shScaleRun = cfg.shadowScale;
    if (g_shScaleRun > 1) {
        if ((p = PatchVtbl(dev, 23, (void*)hk_CreateTexture))) o_CreateTexture = (PFN_CreateTexture)p;
        if ((p = PatchVtbl(dev, 47, (void*)hk_SetViewport)))   o_SetViewport = (PFN_SetViewport)p;
        if ((p = PatchVtbl(dev, 48, (void*)hk_GetViewport)))   o_GetViewport = (PFN_GetViewport)p;
        if ((p = PatchVtbl(dev, 75, (void*)hk_SetScissor)))    o_SetScissor = (PFN_SetScissor)p;
        if ((p = PatchVtbl(dev, 76, (void*)hk_GetScissor)))    o_GetScissor = (PFN_GetScissor)p;
        Log("shadow: shadow maps at %dx resolution", cfg.shadowScale);
    }
    if ((p = PatchVtbl(dev, 81, (void*)hk_DP)))          o_DP = (PFN_DP)p;
    if ((p = PatchVtbl(dev, 82, (void*)hk_DIP)))         o_DIP = (PFN_DIP)p;
    if ((p = PatchVtbl(dev, 83, (void*)hk_DPUP)))        o_DPUP = (PFN_DPUP)p;
    if ((p = PatchVtbl(dev, 84, (void*)hk_DIPUP)))       o_DIPUP = (PFN_DIPUP)p;
    if ((p = PatchVtbl(dev, 107, (void*)hk_SetPS)))      o_SetPS = (PFN_SetPS)p;
    if ((p = PatchVtbl(dev, 106, (void*)hk_CreatePS)))   o_CreatePS = (PFN_CreatePS)p;
    if ((p = PatchVtbl(dev, 109, (void*)hk_SetPSConstF))) o_SetPSConstF = (PFN_SetPSConstF)p;
    if ((p = PatchVtbl(dev, 91, (void*)hk_CreateVS)))     o_CreateVS = (PFN_CreateVS)p;
    if ((p = PatchVtbl(dev, 92, (void*)hk_SetVS)))        o_SetVS = (PFN_SetVS)p;
    if ((p = PatchVtbl(dev, 94, (void*)hk_SetVSConstF)))  o_SetVSConstF = (PFN_SetVSConstF)p;
    if ((p = PatchVtbl(dev, 118, (void*)hk_CreateQuery))) o_CreateQuery = (PFN_CreateQuery)p;
}

// Called at every Present (before PostFX). Ends a running trace, starts an armed one.
static void TraceFrameBoundary(IDirect3DDevice9* dev)
{
    g_frameNo++;
    if (g_censusOn) { g_censusOn = false; CensusDump(); }
    if (g_censusArm) { g_censusArm = false; g_census.clear(); g_censusOn = true; }
    const bool key = (GetAsyncKeyState(cfg.toggleKey) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool down = key && !shift, capDown = key && shift;
    bool& wantCapture = g_wantCapture;
    if (down && !g_traceKeyDown) { g_traceArmed = true; wantCapture = false; }
    if (capDown && !g_captureKeyDown) { g_traceArmed = true; wantCapture = true; }
    g_traceKeyDown = down;
    g_captureKeyDown = capDown;

    if (g_tracing) {
        CaptureSegment();
        SetSegSurf(nullptr);
        if (g_capturing) Log("=== capture: %d images in %s ===", g_captureCount, g_captureDir);
        g_capturing = false;
        TraceFlushSegment();
        Log("=== trace end: %d draws total ===", g_frameDraws);
        g_tracing = false;
        if (g_traceBB) { g_traceBB->Release(); g_traceBB = nullptr; }
    }
    if (g_traceArmed) {
        g_traceArmed = false;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_traceBB);
        IDirect3DSurface9* rt = nullptr;
        dev->GetRenderTarget(0, &rt);
        g_seg = 0; g_frameDraws = 0; g_segDraws = g_segUP = g_segPS = 0;
        DescribeSurface(rt, g_segDesc, sizeof g_segDesc);
        SetSegSurf(rt);
        if (rt) rt->Release();
        if (wantCapture) {
            SYSTEMTIME st; GetLocalTime(&st);
            char sub[64];
            snprintf(sub, sizeof sub, "APRenewal\\capture_%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
            PathJoin(g_captureDir, g_dir, sub);
            CreateDirectoryA(g_captureDir, nullptr);
            g_capturing = true;
            g_captureCount = 0;
        }
        Log(wantCapture ? "=== trace + capture begin (next frame) ===" : "=== trace begin (next frame) ===");
        g_tracing = true;
    }
}

// --------------------------------------------------------------------------
// D3D9 hooks
// --------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

static PFN_CreateDevice o_CreateDevice = nullptr;
static PFN_Reset        o_Reset        = nullptr;
static PFN_Present      o_Present      = nullptr;

// Scene depth as INTZ: the game's full-screen depth buffer is created as an INTZ depth texture instead of a
// plain surface. It behaves identically as a depth-stencil buffer, but can also be sampled (24-bit depth).
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDS)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                                  IDirect3DSurface9**, HANDLE*);
static PFN_CreateDS o_CreateDS = nullptr;
static int g_intzSupport = -1;   // -1 unknown, 0 no, 1 yes
static HRESULT STDMETHODCALLTYPE hk_CreateDS(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                             DWORD msq, BOOL discard, IDirect3DSurface9** out, HANDLE* shared)
{
    const D3DFORMAT INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
    if (out && !shared && ms == D3DMULTISAMPLE_NONE && ShadowCandidate(w, h, fmt, false, true)) {
        const UINT k = (UINT)g_shScaleRun;
        HRESULT hr = o_CreateDS(dev, w * k, h * k, fmt, ms, msq, discard, out, shared);
        if (SUCCEEDED(hr)) { Log("shadow: depth buffer %ux%u (fmt %d) created at %ux%u", w, h, fmt, w * k, h * k); return hr; }
    }
    if (cfg.aoPrecise && out && !shared && ms == D3DMULTISAMPLE_NONE && w == g_ppW && h == g_ppH &&
        (fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8)) {
        if (g_intzSupport < 0) {
            IDirect3D9* d3d = nullptr;
            g_intzSupport = 0;
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
                g_intzSupport = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                                 D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, INTZ)) ? 1 : 0;
                d3d->Release();
            }
            Log("depth: INTZ %s by the driver", g_intzSupport ? "supported" : "NOT supported - using 16-bit depth");
        }
        if (g_intzSupport == 1) {
            IDirect3DTexture9* tex = nullptr;
            if (SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, INTZ, D3DPOOL_DEFAULT, &tex, nullptr))) {
                IDirect3DSurface9* surf = nullptr;
                tex->GetSurfaceLevel(0, &surf);   // the surface keeps the texture alive; the game owns it
                tex->Release();
                for (auto& e : g_intzTab) if (!e.surf) { e.surf = surf; e.tex = tex; break; }
                *out = surf;
                Log("depth: scene depth buffer %ux%u (fmt %d) created as INTZ texture %p", w, h, fmt, (void*)tex);
                return D3D_OK;
            }
            Log("depth: INTZ texture creation failed - plain depth buffer");
        }
    }
    return o_CreateDS(dev, w, h, fmt, ms, msq, discard, out, shared);
}

static void DumpPP(const char* tag, const D3DPRESENT_PARAMETERS* pp)
{
    Log("%s: %ux%u fmt=%d count=%u msaa=%d/%lu windowed=%d hz=%u interval=0x%lx swap=%d depth=%d flags=0x%lx",
        tag, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
        pp->MultiSampleType, pp->MultiSampleQuality, pp->Windowed, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval, pp->SwapEffect, pp->AutoDepthStencilFormat, pp->Flags);
}

static void ApplyPresentParams(D3DPRESENT_PARAMETERS* pp, HWND focus)
{
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (wnd) g_hwnd = wnd;
    if (pp->BackBufferWidth && pp->BackBufferHeight) { g_ppW = pp->BackBufferWidth; g_ppH = pp->BackBufferHeight; }

    if (cfg.vsync == 0) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (cfg.vsync == 1) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (cfg.borderless && g_hwnd) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(mon, &mi);
        const RECT& r = mi.rcMonitor;
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, GetWindowLongA(g_hwnd, GWL_EXSTYLE) & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOPMOST));
        SetWindowPos(g_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        // BackBuffer size is left to the game: set the in-game res to the desktop res.
        // Larger than the window = supersampling: we downscale at Present (needs a copy swap effect
        // so only the downscaled corner of the backbuffer is shown).
        const UINT mw = r.right - r.left, mh = r.bottom - r.top;
        g_ssaa = pp->BackBufferWidth > mw + 8 && pp->BackBufferHeight > mh + 8;
        if (g_ssaa) {
            g_outW = mw; g_outH = mh;
            pp->SwapEffect = D3DSWAPEFFECT_COPY;
            pp->BackBufferCount = 1;
            pp->MultiSampleType = D3DMULTISAMPLE_NONE;
            Log("ssaa: rendering %ux%u, shown at %ux%u (%.2fx)", pp->BackBufferWidth, pp->BackBufferHeight, mw, mh,
                (float)pp->BackBufferWidth / mw);
        }
    } else g_ssaa = false;
    // HDR: fp16 backbuffer (DXVK only); the colour space is set once the device exists
    g_hdrForced = false;
    if (cfg.hdrEnable && g_hdrUnlocked && pp->Windowed) {
        if (pp->BackBufferFormat != D3DFMT_A16B16G16R16F)
            g_bbLogicalFmt = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
        pp->BackBufferFormat = D3DFMT_A16B16G16R16F;
        g_hdrForced = true;
    }
}

// --------------------------------------------------------------------------
// INTZ calibration: fit 1/d = A + B*z from a 64x36 grid of (depth-buffer z, HDR-alpha linear depth)
// pairs. Runs every 30 frames at Present; the HDR alpha is noisy (fp16) but unbiased, and 2000 points
// average that out. The fit is checked before use.
// --------------------------------------------------------------------------
static void CalibrateDepth(IDirect3DDevice9* dev)
{
    if (!cfg.aoPrecise || !g_fx.zcopy || !g_fx.zValid || !g_fx.depth || !g_fx.depthValid || !g_fx.psFit || !g_fx.vs || !g_fx.sb) return;
    if (g_fx.fitValid && (++g_fx.fitCounter % 30) != 0) return;
    const UINT FW = 64, FH = 36;
    if (!g_fx.fitRT &&
        FAILED(dev->CreateTexture(FW, FH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &g_fx.fitRT, nullptr))) {
        Log("depth: calibration target unavailable - using 16-bit depth"); cfg.aoPrecise = 0; return;
    }
    if (!g_fx.fitSys &&
        FAILED(dev->CreateOffscreenPlainSurface(FW, FH, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &g_fx.fitSys, nullptr))) {
        Log("depth: calibration readback unavailable - using 16-bit depth"); cfg.aoPrecise = 0; return;
    }
    g_inPostFX = true;
    g_fx.sb->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr, *rt = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    g_fx.fitRT->GetSurfaceLevel(0, &rt);
    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetRenderTarget(0, rt);
        dev->SetDepthStencilSurface(nullptr);          // the depth buffer is the texture we read
        dev->SetVertexShader(g_fx.vs);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
        const float hp[4] = { -1.0f / FW, 1.0f / FH, 0, 0 };
        dev->SetVertexShaderConstantF(0, hp, 1);
        const DWORD rs[][2] = { { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF } };
        for (auto& r : rs) dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
        for (DWORD st = 0; st < 2; st++) {
            dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        D3DVIEWPORT9 vp = { 0, 0, FW, FH, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->SetTexture(0, g_fx.zcopy);
        dev->SetTexture(1, g_fx.depth);
        DrawFullscreen(dev, g_fx.zcopy, rt, g_fx.psFit);
        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        dev->EndScene();
    }
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    g_fx.sb->Apply();
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    g_inPostFX = false;

    bool ok = SUCCEEDED(dev->GetRenderTargetData(rt, g_fx.fitSys));
    rt->Release();
    D3DLOCKED_RECT lr;
    if (!ok || FAILED(g_fx.fitSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (!g_fx.fitValid && g_fx.fitFail++ < 3) Log("depth: calibration readback failed");
        return;
    }
    // least squares of y = 1/d against x = z
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    static float px[64 * 36], pd[64 * 36];
    int cnt = 0;
    for (UINT y = 0; y < FH; y++) {
        const float* row = (const float*)((const BYTE*)lr.pBits + y * lr.Pitch);
        for (UINT x = 0; x < FW; x++) {
            const float z = row[x * 2], d = row[x * 2 + 1];
            if (!(z > 0.0f && z < 1.0f && d > 5.0f && d < 4000.0f)) continue;
            const double iy = 1.0 / d;
            n++; sx += z; sy += iy; sxx += (double)z * z; sxy += z * iy;
            px[cnt] = z; pd[cnt] = d; cnt++;
        }
    }
    g_fx.fitSys->UnlockRect();
    const double den = n * sxx - sx * sx;
    if (cnt < 100 || fabs(den) < 1e-12) {
        if (!g_fx.fitValid && g_fx.fitFail++ < 3)
            Log("depth: calibration skipped - only %d usable depth samples", cnt);
        return;
    }
    const double B = (n * sxy - sx * sy) / den, A = (sy - B * sx) / n;
    // check: median relative error of the fitted depth against the HDR-alpha depth
    static float err[64 * 36];
    for (int i = 0; i < cnt; i++) {
        const double pred = 1.0 / (A + B * px[i]);
        err[i] = (float)fabs(pred - pd[i]) / pd[i];
    }
    std::nth_element(err, err + cnt / 2, err + cnt);
    const float med = err[cnt / 2];
    if (med < 0.01f && A + B * 0.5 > 0) {
        g_fx.fitA = (float)A; g_fx.fitB = (float)B; g_fx.fitValid = true;
        if (!g_fx.fitLogged) {
            g_fx.fitLogged = true;
            Log("depth: 24-bit INTZ depth calibrated (1/d = %.6g + %.6g*z, %d points, median error %.3f%%)", A, B, cnt, med * 100);
        }
    } else if (!g_fx.fitValid) {
        static int warned = 0;
        if (warned++ < 3) Log("depth: calibration rejected (median error %.2f%%, %d points) - still using 16-bit depth", med * 100, cnt);
    }
}

// ==========================================================================
// In-game menu (Dear ImGui)
//
// Input: UE3 reads mouse movement through DirectInput (dixi's dinput8.dll),
// keys and mouse buttons through window messages, gamepads through XInput.
// While the menu is open all three are captured: the DirectInput mouse feeds
// a virtual cursor, messages feed ImGui, and the game sees no input at all.
// ImGui itself only ever runs on the render thread (in Present); the main
// thread just queues events.
// ==========================================================================
static volatile bool g_menuOpen = false;

// ---- events from the main thread -> render thread
struct MenuEv { int type, a, b; };              // 0 = key(vk, down), 1 = char, 2 = wheel(delta)
static SRWLOCK g_evLock = SRWLOCK_INIT;
static MenuEv  g_ev[256];
static int     g_evN = 0;
static void PushEv(int t, int a, int b)
{
    AcquireSRWLockExclusive(&g_evLock);
    if (g_evN < 256) g_ev[g_evN++] = { t, a, b };
    ReleaseSRWLockExclusive(&g_evLock);
}

// ---- window procedure subclass
static WNDPROC g_origWndProc = nullptr;
static bool    g_wndUnicode = true;
static LRESULT CALLBACK hk_WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY || m == WM_QUERYENDSESSION || m == WM_ENDSESSION ||
        (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE)) {
        if (!g_exiting) Log("window closing (msg 0x%x) - focus pause disabled", m);
        g_exiting = true;
    }
    // supersampling: the game's view of the window is the render size
    if (g_ssaa && g_outW && g_outH) {
        // the game resizes its window to the render size (3840x2160 on a 2560x1440 screen):
        // keep the window on the monitor - the bigger size only exists in the game's head
        if (m == WM_WINDOWPOSCHANGING && l) {
            WINDOWPOS* wp = (WINDOWPOS*)l;
            MONITORINFO mi = { sizeof mi };
            if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                const RECT& r = mi.rcMonitor;
                if (!(wp->flags & SWP_NOSIZE) && (wp->cx != r.right - r.left || wp->cy != r.bottom - r.top)) {
                    static int n = 0;
                    if (n++ < 3) Log("ssaa: game tried to resize its window to %dx%d - kept at %ldx%ld", wp->cx, wp->cy,
                                     r.right - r.left, r.bottom - r.top);
                    wp->cx = r.right - r.left; wp->cy = r.bottom - r.top;
                }
                if (!(wp->flags & SWP_NOMOVE)) { wp->x = r.left; wp->y = r.top; }
            }
        }
        if (m == WM_SIZE && w != SIZE_MINIMIZED) l = MAKELPARAM(g_ppW, g_ppH);
        else if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL)
            l = MAKELPARAM(MulDiv((short)LOWORD(l), g_ppW, g_outW), MulDiv((short)HIWORD(l), g_ppH, g_outH));
    }
    if (g_menuOpen) {
        switch (m) {
        case WM_SYSKEYDOWN:
            if (w == VK_F4) break;                 // keep Alt+F4 working
            // fallthrough
        case WM_KEYDOWN:   PushEv(0, (int)w, 1); return 0;
        case WM_KEYUP: case WM_SYSKEYUP: PushEv(0, (int)w, 0); return 0;
        case WM_CHAR:      PushEv(1, (int)w, 0); return 0;
        case WM_MOUSEWHEEL: PushEv(2, GET_WHEEL_DELTA_WPARAM(w), 0); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEMOVE:
            return 0;
        case WM_INPUT:
            return g_wndUnicode ? DefWindowProcW(h, m, w, l) : DefWindowProcA(h, m, w, l);
        }
    }
    return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, m, w, l) : CallWindowProcA(g_origWndProc, h, m, w, l);
}

static void InstallWndProcHook()
{
    if (g_origWndProc || !g_hwnd) return;
    g_wndUnicode = IsWindowUnicode(g_hwnd) != FALSE;
    g_origWndProc = g_wndUnicode
        ? (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_WndProc)
        : (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_WndProc);
    Log("menu: window hook %s (unicode=%d)", g_origWndProc ? "ok" : "FAILED", g_wndUnicode);
}

// ---- DirectInput (via the game's import of dinput8.dll = GOG dixi)
typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DICreateDevice)(void*, REFGUID, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetState)(void*, DWORD, void*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetData)(void*, DWORD, void*, DWORD*, DWORD);
static PFN_DI8Create o_DI8Create = nullptr;
static const GUID kGUID_SysMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kGUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static void* g_diMouse = nullptr;
static void* g_diKeyboard = nullptr;
static volatile LONG g_miceX = 0, g_miceY = 0, g_miceZ = 0;
static volatile bool g_haveDIMouse = false;

// originals are kept per vtable: different device classes may have different vtables
struct VtOrig { void** vtbl; void* fn; };
static VtOrig g_vtCreate[4], g_vtState[8], g_vtData[8];
static void* FindOrig(VtOrig* t, int n, void* self)
{
    void** vt = *(void***)self;
    for (int i = 0; i < n; i++) if (t[i].vtbl == vt) return t[i].fn;
    return nullptr;
}
static void HookSlot(VtOrig* t, int n, void* self, int index, void* hook)
{
    void** vt = *(void***)self;
    for (int i = 0; i < n; i++) if (t[i].vtbl == vt) return;     // already done
    for (int i = 0; i < n; i++) if (!t[i].vtbl) {
        void* orig = PatchVtbl(self, index, hook);
        if (orig) { t[i].vtbl = vt; t[i].fn = orig; }
        return;
    }
}

static HRESULT STDMETHODCALLTYPE hk_DIGetState(void* self, DWORD cb, void* data)
{
    auto orig = (PFN_DIGetState)FindOrig(g_vtState, 8, self);
    HRESULT hr = orig ? orig(self, cb, data) : E_FAIL;
    if (SUCCEEDED(hr) && data) {
        if (self == g_diMouse && cb >= 12) {
            const LONG* st = (const LONG*)data;
            InterlockedExchangeAdd(&g_miceX, st[0]);
            InterlockedExchangeAdd(&g_miceY, st[1]);
            InterlockedExchangeAdd(&g_miceZ, st[2]);
            g_haveDIMouse = true;
        }
        if (g_menuOpen && (self == g_diMouse || self == g_diKeyboard)) memset(data, 0, cb);
    }
    return hr;
}

static void AccumulateMouseData(const void* rgdod, DWORD cbObj, DWORD n)
{
    const BYTE* p = (const BYTE*)rgdod;
    for (DWORD i = 0; i < n; i++, p += cbObj) {
        DWORD ofs = ((const DWORD*)p)[0];
        LONG  val = (LONG)((const DWORD*)p)[1];
        if (ofs == 0) InterlockedExchangeAdd(&g_miceX, val);
        else if (ofs == 4) InterlockedExchangeAdd(&g_miceY, val);
        else if (ofs == 8) InterlockedExchangeAdd(&g_miceZ, val);
    }
    g_haveDIMouse = true;
}

static HRESULT STDMETHODCALLTYPE hk_DIGetData(void* self, DWORD cbObj, void* rgdod, DWORD* inout, DWORD flags)
{
    auto orig = (PFN_DIGetData)FindOrig(g_vtData, 8, self);
    if (!orig) return E_FAIL;
    // Menu open: the game reads the mouse buffer in small chunks and stops when it gets nothing back,
    // so a fast mouse would pile up events (slow, laggy cursor). Drain the whole buffer here instead.
    if (g_menuOpen && self == g_diMouse && inout && cbObj >= 8 && cbObj <= 64 && !(flags & 1 /*DIGDD_PEEK*/)) {
        BYTE buf[64 * 64];
        for (int guard = 0; guard < 64; guard++) {
            DWORD n = sizeof(buf) / cbObj;
            HRESULT r = orig(self, cbObj, buf, &n, 0);
            if (FAILED(r)) break;
            AccumulateMouseData(buf, cbObj, n);
            if (n < sizeof(buf) / cbObj) break;
        }
        *inout = 0;
        return DI_OK;
    }
    HRESULT hr = orig(self, cbObj, rgdod, inout, flags);
    if (SUCCEEDED(hr) && inout) {
        if (self == g_diMouse && rgdod && cbObj >= 8) AccumulateMouseData(rgdod, cbObj, *inout);
        if (g_menuOpen && (self == g_diMouse || self == g_diKeyboard)) *inout = 0;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_DICreateDevice(void* self, REFGUID guid, void** dev, void* outer)
{
    auto orig = (PFN_DICreateDevice)FindOrig(g_vtCreate, 4, self);
    HRESULT hr = orig ? orig(self, guid, dev, outer) : E_FAIL;
    if (SUCCEEDED(hr) && dev && *dev) {
        const char* kind = "other";
        if (IsEqualGUID(guid, kGUID_SysMouse))    { g_diMouse = *dev; kind = "mouse"; }
        if (IsEqualGUID(guid, kGUID_SysKeyboard)) { g_diKeyboard = *dev; kind = "keyboard"; }
        HookSlot(g_vtState, 8, *dev, 9, (void*)hk_DIGetState);   // IDirectInputDevice8::GetDeviceState
        HookSlot(g_vtData, 8, *dev, 10, (void*)hk_DIGetData);    // IDirectInputDevice8::GetDeviceData
        Log("menu: DirectInput %s device %p hooked", kind, *dev);
    }
    return hr;
}

static HRESULT WINAPI hk_DI8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = o_DI8Create(inst, ver, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) HookSlot(g_vtCreate, 4, *out, 3, (void*)hk_DICreateDevice);  // IDirectInput8::CreateDevice
    Log("menu: DirectInput8Create -> 0x%08lx", hr);
    return hr;
}

// ---- XInput (imported by ordinal from xinput1_3.dll)
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, void*);
static PFN_XInputGetState o_XInputGetState = nullptr;
static DWORD WINAPI hk_XInputGetState(DWORD idx, void* state)
{
    DWORD r = o_XInputGetState(idx, state);
    if (r == 0 && g_menuOpen && state) memset((BYTE*)state + 4, 0, 12);   // keep dwPacketNumber, clear XINPUT_GAMEPAD
    return r;
}

static void** FindIATOrdinal(HMODULE mod, const char* dll, WORD ordinal)
{
    auto base = (uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat   = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat)
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal)
                return (void**)&iat->u1.Function;
    }
    return nullptr;
}

// Only hook an import slot that already holds a real function address (bound by the loader).
static void* SafePatchIAT(void** slot, void* hook)
{
    if (!slot || *slot == hook) return nullptr;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)*slot, &m)) {
        Log("menu: import slot %p not bound yet - skipped", slot);
        return nullptr;
    }
    return PatchIAT(slot, hook);
}

static void InstallInputHooks()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!o_DI8Create)
        if (void** s = FindIAT(exe, "dinput8.dll", "DirectInput8Create"))
            o_DI8Create = (PFN_DI8Create)SafePatchIAT(s, (void*)hk_DI8Create);
    if (!o_XInputGetState)
        if (void** s = FindIATOrdinal(exe, "xinput1_3.dll", 2))
            o_XInputGetState = (PFN_XInputGetState)SafePatchIAT(s, (void*)hk_XInputGetState);
    Log("menu: input hooks DirectInput8Create=%p XInputGetState=%p", (void*)o_DI8Create, (void*)o_XInputGetState);
}

// ---- settings persistence
static int g_saveFailures = 0;
static void WriteIniInt(const char* sec, const char* key, int v)
{
    char b[32]; snprintf(b, sizeof b, "%d", v);
    if (!WritePrivateProfileStringA(sec, key, b, g_ini)) {
        Log("menu: SAVE FAILED [%s] %s=%d (error %lu)", sec, key, v, GetLastError());
        g_saveFailures++;
        return;
    }
    // read it back: catches redirected/virtualized or locked files
    const int back = (int)GetPrivateProfileIntA(sec, key, v == -12345 ? 0 : -12345, g_ini);
    if (back != v) {
        Log("menu: SAVE MISMATCH [%s] %s wrote %d, file now says %d", sec, key, v, back);
        g_saveFailures++;
    }
}

// returns true when every value landed in the file
static bool SaveConfig()
{
    g_saveFailures = 0;
    const DWORD attr = GetFileAttributesA(g_ini);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        Log("menu: %s is READ-ONLY - settings can't be saved (clear the read-only flag)", g_ini);
    cfg.fxEnabled = g_fx.enabled ? 1 : 0;
    cfg.aoMode = g_fx.aoMode;
    WriteIniInt("Display", "Borderless", cfg.borderless);
    WriteIniInt("Game", "SkipIntros", cfg.skipIntros);
    WriteIniInt("Game", "NoOcclusionCulling", cfg.noOcclusion);
    WriteIniInt("Game", "DrawDistance", cfg.drawDistance);
    WritePrivateProfileStringA("Shaders", "Compiler", cfg.compiler == 2 ? "game" : cfg.compiler == 1 ? "47" : "auto", g_ini);
    WriteIniInt("Shadows", "Scale", cfg.shadowScale);
    WriteIniInt("Shadows", "Softness", cfg.shadowSoft);
    WriteIniInt("Tonemap", "Enable", cfg.tmEnable);
    WriteIniInt("Tonemap", "Knee", cfg.tmKnee);
    WriteIniInt("Tonemap", "White", cfg.tmWhite);
    WriteIniInt("Tonemap", "HuePreserve", cfg.tmHue);
    WriteIniInt("Tonemap", "ReplaceFinalPass", cfg.fxBlend);
    WriteIniInt("Tonemap", "Gamma", cfg.gammaMode);
    WriteIniInt("Effects", "BloomIntensity", cfg.bloomIntensity);
    WriteIniInt("Effects", "BloomThreshold", cfg.bloomThreshold);
    WriteIniInt("Effects", "BloomSmooth", cfg.bloomSmooth);
    WriteIniInt("Effects", "DepthOfField", cfg.dofStrength);
    WriteIniInt("Effects", "DepthOfFieldSmooth", cfg.dofSmooth);
    WriteIniInt("Effects", "FogDensity", cfg.fogDensity);
    WriteIniInt("Effects", "MotionBlur", cfg.motionBlur);
    WriteIniInt("Effects", "GameEdgeAA", cfg.edgeAA);
    WriteIniInt("Effects", "GameSharpen", cfg.gameSharpen);
    WriteIniInt("Shadows", "Filter", cfg.shadowFilter);
    WriteIniInt("Shadows", "Radius", cfg.shadowRadius);
    WriteIniInt("Lighting", "CharacterEven", cfg.charEven);
    WriteIniInt("Lighting", "CharacterFill", cfg.charFill);
    WriteIniInt("Glass", "Enable", cfg.glassEnable);
    WriteIniInt("Glass", "Opacity", cfg.glassClarity);
    WriteIniInt("Glass", "Reflection", cfg.glassRefl);
    WriteIniInt("Glass", "Fresnel", cfg.glassFresnel);
    WriteIniInt("Glass", "ReflectionContrast", cfg.glassReflContrast);
    WriteIniInt("Glass", "Bump", cfg.glassBump);
    WriteIniInt("Glass", "Refraction", cfg.glassRefract);
    WriteIniInt("Display", "VSync", cfg.vsync);
    WriteIniInt("Display", "FPSLimit", cfg.fpsLimit);
    WriteIniInt("Focus", "PauseOnFocusLoss", cfg.pauseOnFocusLoss);
    WriteIniInt("Focus", "ConfineCursor", cfg.confineCursor);
    WriteIniInt("PostFX", "Enabled", cfg.fxEnabled);
    WriteIniInt("PostFX", "BeforeHUD", cfg.beforeHud);
    WriteIniInt("PostFX", "FXAA", cfg.fxaa);
    WriteIniInt("PostFX", "Sharpen", cfg.sharpen);
    WriteIniInt("AO", "Enable", cfg.aoEnable);
    WriteIniInt("AO", "Mode", cfg.aoMode);
    WriteIniInt("AO", "Intensity", cfg.aoIntensity);
    WriteIniInt("AO", "Radius", cfg.aoRadius);
    WriteIniInt("AO", "FOV", cfg.aoFov);
    WriteIniInt("AO", "MaxDistance", cfg.aoMaxDist);
    WriteIniInt("AO", "NearFade", cfg.aoNear);
    WriteIniInt("AO", "ProtectHighlights", cfg.aoProtect);
    WriteIniInt("AO", "BehindGlass", cfg.aoGlass);
    WriteIniInt("AO", "FadeWithFog", cfg.aoFog);
    WriteIniInt("PostFX", "FXAAQuality", cfg.fxaaQuality);
    WriteIniInt("Reflections", "Enable", cfg.ssr);
    WriteIniInt("Reflections", "Strength", cfg.ssrStrength);
    WriteIniInt("Reflections", "Length", cfg.ssrLength);
    WriteIniInt("Reflections", "FloorsOnly", cfg.ssrFloor);
    WriteIniInt("Materials", "Enable", cfg.matEnable);
    WriteIniInt("Materials", "SkinScatter", cfg.sssStrength);
    WriteIniInt("Materials", "SkinScatterWidth", cfg.sssWidth);
    WriteIniInt("Materials", "MetalReflections", cfg.metalRefl);
    WriteIniInt("Materials", "MetalRoughness", cfg.metalRough);
    WriteIniInt("Reflections", "Roughness", cfg.ssrRough);
    WriteIniInt("Glass", "ScreenReflections", cfg.glassSsr);
    WriteIniInt("Effects", "ModernBloom", cfg.bloomModern);
    WriteIniInt("Effects", "ModernBloomThreshold", cfg.bloomModernThr);
    WriteIniInt("AO", "PreciseDepth", cfg.aoPrecise);
    WriteIniInt("Menu", "MouseSpeed", cfg.menuMouse);
    WriteIniInt("Grade", "Enable", cfg.gradeEnable);
    WriteIniInt("Grade", "Exposure", cfg.gradeExposure);
    WriteIniInt("Grade", "Contrast", cfg.gradeContrast);
    WriteIniInt("Grade", "Vibrance", cfg.gradeVibrance);
    WriteIniInt("Grade", "Saturation", cfg.gradeSaturation);
    WriteIniInt("Grade", "BlackLevel", cfg.gradeBlack);
    WriteIniInt("Grade", "Temperature", cfg.gradeTemp);
    WriteIniInt("Display", "RenderScale", cfg.renderScale);
    WriteIniInt("HDR", "Enable", cfg.hdrEnable);
    WriteIniInt("HDR", "PaperWhite", cfg.hdrPaper);
    WriteIniInt("HDR", "Curve", cfg.hdrCurve);
    WriteIniInt("HDR", "CompensateSRGB", cfg.hdrCompensate);
    WriteIniInt("HDR", "PeakNits", cfg.hdrPeak);
    WriteIniInt("HDR", "Highlights", cfg.hdrHighlights);
    WriteIniInt("HDR", "Saturation", cfg.hdrSaturation);
    WriteIniInt("Audio", "HRTF", cfg.hrtf);
    WriteIniInt("View", "Enable", cfg.viewEnable);
    WriteIniInt("View", "FOV", cfg.fovDeg);
    WriteIniInt("View", "HUD169", cfg.hudLimit);
    WriteIniInt("AO", "FOVAuto", cfg.aoFovAuto);
    if (g_saveFailures) Log("menu: save finished with %d failure(s) - %s", g_saveFailures, g_ini);
    else Log("menu: settings saved and verified - %s", g_ini);
    return g_saveFailures == 0;
}

// ---- ImGui frame
static bool  g_imguiInit = false;
static float g_vcx = 0, g_vcy = 0;            // virtual cursor (backbuffer pixels)
static LARGE_INTEGER g_imLast = {};
static char  g_menuNote[128] = "";
static DWORD g_menuNoteTime = 0;

static ImGuiKey VkToImGuiKey(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;       case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow; case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;       case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace; case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;  case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_MINUS: return ImGuiKey_Minus; case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_CONTROL: return ImGuiKey_LeftCtrl; case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_MENU: return ImGuiKey_LeftAlt;
    }
    return ImGuiKey_None;
}

static void MenuStyle(float scale)
{
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 4; st.FrameRounding = 3; st.GrabRounding = 3; st.TabRounding = 3;
    st.WindowPadding = ImVec2(14, 12); st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(10, 7);
    ImVec4* c = st.Colors;
    const ImVec4 accent(0.16f, 0.66f, 0.80f, 1.0f), accentDim(0.12f, 0.42f, 0.52f, 1.0f);
    c[ImGuiCol_WindowBg]         = ImVec4(0.05f, 0.07f, 0.10f, 0.94f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.06f, 0.10f, 0.14f, 1.0f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.08f, 0.16f, 0.22f, 1.0f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.10f, 0.14f, 0.19f, 1.0f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.14f, 0.22f, 0.29f, 1.0f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.16f, 0.27f, 0.35f, 1.0f);
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accentDim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button]           = ImVec4(0.12f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_ButtonHovered]    = accentDim;
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = ImVec4(0.12f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_HeaderHovered]    = accentDim;
    c[ImGuiCol_Tab]              = ImVec4(0.09f, 0.14f, 0.19f, 1.0f);
    c[ImGuiCol_TabHovered]       = accentDim;
    c[ImGuiCol_TabSelected]      = ImVec4(0.12f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_Separator]        = ImVec4(0.18f, 0.26f, 0.32f, 1.0f);
    st.ScaleAllSizes(scale);
}

static bool CheckboxInt(const char* label, int* v)
{
    bool b = *v != 0;
    bool changed = ImGui::Checkbox(label, &b);
    if (changed) *v = b ? 1 : 0;
    return changed;
}

static void MenuNote(const char* text)
{
    snprintf(g_menuNote, sizeof g_menuNote, "%s", text);
    g_menuNoteTime = GetTickCount();
}

static void DrawMenu()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("APRenewal 0.17.0", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Video")) {
            if (ImGui::BeginTabBar("video")) {
            if (ImGui::BeginTabItem("Image")) {
                bool en = g_fx.enabled;
                if (ImGui::Checkbox("Post-processing (F11)", &en)) g_fx.enabled = en;
                CheckboxInt("Apply before the UI (keeps HUD and menus sharp)", &cfg.beforeHud);
                CheckboxInt("FXAA anti-aliasing", &cfg.fxaa);
                if (cfg.fxaa) {
                    static const char* fq[] = { "Fast (console style)", "Quality (FXAA 3.11, edge search)" };
                    ImGui::Combo("FXAA type", &cfg.fxaaQuality, fq, 2);
                }
                ImGui::SliderInt("Sharpening", &cfg.sharpen, 0, 100, cfg.sharpen ? "%d %%" : "off");
                ImGui::SeparatorText("Display");
                ImGui::SliderInt("Frame cap", &cfg.fpsLimit, 0, 240, cfg.fpsLimit ? "%d fps" : "off");
                static const int caps[] = { 30, 60, 120, 144, 165 };
                for (int i = 0; i < 5; i++) {
                    if (i) ImGui::SameLine();
                    char b[8]; snprintf(b, sizeof b, "%d", caps[i]);
                    if (ImGui::Button(b)) cfg.fpsLimit = caps[i];
                }
                ImGui::TextDisabled("Above ~62 fps also needs MulderConfig's \"Uncap FPS\".");
                ImGui::Separator();
                ImGui::TextDisabled("Applied at next launch:");
                CheckboxInt("Borderless window", &cfg.borderless);
                ImGui::SliderInt("Render scale (supersampling)", &cfg.renderScale, 100, 200, cfg.renderScale > 100 ? "%d %%" : "off");
                if (g_ssaa) ImGui::TextDisabled("Now: rendering %ux%u, shown at %ux%u", g_ppW, g_ppH, g_outW, g_outH);
                else if (cfg.renderScale > 100 && !cfg.borderless) ImGui::TextDisabled("Supersampling needs the borderless window.");
                static const char* vs[] = { "Game decides", "Force off", "Force on" };
                int vi = cfg.vsync + 1;
                if (ImGui::Combo("VSync", &vi, vs, 3)) cfg.vsync = vi - 1;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Colour")) {
                CheckboxInt("Colour grading", &cfg.gradeEnable);
                ImGui::SliderInt("Exposure", &cfg.gradeExposure, -150, 150, "%d (1/100 stop)");
                ImGui::SliderInt("Contrast", &cfg.gradeContrast, 70, 160, "%d %%");
                ImGui::SliderInt("Vibrance", &cfg.gradeVibrance, -50, 150, "%d %%");
                ImGui::SliderInt("Saturation", &cfg.gradeSaturation, 0, 200, "%d %%");
                ImGui::SliderInt("Black level", &cfg.gradeBlack, -10, 30, "%d");
                ImGui::SliderInt("Temperature", &cfg.gradeTemp, -100, 100, cfg.gradeTemp ? (cfg.gradeTemp > 0 ? "%d warm" : "%d cool") : "neutral");
                if (ImGui::Button("Defaults (vivid)")) { cfg.gradeExposure = 10; cfg.gradeContrast = 106; cfg.gradeVibrance = 45; cfg.gradeSaturation = 100; cfg.gradeBlack = 1; cfg.gradeTemp = 0; }
                ImGui::SameLine();
                if (ImGui::Button("Neutral")) { cfg.gradeExposure = 0; cfg.gradeContrast = 100; cfg.gradeVibrance = 0; cfg.gradeSaturation = 100; cfg.gradeBlack = 0; cfg.gradeTemp = 0; }
                ImGui::TextDisabled("Runs on the 3D image only, before the HUD. Part of post-processing (F11).");
                ImGui::SeparatorText("Highlights and gamma");
                CheckboxInt("Highlight roll-off (instead of the game's hard clip)", &cfg.tmEnable);
                ImGui::SliderInt("Roll-off starts at", &cfg.tmKnee, 30, 95, "%d %% of white");
                ImGui::SliderInt("White point", &cfg.tmWhite, 150, 1000, "%d %%");
                ImGui::SliderInt("Keep highlight colour", &cfg.tmHue, 0, 100, "%d %%");
                if (ImGui::Button("Roll-off defaults")) { cfg.tmKnee = 60; cfg.tmWhite = 400; cfg.tmHue = 60; }
                CheckboxInt("Use APRenewal's final pass (roll-off, gamma, bloom, DOF)", &cfg.fxBlend);
                static const char* gm[] = { "Game's (square root, as shipped)", "sRGB (like modern games)", "Gamma 2.2" };
                ImGui::Combo("Gamma curve", &cfg.gammaMode, gm, 3);
                ImGui::TextDisabled("The game uses a cheap square root; sRGB / 2.2 lift the midtones a little.");
                ImGui::TextDisabled("Game's final pass: %d plain + %d DOF variant(s) recognised%s.", g_shCount[SK_BLEND_BLOOM], g_shCount[SK_BLEND_DOF],
                                    g_shReplFail ? ", some replacements FAILED (see log)" : "");
                ImGui::TextDisabled("Lamps, windows and sky keep detail instead of clipping to flat white. Toggle to compare.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Effects")) {
                ImGui::TextDisabled("The game's own effects, tuned inside its shaders. 100 %% = the game's look.");
                ImGui::SeparatorText("Bloom");
                ImGui::SliderInt("Bloom intensity", &cfg.bloomIntensity, 0, 300, cfg.bloomIntensity ? "%d %%" : "off");
                ImGui::SliderInt("Bloom threshold", &cfg.bloomThreshold, -50, 150, cfg.bloomThreshold ? "%+d" : "game's");
                CheckboxInt("Smooth bloom (no blocky, shimmering glow)", &cfg.bloomSmooth);
                ImGui::SliderInt("Wide HDR bloom", &cfg.bloomModern, 0, 200, cfg.bloomModern ? "%d %%" : "off");
                ImGui::SliderInt("Wide bloom threshold", &cfg.bloomModernThr, 0, 400, "%d %%");
                ImGui::TextDisabled("APRenewal's own bloom: a soft, wide glow from the HDR scene, added to the game's.");
                ImGui::SeparatorText("Depth of field");
                ImGui::SliderInt("Depth of field strength", &cfg.dofStrength, 0, 150, cfg.dofStrength ? "%d %%" : "off");
                CheckboxInt("Smooth depth of field blur", &cfg.dofSmooth);
                ImGui::TextDisabled("Mostly visible in dialogue close-ups.");
                ImGui::SeparatorText("Fog");
                ImGui::SliderInt("Fog density", &cfg.fogDensity, 0, 300, cfg.fogDensity ? "%d %%" : "off");
                ImGui::SeparatorText("Motion blur pass");
                ImGui::SliderInt("Motion blur", &cfg.motionBlur, 0, 200, cfg.motionBlur ? "%d %%" : "off");
                ImGui::SliderInt("Game's edge anti-aliasing", &cfg.edgeAA, 0, 100, cfg.edgeAA ? "%d %%" : "off");
                ImGui::SliderInt("Game's sharpening", &cfg.gameSharpen, 0, 300, cfg.gameSharpen ? "%d %%" : "off");
                ImGui::TextDisabled("The game's edge AA blurs along edges; with FXAA on you may prefer it lower.");
                if (ImGui::Button("Game defaults")) { cfg.bloomIntensity = 100; cfg.bloomThreshold = 0; cfg.dofStrength = 100; cfg.fogDensity = 100;
                                                       cfg.motionBlur = 100; cfg.edgeAA = 100; cfg.gameSharpen = 100; }
                ImGui::SeparatorText("Window glass");
            CheckboxInt("Improved glass", &cfg.glassEnable);
            ImGui::SliderInt("Glass opacity", &cfg.glassClarity, 10, 150, "%d %%");
            ImGui::SliderInt("Glass reflections", &cfg.glassRefl, 0, 400, "%d %%");
            ImGui::SliderInt("Angle-dependent reflections", &cfg.glassFresnel, 0, 100, "%d %%");
            ImGui::SliderInt("Reflection contrast (game's)", &cfg.glassReflContrast, 0, 100, "%d %%");
            ImGui::SliderInt("Glass bumpiness", &cfg.glassBump, 0, 300, "%d %%");
            ImGui::SliderInt("Glass refraction", &cfg.glassRefract, 0, 300, "%d %%");
            ImGui::SliderInt("Screen-space glass reflections", &cfg.glassSsr, 0, 200, cfg.glassSsr ? "%d %%" : "off (cubemap only)");
            ImGui::TextDisabled("Windows reflect the room from the previous frame; the cubemap fills in where it can't.");
            if (ImGui::Button("Game's glass")) { cfg.glassClarity = 100; cfg.glassRefl = 100; cfg.glassFresnel = 0; cfg.glassReflContrast = 100; cfg.glassBump = 100; cfg.glassRefract = 100; }
            ImGui::SameLine();
            if (ImGui::Button("APRenewal defaults")) { cfg.glassClarity = 65; cfg.glassRefl = 160; cfg.glassFresnel = 70; cfg.glassReflContrast = 30; cfg.glassBump = 100; cfg.glassRefract = 100; }
            ImGui::TextDisabled("%d glass material(s) recognised, %d used. Other glass: send a shader census.", g_shCount[SK_GLASS], g_shUsed[SK_GLASS]);
            if (!cfg.fxBlend) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Bloom and DOF tuning need \"Use APRenewal's final pass\" (Colour tab).");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Lighting & shadows")) {
                ImGui::SeparatorText("Shadows");
                static const char* sf[] = { "Game's (sharp, grainy)", "Soft (16 samples)", "Softer (32 samples)", "Smoothest (48 samples)" };
                ImGui::Combo("Shadow filter", &cfg.shadowFilter, sf, 4);
                if (cfg.shadowFilter > 0) ImGui::SliderInt("Shadow softness radius", &cfg.shadowRadius, 5, 120, "%d (1/10 texel)");
                static const char* sh[] = { "1x (game default)", "2x", "4x" };
                int si = cfg.shadowScale == 4 ? 2 : cfg.shadowScale == 2 ? 1 : 0;
                if (ImGui::Combo("Shadow resolution", &si, sh, 3)) cfg.shadowScale = si == 2 ? 4 : si == 1 ? 2 : 1;
                if (g_shScaleRun > 1 && cfg.shadowFilter == 0) {
                    ImGui::SliderInt("Shadow edge softness", &cfg.shadowSoft, 0, 100, cfg.shadowSoft ? "%d %%" : "sharp (matches the map)");
                    ImGui::TextDisabled("0 = filter sized for the bigger map, 100 = the game's original soft edges. %d shadow shaders adjusted.", g_shCount[SK_SHADOW]);
                }
                if (g_shadowBig) ImGui::TextDisabled("Now: shadow map %ux%u (game asked for %ux%u). Applies at next launch.", g_shadowBig, g_shadowBig, g_shadowSize, g_shadowSize);
                else ImGui::TextDisabled("Applies at next launch.");
                ImGui::SeparatorText("Reflections");
                CheckboxInt("Screen-space reflections on floors", &cfg.ssr);
                ImGui::SliderInt("Reflection strength", &cfg.ssrStrength, 0, 100, "%d %%");
                ImGui::SliderInt("Reflection distance", &cfg.ssrLength, 300, 5000, "%d units");
                ImGui::SliderInt("Only surfaces this flat", &cfg.ssrFloor, 30, 98, "%d %%");
                ImGui::SliderInt("Reflection blur (roughness)", &cfg.ssrRough, 0, 100, cfg.ssrRough ? "%d %%" : "mirror");
                ImGui::TextDisabled("Reflects what's on screen in floors and other upward-facing surfaces. Needs the view");
                ImGui::TextDisabled("matrix from the game (%s).", g_vpKey / 64u == g_frameNo ? "found" : "not seen this frame");
                ImGui::SeparatorText("Materials (skin, metal)");
                CheckboxInt("Recognise skin and metal materials", &cfg.matEnable);
                ImGui::SliderInt("Skin subsurface scattering", &cfg.sssStrength, 0, 100, cfg.sssStrength ? "%d %%" : "off");
                ImGui::SliderInt("Scattering width", &cfg.sssWidth, 25, 300, "%d %%");
                ImGui::SliderInt("Metal reflections", &cfg.metalRefl, 0, 100, cfg.metalRefl ? "%d %%" : "off");
                ImGui::SliderInt("Metal roughness", &cfg.metalRough, 0, 100, cfg.metalRough ? "%d %%" : "mirror");
                ImGui::TextDisabled("Light bleeds softly through skin; weapons and metal props reflect the scene (needs");
                ImGui::TextDisabled("screen-space reflections on). %d skin / %d metal shaders tagged, %s.", g_matTagCount[1], g_matTagCount[2],
                                    MatMaskValid() ? "mask drawn this frame" : "none on screen");
                ImGui::SeparatorText("Character lighting");
                ImGui::SliderInt("Even out lighting", &cfg.charEven, 0, 100, cfg.charEven ? "%d %%" : "game's");
                ImGui::SliderInt("Fill light", &cfg.charFill, 0, 150, cfg.charFill ? "+%d %%" : "game's");
                ImGui::TextDisabled("Characters and moving objects are lit from baked light probes; when a probe misses a lamp,");
                ImGui::TextDisabled("their front goes dark. Evening out lifts the dark side, fill adds ambient light.");
                ImGui::SeparatorText("Ambient occlusion");
                CheckboxInt("SSAO enabled", &cfg.aoEnable);
                static const char* modes[] = { "Off", "On", "Debug view (AO only)" };
                ImGui::Combo("Mode (F10)", &g_fx.aoMode, modes, 3);
                ImGui::SliderInt("Intensity", &cfg.aoIntensity, 0, 200, "%d %%");
                ImGui::SliderInt("Radius", &cfg.aoRadius, 5, 150, "%d units");
                CheckboxInt("Follow the game's field of view", &cfg.aoFovAuto);
                if (!cfg.aoFovAuto || !(g_viewSites & 2)) ImGui::SliderInt("Field of view", &cfg.aoFov, 50, 120, "%d deg");
                else ImGui::TextDisabled("Live FOV: %.1f deg", g_gameFov);
                ImGui::SliderInt("Max distance", &cfg.aoMaxDist, 500, 20000, "%d units");
                ImGui::SliderInt("Near fade", &cfg.aoNear, 0, 200, "%d units");
                ImGui::SliderInt("Protect highlights", &cfg.aoProtect, 0, 100, "%d %%");
                ImGui::SliderInt("Hide AO behind glass", &cfg.aoGlass, 0, 100, cfg.aoGlass ? "%d %%" : "off");
                ImGui::SliderInt("Fade AO with fog", &cfg.aoFog, 0, 100, cfg.aoFog ? "%d %%" : "off");
                CheckboxInt("Precise depth (24-bit INTZ, applies at next launch)", &cfg.aoPrecise);
                ImGui::TextDisabled("Method: %s", g_fx.aoIsGtao ? "GTAO" : "fallback (GTAO shader failed - see log)");
                ImGui::TextDisabled("Depth source: %s", (g_fx.aoIsGtao && cfg.aoPrecise && g_fx.zcopy && g_fx.fitValid) ? "24-bit depth buffer (calibrated)"
                                    : g_fx.intz ? "24-bit depth buffer (calibrating...)" : "16-bit HDR alpha");
                ImGui::TextDisabled("Tip: debug view + FOV slider. Flat walls should stay white.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("View")) {
                CheckboxInt("FOV and HUD fixes (replaces AlphaFixer)", &cfg.viewEnable);
                ImGui::SliderInt("Field of view", &cfg.fovDeg, 40, 130, "%d deg");
                ImGui::SameLine();
                if (ImGui::SmallButton("Game default")) cfg.fovDeg = 60;
                CheckboxInt("Keep HUD and menus 16:9 on wider screens", &cfg.hudLimit);
                if (!g_viewHooks) ImGui::TextDisabled("Not active - see the log (AlphaFixer's winmm.dll still installed?)");
                else {
                    ImGui::TextDisabled("Rendering %ux%u (aspect %.3f). Game FOV now: %.1f deg", g_ppW, g_ppH, ViewAspect(), g_gameFov);
                    ImGui::TextDisabled("The normal camera at 16:9 (game default 60, AlphaFixer at 1.20 = 85). Aim/zoom scale along. Live.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("HDR")) { if (!HdrAllowed()) {
                ImGui::TextWrapped("HDR is off on Windows: on the tested setup the output never got brighter than SDR white, "
                                   "so it only cost performance. For HDR on Windows use NVIDIA RTX HDR (NVIDIA app) with this off.");
                ImGui::TextWrapped("Under Proton with gamescope HDR (Steam Deck OLED, Linux desktops) it is available. "
                                   "To try it on Windows anyway: [HDR] AllowWindows=1 in APRenewal.ini.");
                ImGui::EndTabItem();
            } else {
                CheckboxInt("HDR output (applies at next launch)", &cfg.hdrEnable);
                CheckboxInt("Reference: plain SDR exactly as Windows shows it (A/B)", &cfg.hdrReference);
                ImGui::Checkbox("Test pattern (compare with hdr_testpattern.png in an SDR viewer)", &g_hdrPattern);
                CheckboxInt("Compensate: output gets decoded as sRGB twice (fixes washed-to-neon look)", &cfg.hdrCompensate);
                if (g_hdrPattern) ImGui::Checkbox("Highlight test: bottom row = 1x, 1.25x, 1.5x, 2x, 3x, 4x paper white", &g_hdrPatternHi);
                if (g_hdrPattern)
                    ImGui::TextWrapped("If the grey ramp turns flat white after about half way and the 75%% bars look like pure primaries, "
                                       "Windows is treating the game's HDR picture as SDR. On NVIDIA: Control Panel > Manage 3D settings > "
                                       "Vulkan/OpenGL present method > Prefer layered on DXGI Swapchain.");
                ImGui::SliderInt("Paper white", &cfg.hdrPaper, 0, 400, cfg.hdrPaper ? "%d nits" : "Windows setting");
                static const char* curves[] = { "sRGB (like Windows)", "Gamma 2.2 (deeper shadows)" };
                ImGui::Combo("SDR curve", &cfg.hdrCurve, curves, 2);
                ImGui::SliderInt("Peak brightness", &cfg.hdrPeak, 0, 4000, cfg.hdrPeak ? "%d nits" : "display's own");
                ImGui::SliderInt("Saturation", &cfg.hdrSaturation, 50, 130, "%d %%");
                if (g_hdrActive && HdrPaperNits() > HdrPeakNits() * 0.75f)
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Paper white is close to the display peak: whites will look blown out.");
                ImGui::SliderInt("Highlights", &cfg.hdrHighlights, 0, 150, cfg.hdrHighlights ? "%d %%" : "off (SDR in HDR)");
                if (g_hdrActive)
                    ImGui::TextDisabled("Active: %s - %s", g_hdrPQ ? "HDR10 (PQ)" : "scRGB", g_hdrDiag);
                if (g_hdrActive)
                    ImGui::TextDisabled("Paper white %.0f nits (Windows: %.0f), peak %.0f (display reports %.0f), exposure fit %.3g",
                                        HdrPaperNits(), g_winSdrWhite, HdrPeakNits(), g_hdrMaxNits, g_hdrK);
                else if (cfg.hdrEnable && g_vkHdrExt == 0)
                    ImGui::TextWrapped("Not active: Vulkan offers no HDR colour spaces here (%s). An old vulkan-1.dll in the game folder can cause this.", g_vkPath);
                else if (cfg.hdrEnable && !g_hdrUnlocked)
                    ImGui::TextDisabled("Not active: needs DXVK (set RealD3D9 in APRenewal.ini). See the README.");
                else if (cfg.hdrEnable && g_hdrForced)
                    ImGui::TextDisabled("Not active: no HDR colour space - turn on HDR in Windows display settings.");
                else ImGui::TextDisabled("Off.");
                ImGui::TextDisabled("Highlights come from the game's own HDR scene buffer; the HUD stays at paper white.");
                ImGui::EndTabItem();
            } }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            static const char* hm[] = { "Off", "On (headphones)", "Leave to OpenAL / alsoft.ini" };
            if (ImGui::Combo("HRTF 3D audio", &cfg.hrtf, hm, 3)) AlApplyHrtf();
            if (!g_alMod) ImGui::TextDisabled("OpenAL not loaded yet.");
            else if (!g_alSoft) ImGui::TextDisabled("Game is using Creative's OpenAL - copy APRenewal's OpenAL32.dll to Binaries for HRTF.");
            else {
                static const char* st[] = { "disabled", "enabled", "denied", "required", "headphones detected", "unsupported output" };
                ImGui::TextDisabled("OpenAL Soft on: %s", g_alDevName);
                ImGui::TextDisabled("HRTF: %s %s", (g_alHrtfStatus >= 0 && g_alHrtfStatus <= 5) ? st[g_alHrtfStatus] : "unknown", g_alHrtfName);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            CheckboxInt("Skip the intro logo movies", &cfg.skipIntros);
            ImGui::TextDisabled("Only movies before the first 3D scene; story movies still play.");
            CheckboxInt("No occlusion culling (fixes distant objects popping in late)", &cfg.noOcclusion);
            ImGui::TextDisabled("Every object the game would hide behind walls is drawn. %ld hidden draws rescued so far.", g_occForced);
            ImGui::SliderInt("Draw distance", &cfg.drawDistance, 50, 400, "%d %%");
            ImGui::SameLine();
            if (ImGui::Button("Apply")) ApplyDrawDistance();
            ImGui::TextDisabled("How far away objects are still drawn and keep their detailed models (100 = game). Applied at");
            ImGui::TextDisabled("every level load; needs the console.");
            ImGui::SeparatorText("Developer console");
            if (!g_conOk) ImGui::TextDisabled("Unavailable (see the log: console:).");
            else {
                const bool live = g_pc && GetTickCount() - g_pcTick < 3000;
                ImGui::TextDisabled(live ? "Ready. Unreal Engine 3 commands, e.g. slomo 0.5 / stat fps / show fog. Some are stripped from the release build."
                                         : "Waiting for a player (load a save or start a game).");
                ImGui::BeginChild("conhist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), ImGuiChildFlags_Borders);
                EnterCriticalSection(&g_conCs);
                for (auto& l : g_conHistory) ImGui::TextUnformatted(l.c_str());
                static size_t lastN = 0;
                if (g_conHistory.size() != lastN) { lastN = g_conHistory.size(); ImGui::SetScrollHereY(1.0f); }
                LeaveCriticalSection(&g_conCs);
                ImGui::EndChild();
                static char buf[256] = "";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##con", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) && buf[0]) {
                    ConQueue(buf);
                    buf[0] = 0;
                    ImGui::SetKeyboardFocusHere(-1);
                }
                ImGui::TextDisabled("Commands run when the game next pumps messages (close the menu if one seems stuck).");
                // Shortcuts: commands found in the game's own exec handler (APGame.exe), plus UE3 cheats
                struct Sc { const char* label; const char* cmd; const char* tip; };
                // Groups: engine / exe commands (APGame.exe exec handler) and UnrealScript exec functions
                // mapped from the cooked scripts (APGame.u / Engine.u, FUNC_Exec).
                static const Sc scView[] = {
                    { "Hide / show HUD", "TOGGLEUI", "Toggles the whole game UI (screenshots)." },
                    { "Screenshot", "SHOT", "Saved by the game to APGame\\ScreenShots." },
                    { "Hi-res screenshot (2x)", "TILEDSHOT 2", "Renders the frame in tiles at twice the resolution." },
                    { "Camera bob on/off", "ToggleCameraBob", "APPawn_Player: the camera's walking bob." },
                    { "Copyright text on/off", "ToggleCopyright", "APPawn_Player: the build/copyright overlay." },
                    { "Free camera (cheats)", "ToggleDebugCamera", "CheatManager: fly the camera around. Run 'Enable cheats' first; run again to return." },
                    { "Freeze world (cheats)", "PlayersOnly", "CheatManager: everything but the player stops - for photos. Run again to resume." },
                    { "FPS counter", "STAT FPS", "Toggles the engine's FPS display." },
                    { "Frame timing", "STAT UNIT", "Game / draw / GPU times." },
                };
                static const Sc scRender[] = {
                    { "Occlusion culling on/off", "TOGGLEOCCLUSION", "The game's own switch (APRenewal also has 'No occlusion culling' above)." },
                    { "High-quality character light on/off", "HQDLE", "Obsidian's high-quality dynamic light environments (on by default)." },
                    { "Character rim light on/off", "USEMAGICRIM", "Obsidian's 'magic rim' light on characters (on by default)." },
                    { "Door lighting fix on/off", "DOORLIGHTINGFIX", "Obsidian's fix for lighting on doors." },
                    { "DOF bleeding fix on/off", "TOGGLEDOFFIX", "" },
                    { "Aim depth of field on/off", "ToggleAimDOF", "APPlayerController: the blur while aiming." },
                    { "Lit view", "VIEWMODE LIT", "" },
                    { "Lighting only", "VIEWMODE LIGHTINGONLY", "Shows only lighting - nice for checking lights." },
                    { "Unlit", "VIEWMODE UNLIT", "" },
                    { "Wireframe", "VIEWMODE WIREFRAME", "" },
                    { "Fog on/off", "SHOW FOG", "" },
                    { "Particles on/off", "SHOW PARTICLES", "" },
                    { "Decals on/off", "SHOW DECALS", "" },
                    { "Post-processing on/off", "SHOW POSTPROCESS", "The game's own post-processing." },
                };
                static const Sc scPlay[] = {
                    { "Subtitles on/off", "ToggleSubtitles", "" },
                    { "Aim assist on/off", "ToggleAimAssist", "APPawn_Player." },
                    { "Recoil on/off", "ToggleRecoil", "APPawn_Player." },
                    { "Infinite ammo on/off", "ToggleInfiniteAmmo", "APPawn_Player." },
                    { "Slow motion", "slomo 0.3", "Needs 'Enable cheats'." },
                    { "Normal speed", "slomo 1", "" },
                    { "Enable cheats", "EnableCheats", "Creates UE3's cheat manager (God, Ghost, Walk, SloMo, ToggleDebugCamera, PlayersOnly...)." },
                    { "God mode (cheats)", "God", "CheatManager. Run again to turn off." },
                    { "Noclip / walk (cheats)", "Ghost", "CheatManager. 'Walk' returns to normal." },
                };
                auto group = [&](const char* title, const Sc* v, int n) {
                    ImGui::SeparatorText(title);
                    for (int i = 0; i < n; i++) {
                        if (i % 3) ImGui::SameLine();
                        ImGui::PushID(v[i].cmd);
                        if (ImGui::Button(v[i].label, ImVec2(ImGui::GetFontSize() * 11.0f, 0))) ConQueue(v[i].cmd);
                        ImGui::PopID();
                        if (v[i].tip[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", v[i].cmd, v[i].tip);
                        else if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", v[i].cmd);
                    }
                };
                group("Screenshots & camera", scView, (int)(sizeof scView / sizeof scView[0]));
                group("Rendering switches", scRender, (int)(sizeof scRender / sizeof scRender[0]));
                group("Gameplay", scPlay, (int)(sizeof scPlay / sizeof scPlay[0]));
                ImGui::TextDisabled("Value commands (type them): MINAMBIENT x, IRRADIANCESCALE x, IRRADIANCECONTRAST x,");
                ImGui::TextDisabled("LIGHTSCALE x, SHADOWINTENSITY x, SHARPNESS x, DESATURATE x, MOTIONBLURAMOUNT x, MAGICRIMSCALE x,");
                ImGui::TextDisabled("SetSensitivity x, SpecialSloMo x. Progress-changing ones (GiveMoney n, GiveAP n, GiveXP n,");
                ImGui::TextDisabled("UnlockVeteran, GiveAllWeapons...) also exist - they change your save, use with care.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            ImGui::Text("%.0f fps", io.Framerate);
            if (g_lastStatFrames) {
                ImGui::Text("Last 10 s: effects ran on %d of %d frames, depth captured in %d",
                            g_lastStatFx, g_lastStatFrames, g_lastStatDepth);
                ImGui::Text("Routes: switch %d | at Present %d | no-bloom %d | skipped %d | no 3D %d | off %d",
                            g_lastRoute[0], g_lastRoute[1], g_lastRoute[2], g_lastRoute[3], g_lastRoute[4], g_lastRoute[5]);
            }
            ImGui::Text("Input: DirectInput mouse %s, window hook %s",
                        g_diMouse ? "captured" : "not seen", g_origWndProc ? "ok" : "missing");
            {
                static const char* cn[] = { "Auto (Windows' d3dcompiler_47)", "d3dcompiler_47", "Game's D3DX (2009)" };
                if (ImGui::Combo("Shader compiler", &cfg.compiler, cn, 3)) MenuNote("Applies at next launch.");
                ImGui::TextDisabled("Used for APRenewal's own shaders: %s.", g_compilerUsed == 1 ? "d3dcompiler_47" : g_compilerUsed == 2 ? "the game's D3DX" : "none compiled yet");
            }
            if (ImGui::Button("Shader census (one frame)")) { g_censusArm = true; MenuNote("Shader list written to the log (census:)."); }
            ImGui::SameLine();
            if (ImGui::Button("Trace one frame")) { g_traceArmed = true; g_wantCapture = false; MenuNote("Frame trace written to the log."); }
            ImGui::SameLine();
            if (ImGui::Button("Capture one frame (images)")) { g_traceArmed = true; g_wantCapture = true; MenuNote("Capturing: see Binaries\\APRenewal\\capture_*"); }
            ImGui::TextDisabled("Log: Binaries\\APRenewal\\APRenewal.log");
            ImGui::SeparatorText("Game shaders recognised (used)");
            ImGui::Text("Character lighting: %d shaders (%s)", g_shCount[SK_CHARLIGHT], g_shUsed[SK_CHARLIGHT] ? "used" : "not seen yet");
            ImGui::Text("Final pass %d+%d (%d)  shadows %d (%d)  fog %d (%d)  motion blur %d (%d)  bloom %d (%d)",
                        g_shCount[SK_BLEND_BLOOM], g_shCount[SK_BLEND_DOF], g_shUsed[SK_BLEND_BLOOM] + g_shUsed[SK_BLEND_DOF],
                        g_shCount[SK_SHADOW], g_shUsed[SK_SHADOW], g_shCount[SK_FOG], g_shUsed[SK_FOG],
                        g_shCount[SK_MBLUR], g_shUsed[SK_MBLUR], g_shCount[SK_BLOOMGATHER], g_shUsed[SK_BLOOMGATHER]);
            if (g_shReplFail) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%d final-pass replacement(s) failed to build - see the log.", g_shReplFail);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Miscellaneous")) {
            ImGui::SeparatorText("Window and input");
            CheckboxInt("Pause the game when it loses focus", &cfg.pauseOnFocusLoss);
            CheckboxInt("Keep the cursor inside the game window", &cfg.confineCursor);
            ImGui::SliderInt("Menu cursor speed", &cfg.menuMouse, 25, 400, "%d %%");
            ImGui::TextDisabled("While this menu is open the game gets no keyboard, mouse or gamepad input.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save to APRenewal.ini"))
        MenuNote(SaveConfig() ? "Saved and verified." : "SAVE FAILED - see the log.");
    ImGui::SameLine();
    if (ImGui::Button("Reload from ini")) { LoadConfig(); ApplySavedToggles(); MenuNote("Reloaded."); }
    ImGui::TextDisabled("File: %s", g_ini);
    ImGui::SameLine();
    if (ImGui::Button("Close")) open = false;
    if (g_menuNote[0] && GetTickCount() - g_menuNoteTime < 4000) { ImGui::SameLine(); ImGui::TextDisabled("%s", g_menuNote); }
    ImGui::End();

    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_menuOpen = false; Log("menu closed"); }
}

static void MenuFrame(IDirect3DDevice9* dev)
{
    // toggle
    static bool keyWas = false;
    const bool key = (GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0;
    if (key && !keyWas && GameHasFocus()) {
        g_menuOpen = !g_menuOpen;
        if (g_menuOpen) {
            InterlockedExchange(&g_miceX, 0); InterlockedExchange(&g_miceY, 0); InterlockedExchange(&g_miceZ, 0);
            g_vcx = g_bbW * 0.5f; g_vcy = g_bbH * 0.5f;
        }
        Log(g_menuOpen ? "menu opened" : "menu closed");
    }
    keyWas = key;
    if (!g_menuOpen) {
        AcquireSRWLockExclusive(&g_evLock); g_evN = 0; ReleaseSRWLockExclusive(&g_evLock);
        return;
    }
    if (!g_bbW || dev->TestCooperativeLevel() != D3D_OK) return;

    if (!g_imguiInit) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;                 // window layout isn't persisted
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        const float scale = g_bbH / 1080.0f < 1.0f ? 1.0f : g_bbH / 1080.0f;
        char font[MAX_PATH]; GetWindowsDirectoryA(font, MAX_PATH); strncat(font, "\\Fonts\\segoeui.ttf", MAX_PATH - strlen(font) - 1);
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 18.0f * scale);
        else io.FontGlobalScale = scale;
        MenuStyle(scale);
        ImGui_ImplDX9_Init(dev);
        QueryPerformanceCounter(&g_imLast);
        g_imguiInit = true;
        Log("menu: ImGui %s ready (scale %.2f)", IMGUI_VERSION, scale);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_bbW, (float)g_bbH);
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    io.DeltaTime = (float)(now.QuadPart - g_imLast.QuadPart) / (float)freq.QuadPart;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
    g_imLast = now;

    // cursor: DirectInput mouse deltas drive a virtual cursor (the game may recenter the OS cursor)
    if (g_haveDIMouse) {
        const float k = cfg.menuMouse / 100.0f * (g_bbH / 1080.0f);
        g_vcx += InterlockedExchange(&g_miceX, 0) * k;
        g_vcy += InterlockedExchange(&g_miceY, 0) * k;
        const LONG wz = InterlockedExchange(&g_miceZ, 0);
        if (wz) io.AddMouseWheelEvent(0, wz / 120.0f);
    } else {
        POINT pt; GetCursorPos(&pt); ScreenToClient(g_hwnd, &pt);
        RECT rc; GetClientRect(g_hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) { g_vcx = pt.x * (float)g_bbW / rc.right; g_vcy = pt.y * (float)g_bbH / rc.bottom; }
    }
    if (g_vcx < 0) g_vcx = 0;
    if (g_vcy < 0) g_vcy = 0;
    if (g_vcx > g_bbW - 1) g_vcx = (float)g_bbW - 1;
    if (g_vcy > g_bbH - 1) g_vcy = (float)g_bbH - 1;
    io.AddMousePosEvent(g_vcx, g_vcy);
    const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(swap ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(swap ? VK_LBUTTON : VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    io.MouseDrawCursor = true;

    // queued keyboard / wheel events from the window procedure
    MenuEv evs[256]; int n;
    AcquireSRWLockExclusive(&g_evLock); n = g_evN; memcpy(evs, g_ev, n * sizeof(MenuEv)); g_evN = 0; ReleaseSRWLockExclusive(&g_evLock);
    for (int i = 0; i < n; i++) {
        const MenuEv& e = evs[i];
        if (e.type == 0) {
            const ImGuiKey k = VkToImGuiKey(e.a);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.b != 0);
            if (e.a == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            if (e.a == VK_SHIFT)   io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == VK_MENU)    io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
        } else if (e.type == 1) {
            if (e.a > 0 && e.a < 0x10000) io.AddInputCharacterUTF16((unsigned short)e.a);
        } else if (e.type == 2 && !g_haveDIMouse) {
            io.AddMouseWheelEvent(0, e.a / 120.0f);
        }
    }

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::EndFrame();
    ImGui::Render();
    g_inPostFX = true;                             // keep our draws out of frame detection / traces
    IDirect3DSurface9 *oldRT = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        dev->SetRenderTarget(0, bb);               // draw on the screen, whatever the game left bound
        if (SUCCEEDED(dev->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            dev->EndScene();
        }
        dev->SetRenderTarget(0, oldRT);
        bb->Release();
    }
    if (oldRT) oldRT->Release();
    g_inPostFX = false;
}

static void MenuOnReset()
{
    if (g_imguiInit) ImGui_ImplDX9_InvalidateDeviceObjects();   // recreated by the next NewFrame
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    TraceFrameBoundary(dev);
    PostFXHotkey();
    if (dev->TestCooperativeLevel() == D3D_OK) CalibrateDepth(dev);
    if (dev->TestCooperativeLevel() == D3D_OK) {
        // world target still bound at Present (no switch happened): process it now
        if (g_worldRT && g_curRT == g_worldRT && g_curRTDraws > 0 && !g_fxDoneThisFrame && cfg.beforeHud && g_fx.enabled) {
            g_inPostFX = true; PostFXRun(dev, g_worldRT, false); g_inPostFX = false;
            g_fxDoneThisFrame = true;
            g_frameRoute = 1;
        }
        // frames without a bloom chain: the final pass wrote straight to the screen. The 3D image is in
        // the backbuffer now and (in these frames) nothing UI-like follows it, so process it here.
        if (cfg.beforeHud && g_fx.enabled && !g_fxDoneThisFrame && g_sawHDR && !g_sawSmallRT) {
            IDirect3DSurface9* bbs = nullptr;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbs))) {
                g_inPostFX = true; PostFXRun(dev, bbs, false); g_inPostFX = false;
                bbs->Release();
                g_fxDoneThisFrame = true;
                g_frameRoute = 2;
            }
        }
        // legacy mode: whole final image incl. UI
        if (!cfg.beforeHud && g_fx.enabled) {
            IDirect3DSurface9* bbs = nullptr;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbs))) {
                g_inPostFX = true; PostFXRun(dev, bbs, false); g_inPostFX = false;
                bbs->Release();
            }
        }
    }
    // new frame: refresh backbuffer identity, reset detection
    {
        IDirect3DSurface9* bbs = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbs))) {
            D3DSURFACE_DESC d; bbs->GetDesc(&d);
            g_bbPtr = bbs; g_bbW = d.Width; g_bbH = d.Height;
            g_bbFmt = g_hdrForced ? g_bbLogicalFmt : d.Format;   // detection compares against the game's own format
            bbs->Release();
        }
        // once every ~10 s: how many frames actually got processed (helps diagnose flicker)
        g_statFrames++;
        if (!g_fxDoneThisFrame) g_frameRoute = !g_fx.enabled ? 5 : (g_sawHDR ? 3 : 4);
        g_route[g_frameRoute]++;
        g_frameRoute = 4;
        if (g_fxDoneThisFrame) g_statFx++;
        else if (g_sawHDR && g_fx.enabled && cfg.beforeHud && !g_skipLogged) {
            g_skipLogged = true;   // one sample per stats period
            char last[64] = "none";
            if (g_curRT) { D3DSURFACE_DESC ld; g_curRT->GetDesc(&ld);
                snprintf(last, sizeof last, "%s%ux%u/%d:%d", g_curRT == g_bbPtr ? "BB " : "", ld.Width, ld.Height, ld.Format, g_curRTDraws); }
            Log("postfx skipped a 3D frame (smallRT=%d world=%p depth=%d). sequence: %s| at Present: %s",
                g_sawSmallRT, (void*)g_worldRT, g_fx.depthValid, g_frameSeq, last);
        }
        if (g_fx.depthValid) g_statDepth++;
        DWORD now = GetTickCount();
        if (!g_statStart) g_statStart = now;
        if (now - g_statStart > 10000) {
            if (g_fx.enabled && cfg.beforeHud && g_route[3] > 0)
                Log("postfx stats: %d frames | switch %d, at-present %d, no-bloom %d, SKIPPED %d, no-3D %d, off %d | depth %d",
                    g_statFrames, g_route[0], g_route[1], g_route[2], g_route[3], g_route[4], g_route[5], g_statDepth);
            memcpy(g_lastRoute, g_route, sizeof g_route);
            memset(g_route, 0, sizeof g_route);
            g_lastStatFrames = g_statFrames; g_lastStatFx = g_statFx; g_lastStatDepth = g_statDepth;
            g_statFrames = g_statFx = g_statDepth = 0;
            g_statStart = now;
            g_skipLogged = false;
        }
        g_sawSmallRT = false; g_fxDoneThisFrame = false; g_worldRT = nullptr; g_curRTDraws = 0;
        g_scenesThisFrame = 0;
        g_fx.depthValid = false; g_fx.zValid = false; g_sawHDR = false;
        g_frameSeqLen = 0; g_frameSeq[0] = 0;
        g_hdrArm = false; g_hdrSceneSurf = nullptr;
    }
    if (g_hdrActive && g_hdrPattern && g_psHdrPattern) {      // test pattern under the menu
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC bd; bb->GetDesc(&bd);
            const float hp[4] = { g_hdrPatternHi ? 1.0f : 0.0f, 0, 0, 0 };
            XPass(dev, bb, bd.Width, bd.Height, nullptr, false, nullptr, false, g_psHdrPattern, hp, nullptr, false);
            bb->Release();
        }
    }
    MenuFrame(dev);
    const RECT* srcRect = PresentExtras(dev);
    HRESULT hr = srcRect ? o_Present(dev, srcRect, nullptr, c, d) : o_Present(dev, a, b, c, d);
    UpdateCursorClip(GameHasFocus());

    LimitFrame();
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    PostFXReleaseDefault();   // D3DPOOL_DEFAULT resources must be gone before Reset
    ExtrasRelease();
    MenuOnReset();
    IntzForget(); g_fx.intz = nullptr;
    g_rt0Scale = 1;
    GlassMaskRelease();
    BloomRelease();
    g_bbPtr = nullptr; g_curRT = nullptr; g_worldRT = nullptr; g_bbW = g_bbH = 0;
    DumpPP("Reset (game)", pp);
    ApplyPresentParams(pp, nullptr);
    const D3DFORMAT want = pp->BackBufferFormat;
    HRESULT hr = o_Reset(dev, pp);
    if (FAILED(hr) && g_hdrForced) {
        Log("hdr: Reset with an fp16 backbuffer failed (0x%08lx) - retrying SDR", hr);
        pp->BackBufferFormat = g_bbLogicalFmt; g_hdrForced = false;
        hr = o_Reset(dev, pp);
    }
    (void)want;
    if (SUCCEEDED(hr)) HdrSetupSwapchain(dev);
    DumpPP("Reset (applied)", pp);
    Log("Reset -> 0x%08lx", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
    DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    DumpPP("CreateDevice (game)", pp);
    Log("  adapter=%u type=%d behavior=0x%lx", adapter, type, behavior);
    ApplyPresentParams(pp, focus);
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, behavior, pp, out);
    if (FAILED(hr) && g_hdrForced) {
        Log("hdr: CreateDevice with an fp16 backbuffer failed (0x%08lx) - retrying SDR", hr);
        pp->BackBufferFormat = g_bbLogicalFmt; g_hdrForced = false;
        hr = o_CreateDevice(d3d, adapter, type, focus, behavior, pp, out);
    }
    if (SUCCEEDED(hr) && out && *out) HdrSetupSwapchain(*out);
    DumpPP("CreateDevice (applied)", pp);
    Log("CreateDevice -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out) {
        void* p;
        if ((p = PatchVtbl(*out, 16, (void*)hk_Reset)))   o_Reset   = (PFN_Reset)p;
        if ((p = PatchVtbl(*out, 17, (void*)hk_Present))) o_Present = (PFN_Present)p;
        if ((p = PatchVtbl(*out, 29, (void*)hk_CreateDS))) o_CreateDS = (PFN_CreateDS)p;
        if ((p = PatchVtbl(*out, 21, (void*)hk_SetGammaRamp))) o_SetGammaRamp = (PFN_SetGammaRamp)p;
        HookTraceVtbl(*out);
        InstallWndProcHook();
    }
    return hr;
}

static void HookD3D9(IDirect3D9* d3d)
{
    if (!d3d) return;
    if (void* p = PatchVtbl(d3d, 16, (void*)hk_CreateDevice)) o_CreateDevice = (PFN_CreateDevice)p;
    if (g_ssReqW) {
        if (void* p = PatchVtbl(d3d, 6, (void*)hk_GetAdapterModeCount)) o_GetAdapterModeCount = (PFN_GetAdapterModeCount)p;
        if (void* p = PatchVtbl(d3d, 7, (void*)hk_EnumAdapterModes))    o_EnumAdapterModes = (PFN_EnumAdapterModes)p;
    }
}

// --------------------------------------------------------------------------
// Real d3d9 loading + exports
// --------------------------------------------------------------------------
static void DetectCompat()
{
    typedef const char* (__cdecl *PFN_wgv)();
    if (HMODULE nt = GetModuleHandleA("ntdll.dll"))
        if (PFN_wgv wgv = (PFN_wgv)GetProcAddress(nt, "wine_get_version")) {
            const char* v = wgv(); snprintf(g_wineVer, sizeof g_wineVer, "%s", v ? v : "?");
        }
    char e[8] = "";
    g_steamDeck = GetEnvironmentVariableA("SteamDeck", e, sizeof e) && e[0] == '1';
    if (g_wineVer[0]) Log("compat: running under Wine %s%s", g_wineVer, g_steamDeck ? " on a Steam Deck" : "");
    else Log("compat: native Windows");
}

static bool LoadReal()
{
    if (g_real) return true;
    char path[MAX_PATH] = {};
    if (cfg.chain[0]) {
        if (strchr(cfg.chain, ':')) strncpy(path, cfg.chain, MAX_PATH - 1);
        else PathJoin(path, g_dir, cfg.chain);
        g_real = LoadLibraryA(path);
        Log("chain load '%s' -> %p", path, g_real);
    }
    if (!g_real) {
        GetSystemDirectoryA(path, MAX_PATH);
        strncat(path, "\\d3d9.dll", MAX_PATH - strlen(path) - 1);
        g_real = LoadLibraryA(path);
        Log("system d3d9 '%s' -> %p", path, g_real);
    }
    if (g_real && g_real == g_self) {   // Wine can hand our own module back for "d3d9.dll"
        Log("chain: '%s' resolved to APRenewal itself - set RealD3D9 to a DXVK d3d9 (e.g. d3d9_dxvk.dll)", path);
        g_real = nullptr;
    }
    return g_real != nullptr;
}

template <class T> static T Real(const char* name)
{
    return LoadReal() ? (T)GetProcAddress(g_real, name) : nullptr;
}

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    Log("Direct3DCreate9(sdk=%u)", sdk);
    InstallShaderHook();
    InstallClockHooks();
    InstallFocusHook();
    InstallInputHooks();   // retry, in case the import table wasn't bound yet at load time
    auto fn = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
    IDirect3D9* d3d = fn ? fn(sdk) : nullptr;
    HdrUnlock(d3d);
    HookD3D9(d3d);
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    Log("Direct3DCreate9Ex(sdk=%u)", sdk);
    InstallShaderHook();
    auto fn = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    HRESULT hr = fn ? fn(sdk, out) : E_FAIL;
    if (SUCCEEDED(hr) && out) HookD3D9((IDirect3D9*)*out);
    return hr;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n)
{ auto f = Real<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent"); return f ? f(c, n) : 0; }
int WINAPI D3DPERF_EndEvent()
{ auto f = Real<int(WINAPI*)()>("D3DPERF_EndEvent"); return f ? f() : 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)
{ auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"); if (f) f(c, n); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)
{ auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"); if (f) f(c, n); }
BOOL WINAPI D3DPERF_QueryRepeatFrame()
{ auto f = Real<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame"); return f ? f() : FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD o)
{ auto f = Real<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions"); if (f) f(o); }
DWORD WINAPI D3DPERF_GetStatus()
{ auto f = Real<DWORD(WINAPI*)()>("D3DPERF_GetStatus"); return f ? f() : 0; }

} // extern "C"

// --------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        g_mainThread = GetCurrentThreadId();   // loader runs us on the game's main thread
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logcs);

        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        if (char* s = strrchr(g_dir, '\\')) *s = 0;
        LoadConfig();

        char p[MAX_PATH];
        PathJoin(p, g_dir, "APRenewal");          CreateDirectoryA(p, nullptr);
        PathJoin(p, g_dir, "APRenewal\\dump");     if (cfg.shDump) CreateDirectoryA(p, nullptr);
        PathJoin(p, g_dir, "APRenewal\\override"); CreateDirectoryA(p, nullptr);
        PathJoin(p, g_dir, "APRenewal\\APRenewal.log");
        g_log = fopen(p, "w");

        Log("APRenewal 0.17.0 loaded. dir=%s", g_dir);
        Log("config: chain='%s' borderless=%d vsync=%d fps=%d shLog=%d shDump=%d shOverride=%d",
            cfg.chain, cfg.borderless, cfg.vsync, cfg.fpsLimit, cfg.shLog, cfg.shDump, cfg.shOverride);
        DetectCompat();
        ApplySavedToggles();
        InstallInputHooks();   // DirectInput / XInput imports, so the menu can capture input
        InstallSupersampling(); // must run before the game reads its command line
        InstallAudioHook();
        InstallViewHooks();
        InstallMovieHook();
        InstallConsole();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cursorClipped) ClipCursor(nullptr);
        Log("shutdown. total D3DXCompileShader calls: %ld", g_compileCount);
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
