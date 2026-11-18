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
#ifndef DI_OK
#define DI_OK S_OK
#endif
#include "imgui/imgui.h"
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
    int  hdrPaper      = 200;   // nits for UI / SDR white
    int  hdrPeak       = 0;     // nits, 0 = what the display reports
    int  hdrHighlights = 100;   // percent highlight expansion
    int  hdrEncoding   = 1;     // scRGB only (HDR10 + fp16 backbuffer froze the game with DXVK)
    int  hdrSaturation = 100;   // percent, HDR only
    int  hdrCurve      = 0;     // 0 = sRGB like Windows shows SDR, 1 = gamma 2.2
    int  hdrReference  = 0;     // A/B: plain SDR-in-HDR exactly as Windows would show it
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
    cfg.hdrPaper      = GetPrivateProfileIntA("HDR", "PaperWhite", 0, g_ini);   // 0 = Windows' SDR content brightness
    cfg.hdrCurve      = GetPrivateProfileIntA("HDR", "Curve", 0, g_ini) ? 1 : 0;
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

static BOOL WINAPI hk_PeekMessageW(LPMSG msg, HWND wnd, UINT fmin, UINT fmax, UINT remove)
{
    if (cfg.pauseOnFocusLoss && g_hwnd && !g_exiting && GetCurrentThreadId() == g_mainThread &&
        IsWindowVisible(g_hwnd) && !GameHasFocus())
        WaitWhileUnfocused();
    return o_PeekMessageW(msg, wnd, fmin, fmax, remove);
}

static void InstallFocusHook()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (void** s = FindIAT(GetModuleHandleA(nullptr), "user32.dll", "PeekMessageW"))
        o_PeekMessageW = (PFN_PeekMessageW)PatchIAT(s, (void*)hk_PeekMessageW);
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
float Sel(float x, float lo, float hi) { return step(lo, x) * (1 - step(hi, x)); }
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
    return float4(lerp(c / 80.0, PQ(max(c2020, 0)), e.z), 1);
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
float4 opt : register(c4);        // x = debug view, y = highlight protection 0..1
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2D(sColor, uv).rgb;
    float ao = tex2D(sAO, uv).r;
    if (opt.x > 0.5) return float4(ao, ao, ao, 1);
    float l = dot(c, float3(0.299, 0.587, 0.114));
    ao = lerp(ao, 1, smoothstep(0.65, 1.0, l) * opt.y);   // soft: keeps lamps from greying without popping
    return float4(c * ao, 1);
}
)HLSL";

struct PostFX {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* psFxaa = nullptr;
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

static void* CompileBlob(const char* src, const char* profile, const char* name)
{
    PFN_D3DXCompileShader compile = o_D3DXCompileShader;
    if (!compile) {
        HMODULE dx = GetModuleHandleA("d3dx9_42.dll");
        if (dx) compile = (PFN_D3DXCompileShader)GetProcAddress(dx, "D3DXCompileShader");
    }
    if (!compile) { Log("postfx: D3DXCompileShader unavailable"); return nullptr; }
    void* code = nullptr; void* err = nullptr;
    HRESULT hr = compile(src, (UINT)strlen(src), nullptr, nullptr, "main", profile, 0, &code, &err, nullptr);
    if (FAILED(hr) || !code) {
        Log("postfx: %s compile failed 0x%08lx: %s", name, hr, err ? (const char*)BufPtr(err) : "");
        if (code) { BufRelease(code); code = nullptr; }
    }
    if (err) BufRelease(err);
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
    if (!doAO && !doFxaa && !doSharp && !doGrade) return;
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

        int remaining = (doAO ? 1 : 0) + (doGrade ? 1 : 0) + (doFxaa ? 1 : 0) + (doSharp ? 1 : 0);
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
            const float c4[4] = { g_fx.aoMode == 2 ? 1.0f : 0.0f, cfg.aoProtect / 100.0f, 0, 0 };
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
            IDirect3DSurface9* o = nextOut(&outTex);
            DrawFullscreen(dev, cur, o, g_fx.psAOComp);                       // color * AO
            dev->SetTexture(1, nullptr);
            if (outTex) cur = outTex;
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
            DrawFullscreen(dev, cur, o, g_fx.psFxaa);
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
static int  g_scenesThisFrame = 0;
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
                  IDirect3DPixelShader9* ps, const float* c1, const float* c2, bool inScene)
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
        XPass(dev, bb, iw, ih, g_xCopy, false, nullptr, false, g_psHdrEncode, e, e2, false);
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

static void HdrUnlock(IDirect3D9* d3d)
{
    if (!cfg.hdrEnable || !d3d) return;
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
            Log("hdr: the display/driver offers no HDR colour space - is HDR on in Windows? Staying SDR");
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

static HRESULT STDMETHODCALLTYPE hk_SetRT(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* s)
{
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

static void AfterDraw(IDirect3DDevice9* d)
{
    if (g_hdrArm && !g_inPostFX && g_curRT == g_bbPtr) { g_hdrArm = false; HdrExpand(d); }
}
static HRESULT STDMETHODCALLTYPE hk_DP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{ TraceDraw(false); HRESULT hr = o_DP(d, t, a, b); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{ TraceDraw(false); HRESULT hr = o_DIP(d, t, a, b, c, e, f); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, const void* v, UINT s)
{ TraceDraw(true); HRESULT hr = o_DPUP(d, t, a, v, s); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_DIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i, D3DFORMAT fm, const void* v, UINT s)
{ TraceDraw(true); HRESULT hr = o_DIPUP(d, t, a, b, c, i, fm, v, s); AfterDraw(d); return hr; }
static HRESULT STDMETHODCALLTYPE hk_SetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
{ if (g_tracing && !g_inPostFX) g_segPS++; return o_SetPS(d, ps); }

static void HookTraceVtbl(IDirect3DDevice9* dev)
{
    void* p;
    if ((p = PatchVtbl(dev, 34, (void*)hk_StretchRect))) o_StretchRect = (PFN_StretchRect)p;
    if ((p = PatchVtbl(dev, 37, (void*)hk_SetRT)))       o_SetRT = (PFN_SetRT)p;
    if ((p = PatchVtbl(dev, 43, (void*)hk_Clear)))       o_Clear = (PFN_Clear)p;
    if ((p = PatchVtbl(dev, 81, (void*)hk_DP)))          o_DP = (PFN_DP)p;
    if ((p = PatchVtbl(dev, 82, (void*)hk_DIP)))         o_DIP = (PFN_DIP)p;
    if ((p = PatchVtbl(dev, 83, (void*)hk_DPUP)))        o_DPUP = (PFN_DPUP)p;
    if ((p = PatchVtbl(dev, 84, (void*)hk_DIPUP)))       o_DIPUP = (PFN_DIPUP)p;
    if ((p = PatchVtbl(dev, 107, (void*)hk_SetPS)))      o_SetPS = (PFN_SetPS)p;
}

// Called at every Present (before PostFX). Ends a running trace, starts an armed one.
static void TraceFrameBoundary(IDirect3DDevice9* dev)
{
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
    ImGui::Begin("APRenewal 0.9.5", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Image")) {
            bool en = g_fx.enabled;
            if (ImGui::Checkbox("Post-processing (F11)", &en)) g_fx.enabled = en;
            CheckboxInt("Apply before the UI (keeps HUD and menus sharp)", &cfg.beforeHud);
            CheckboxInt("FXAA anti-aliasing", &cfg.fxaa);
            ImGui::SliderInt("Sharpening", &cfg.sharpen, 0, 100, cfg.sharpen ? "%d %%" : "off");
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
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("HDR")) {
            CheckboxInt("HDR output (applies at next launch)", &cfg.hdrEnable);
            CheckboxInt("Reference: plain SDR exactly as Windows shows it (A/B)", &cfg.hdrReference);
            ImGui::Checkbox("Test pattern (compare with hdr_testpattern.png in an SDR viewer)", &g_hdrPattern);
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
        if (ImGui::BeginTabItem("Ambient occlusion")) {
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
            CheckboxInt("Precise depth (24-bit INTZ, applies at next launch)", &cfg.aoPrecise);
            ImGui::TextDisabled("Method: %s", g_fx.aoIsGtao ? "GTAO" : "fallback (GTAO shader failed - see log)");
            ImGui::TextDisabled("Depth source: %s", (g_fx.aoIsGtao && cfg.aoPrecise && g_fx.zcopy && g_fx.fitValid) ? "24-bit depth buffer (calibrated)"
                                : g_fx.intz ? "24-bit depth buffer (calibrating...)" : "16-bit HDR alpha");
            ImGui::TextDisabled("Tip: debug view + FOV slider. Flat walls should stay white.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
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
        if (ImGui::BeginTabItem("Window & input")) {
            CheckboxInt("Pause the game when it loses focus", &cfg.pauseOnFocusLoss);
            CheckboxInt("Keep the cursor inside the game window", &cfg.confineCursor);
            ImGui::SliderInt("Menu cursor speed", &cfg.menuMouse, 25, 400, "%d %%");
            ImGui::TextDisabled("While this menu is open the game gets no keyboard, mouse or gamepad input.");
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
            if (ImGui::Button("Trace one frame")) { g_traceArmed = true; g_wantCapture = false; MenuNote("Frame trace written to the log."); }
            ImGui::SameLine();
            if (ImGui::Button("Capture one frame (images)")) { g_traceArmed = true; g_wantCapture = true; MenuNote("Capturing: see Binaries\\APRenewal\\capture_*"); }
            ImGui::TextDisabled("Log: Binaries\\APRenewal\\APRenewal.log");
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
            XPass(dev, bb, bd.Width, bd.Height, nullptr, false, nullptr, false, g_psHdrPattern, nullptr, nullptr, false);
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

        Log("APRenewal 0.9.5 loaded. dir=%s", g_dir);
        Log("config: chain='%s' borderless=%d vsync=%d fps=%d shLog=%d shDump=%d shOverride=%d",
            cfg.chain, cfg.borderless, cfg.vsync, cfg.fpsLimit, cfg.shLog, cfg.shDump, cfg.shOverride);
        ApplySavedToggles();
        InstallInputHooks();   // DirectInput / XInput imports, so the menu can capture input
        InstallSupersampling(); // must run before the game reads its command line
        InstallAudioHook();
        InstallViewHooks();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cursorClipped) ClipCursor(nullptr);
        Log("shutdown. total D3DXCompileShader calls: %ld", g_compileCount);
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
