// pad.h - APRenewal's controller layer (replaces GOG's "dixi" dinput8.dll / xinput1_3.dll).
//
// The game reads one Xbox 360 pad through XInputGetState / XInputSetState (xinput1_3.dll, ordinals 2/3).
// We present a single virtual pad on slot 0, merged from every controller that is plugged in:
//   - real XInput pads (Xbox 360 / One / Series over USB, wireless adapter or Bluetooth), via the
//     system's xinput1_4 / xinput9_1_0 / xinput1_3
//   - Sony DualSense / DualSense Edge / DualShock 4, read directly over HID (USB and Bluetooth),
//     with rumble and an optional light bar colour
//   - any other DirectInput gamepad (Switch Pro, DS3, Logitech in D mode, DragonRise / PS2 adapters,
//     ...) through built-in or ini button maps
// Buttons are OR-ed, triggers take the maximum, each stick comes from whichever source moves it most,
// so a controller that shows up twice (Steam Input / DS4Windows virtual pad + the real one) is harmless.
// Ships with a tiny xinput1_3.dll whose exports forward to APR_XInput* in this module, so the game
// starts on PCs without the DirectX 9 runtime's xinput1_3 once dixi is gone.
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

// ---------------------------------------------------------------- config
struct PadCfg {
    int enable   = 1;    // 0: plain passthrough to the system XInput (or dixi, if still installed)
    int xinput   = 1;    // real XInput pads
    int sony     = 1;    // DualSense / DualShock 4 over HID
    int dinput   = 1;    // other DirectInput gamepads
    int rumble   = 100;  // %
    int deadzone = 0;    // % radial stick dead zone on top of the game's own (0 = leave to the game)
    int touchpadBack = 1;   // Sony touchpad click = Back
    int light    = -1;   // Sony light bar 0xRRGGBB, -1 = leave alone
    char remap[256] = "";   // "A=B B=A" ... on the final Xbox buttons
    int autoSwitch = 1;  // flip the game's own "Enable gamepad" option to whatever was used last
};
static PadCfg g_padCfg;

enum : WORD {
    XB_UP = 0x0001, XB_DOWN = 0x0002, XB_LEFT = 0x0004, XB_RIGHT = 0x0008, XB_START = 0x0010, XB_BACK = 0x0020,
    XB_LS = 0x0040, XB_RS = 0x0080, XB_LB = 0x0100, XB_RB = 0x0200, XB_GUIDE = 0x0400,
    XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};
static const struct { const char* n; WORD b; } kXbNames[] = {
    { "UP", XB_UP }, { "DOWN", XB_DOWN }, { "LEFT", XB_LEFT }, { "RIGHT", XB_RIGHT }, { "START", XB_START },
    { "BACK", XB_BACK }, { "LS", XB_LS }, { "RS", XB_RS }, { "LB", XB_LB }, { "RB", XB_RB }, { "GUIDE", XB_GUIDE },
    { "A", XB_A }, { "B", XB_B }, { "X", XB_X }, { "Y", XB_Y },
};
static WORD XbByName(const char* s)
{
    for (auto& e : kXbNames) if (!_stricmp(e.n, s)) return e.b;
    return 0;
}

struct PadState { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XIGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XIState { DWORD dwPacketNumber; XIGamepad Gamepad; };
struct XIVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };

static SRWLOCK g_padLock = SRWLOCK_INIT;
static volatile bool g_padStarted = false;
static char g_padInfo[8][96];          // connected devices, for the menu
static int  g_padInfoN = 0;

static void PadLoadConfig()
{
    PadCfg& c = g_padCfg;
    c.enable   = GetPrivateProfileIntA("Controller", "Enable", 1, g_ini) ? 1 : 0;
    c.xinput   = GetPrivateProfileIntA("Controller", "XInput", 1, g_ini) ? 1 : 0;
    c.sony     = GetPrivateProfileIntA("Controller", "Sony", 1, g_ini) ? 1 : 0;
    c.dinput   = GetPrivateProfileIntA("Controller", "DirectInput", 1, g_ini) ? 1 : 0;
    c.rumble   = std::min(std::max((int)GetPrivateProfileIntA("Controller", "Rumble", 100, g_ini), 0), 200);
    c.deadzone = std::min(std::max((int)GetPrivateProfileIntA("Controller", "Deadzone", 0, g_ini), 0), 50);
    c.touchpadBack = GetPrivateProfileIntA("Controller", "TouchpadIsBack", 1, g_ini) ? 1 : 0;
    char v[64] = "";
    GetPrivateProfileStringA("Controller", "LightBar", "", v, sizeof v, g_ini);
    c.light = v[0] ? (int)(strtoul(v[0] == '#' ? v + 1 : v, nullptr, 16) & 0xFFFFFF) : -1;
    GetPrivateProfileStringA("Controller", "Remap", "", c.remap, sizeof c.remap, g_ini);
    c.autoSwitch = GetPrivateProfileIntA("Controller", "AutoSwitch", 1, g_ini) ? 1 : 0;
}
static void PadSaveConfig()
{
    auto W = [](const char* k, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); WritePrivateProfileStringA("Controller", k, b, g_ini); };
    W("Enable", g_padCfg.enable); W("XInput", g_padCfg.xinput); W("Sony", g_padCfg.sony); W("DirectInput", g_padCfg.dinput);
    W("AutoSwitch", g_padCfg.autoSwitch); W("Rumble", g_padCfg.rumble); W("Deadzone", g_padCfg.deadzone); W("TouchpadIsBack", g_padCfg.touchpadBack);
    char b[16] = "";
    if (g_padCfg.light >= 0) snprintf(b, sizeof b, "%06X", g_padCfg.light);
    WritePrivateProfileStringA("Controller", "LightBar", b, g_ini);
    WritePrivateProfileStringA("Controller", "Remap", g_padCfg.remap, g_ini);
}

// ---------------------------------------------------------------- real XInput
typedef DWORD (WINAPI *PFN_XIGetState)(DWORD, XIState*);
typedef DWORD (WINAPI *PFN_XISetState)(DWORD, XIVibration*);
static PFN_XIGetState g_xiGet = nullptr;
static PFN_XISetState g_xiSet = nullptr;
static DWORD g_xiRetry[4] = {};        // disconnected slots are slow to poll: retry once a second
static bool  g_xiConn[4] = {};

static void PadLoadXInput()
{
    wchar_t sys[MAX_PATH]; UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n > MAX_PATH - 20) return;
    static const wchar_t* names[] = { L"\\xinput1_4.dll", L"\\xinput9_1_0.dll", L"\\xinput1_3.dll" };
    for (auto nm : names) {
        wchar_t p[MAX_PATH]; wcscpy(p, sys); wcscat(p, nm);
        HMODULE m = LoadLibraryW(p);
        if (!m) continue;
        if (m == g_self) continue;
        // ordinal 100 = XInputGetStateEx (also reports the Guide button) where it exists
        g_xiGet = (PFN_XIGetState)GetProcAddress(m, (LPCSTR)100);
        if (!g_xiGet) g_xiGet = (PFN_XIGetState)GetProcAddress(m, "XInputGetState");
        g_xiSet = (PFN_XISetState)GetProcAddress(m, "XInputSetState");
        if (g_xiGet) { Log("pad: XInput from %ls", p); return; }
    }
    Log("pad: no system XInput found - Xbox pads unavailable");
}

// ---------------------------------------------------------------- Sony over HID
static uint32_t PadCrc32(uint32_t crc, const BYTE* p, size_t n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
    return ~crc;
}

struct SonyDev {
    HANDLE h = INVALID_HANDLE_VALUE;
    char path[512] = "";
    int kind = 0;                 // 1 DualShock 4, 2 DualSense
    bool bt = false;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    PadState st = {};
    DWORD tick = 0;               // last report
    volatile LONG alive = 0;
    volatile LONG wantL = 0, wantR = 0;
    int sentL = -1, sentR = -1, sentLight = -2;
    bool ledInit = false;
    HANDLE wake = nullptr;
};
static SonyDev g_sony[4];

static SHORT PadAxis8(BYTE v, bool invert)
{
    int x = ((int)v - 128) * 258;     // 0..255 -> about -33024..32766
    if (invert) x = -x - 1;
    return (SHORT)std::min(std::max(x, -32768), 32767);
}
static WORD PadHat(int hat)
{
    static const WORD t[9] = { XB_UP, XB_UP | XB_RIGHT, XB_RIGHT, XB_DOWN | XB_RIGHT, XB_DOWN, XB_DOWN | XB_LEFT, XB_LEFT, XB_UP | XB_LEFT, 0 };
    return t[hat > 8 ? 8 : hat];
}
// buttons in the DS4 / DualSense layout: b0 = hat | face (square 0x10, cross 0x20, circle 0x40, triangle 0x80),
// b1 = L1 R1 L2 R2 share/create options L3 R3, b2 = PS, touchpad click, (mute)
static void SonyButtons(PadState& s, BYTE b0, BYTE b1, BYTE b2)
{
    WORD b = PadHat(b0 & 15);
    if (b0 & 0x10) b |= XB_X;
    if (b0 & 0x20) b |= XB_A;
    if (b0 & 0x40) b |= XB_B;
    if (b0 & 0x80) b |= XB_Y;
    if (b1 & 0x01) b |= XB_LB;
    if (b1 & 0x02) b |= XB_RB;
    if (b1 & 0x10) b |= XB_BACK;
    if (b1 & 0x20) b |= XB_START;
    if (b1 & 0x40) b |= XB_LS;
    if (b1 & 0x80) b |= XB_RS;
    if (b2 & 0x01) b |= XB_GUIDE;
    if ((b2 & 0x02) && g_padCfg.touchpadBack) b |= XB_BACK;
    s.buttons = b;
}
static bool SonyParse(SonyDev& d, const BYTE* r, DWORD n)
{
    PadState s = {};
    auto sticks = [&](const BYTE* p) { s.lx = PadAxis8(p[0], false); s.ly = PadAxis8(p[1], true); s.rx = PadAxis8(p[2], false); s.ry = PadAxis8(p[3], true); };
    if (d.kind == 2 && ((r[0] == 0x01 && !d.bt && n >= 11) || (r[0] == 0x31 && n >= 12))) {
        const BYTE* p = r + (r[0] == 0x31 ? 2 : 1);          // DualSense full report: sticks, L2, R2, counter, buttons
        sticks(p); s.lt = p[4]; s.rt = p[5]; SonyButtons(s, p[7], p[8], p[9]);
    } else if (d.kind == 1 && r[0] == 0x11 && n >= 12) {
        const BYTE* p = r + 3;                                  // DS4 Bluetooth full report
        sticks(p); SonyButtons(s, p[4], p[5], p[6]); s.lt = p[7]; s.rt = p[8];
    } else if (r[0] == 0x01 && n >= 10) {
        const BYTE* p = r + 1;                                  // DS4 USB, or the basic Bluetooth report of both
        sticks(p); SonyButtons(s, p[4], p[5], p[6]); s.lt = p[7]; s.rt = p[8];
    } else return false;
    AcquireSRWLockExclusive(&g_padLock);
    d.st = s; d.tick = GetTickCount();
    ReleaseSRWLockExclusive(&g_padLock);
    return true;
}

static bool SonyWrite(SonyDev& d, HANDLE ev)
{
    if (!d.outLen) return false;
    const int L = std::min((int)d.wantL, 255), R = std::min((int)d.wantR, 255), light = g_padCfg.light;
    if (L == d.sentL && R == d.sentR && light == d.sentLight) return true;
    BYTE buf[600] = {};
    const int len = std::max<int>(d.outLen, d.bt ? 78 : (d.kind == 1 ? 32 : 48));
    if (len > (int)sizeof buf) return false;
    const bool setLight = light >= 0;
    const BYTE lr = (BYTE)(light >> 16), lg = (BYTE)(light >> 8), lb = (BYTE)light;
    if (d.kind == 1) {
        BYTE* e;
        if (d.bt) { buf[0] = 0x11; buf[1] = 0xC4; buf[3] = 0x01 | (setLight ? 0x02 : 0); e = buf + 6; }
        else      { buf[0] = 0x05; buf[1] = 0x01 | (setLight ? 0x02 : 0); e = buf + 4; }
        e[0] = (BYTE)R; e[1] = (BYTE)L;                          // small (right) motor, big (left) motor
        if (setLight) { e[2] = lr; e[3] = lg; e[4] = lb; }
    } else {
        BYTE* e;
        if (d.bt) { buf[0] = 0x31; buf[1] = 0x02; e = buf + 2; }
        else      { buf[0] = 0x02; e = buf + 1; }
        e[0] = 0x01 | 0x02;                                     // compatible rumble, no audio haptics
        e[2] = (BYTE)R; e[3] = (BYTE)L;
        e[38] = 0x04;                                           // improved rumble emulation (firmware 2.24+)
        if (setLight) {
            e[1] |= 0x04;
            if (!d.ledInit) { e[1] |= 0x08; e[41] = 0x02; }      // release the light bar from its start-up animation
            e[44] = lr; e[45] = lg; e[46] = lb;
        }
    }
    if (d.bt) {                                                  // Bluetooth output reports carry a CRC32 seeded with 0xA2
        const BYTE hdr = 0xA2;
        uint32_t crc = PadCrc32(0, &hdr, 1);
        crc = PadCrc32(crc, buf, 74);
        memcpy(buf + 74, &crc, 4);
    }
    OVERLAPPED ov = {}; ov.hEvent = ev; ResetEvent(ev);
    DWORD wr = 0;
    BOOL ok = WriteFile(d.h, buf, (DWORD)(d.outLen ? d.outLen : len), &wr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, 200) == WAIT_OBJECT_0) ok = GetOverlappedResult(d.h, &ov, &wr, FALSE);
        else { CancelIo(d.h); ok = FALSE; }
    }
    if (ok) { d.sentL = L; d.sentR = R; d.sentLight = light; if (setLight) d.ledInit = true; }
    else {
        static int fails = 0;
        if (fails++ < 3) Log("pad: %s output report failed (error %lu)", d.kind == 1 ? "DualShock 4" : "DualSense", GetLastError());
        d.sentL = L; d.sentR = R; d.sentLight = light;           // don't hammer a device that refuses
    }
    return ok != FALSE;
}

static DWORD WINAPI SonyThread(LPVOID p)
{
    SonyDev& d = *(SonyDev*)p;
    if (d.bt && d.featLen) {
        // reading the calibration feature report switches a Bluetooth pad to its full reports (0x11 / 0x31)
        BYTE f[128] = {}; f[0] = d.kind == 1 ? 0x02 : 0x05;
        if (!HidD_GetFeature(d.h, f, std::min<ULONG>(d.featLen, sizeof f))) Log("pad: full-report request failed (%lu) - basic reports", GetLastError());
    }
    std::vector<BYTE> buf(std::max<USHORT>(d.inLen, 64) + 8);
    HANDLE rev = CreateEventW(nullptr, TRUE, FALSE, nullptr), wev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov = {}; ov.hEvent = rev;
    bool pending = false;
    DWORD lastWrite = 0;
    while (!g_exiting) {
        if (!pending) {
            ResetEvent(rev);
            DWORD got = 0;
            if (ReadFile(d.h, buf.data(), d.inLen, &got, &ov)) { SonyParse(d, buf.data(), got); continue; }
            if (GetLastError() != ERROR_IO_PENDING) break;
            pending = true;
        }
        HANDLE hs[2] = { rev, d.wake };
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, 250);
        if (w == WAIT_OBJECT_0) {
            DWORD got = 0;
            pending = false;
            if (!GetOverlappedResult(d.h, &ov, &got, FALSE)) break;
            SonyParse(d, buf.data(), got);
        }
        if (w == WAIT_OBJECT_0 + 1 || GetTickCount() - lastWrite > 1000) {
            ResetEvent(d.wake);
            SonyWrite(d, wev);
            lastWrite = GetTickCount();
        }
    }
    if (pending) CancelIo(d.h);
    Log("pad: %s %s disconnected", d.kind == 1 ? "DualShock 4" : "DualSense", d.bt ? "(Bluetooth)" : "(USB)");
    AcquireSRWLockExclusive(&g_padLock);
    CloseHandle(d.h); d.h = INVALID_HANDLE_VALUE; d.path[0] = 0; d.alive = 0;
    ReleaseSRWLockExclusive(&g_padLock);
    CloseHandle(rev); CloseHandle(wev);
    return 0;
}

static void SonyScan()
{
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); i++) {
        BYTE dbuf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512] = {};
        auto det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)dbuf; det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, sizeof dbuf, nullptr, nullptr)) continue;
        const char* path = det->DevicePath;
        // cheap filter before opening: Sony's vendor id in the path
        if (!strstr(path, "054c") && !strstr(path, "054C")) continue;
        bool known = false;
        for (auto& d : g_sony) if (d.alive && !_stricmp(d.path, path)) known = true;
        if (known) continue;
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            static char failed[512];
            if (strcmp(failed, path)) { snprintf(failed, sizeof failed, "%s", path); Log("pad: Sony device found but can't be opened (error %lu) - is DS4Windows hiding it?", GetLastError()); }
            continue;
        }
        HIDD_ATTRIBUTES at = { sizeof at };
        int kind = 0;
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C) {
            if (at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0) kind = 1;
            if (at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2) kind = 2;
        }
        HIDP_CAPS caps = {};
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (kind && HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        if (!kind || caps.UsagePage != 1 || caps.InputReportByteLength < 10) { CloseHandle(h); continue; }
        SonyDev* slot = nullptr;
        for (auto& d : g_sony) if (!d.alive) { slot = &d; break; }
        if (!slot) { CloseHandle(h); break; }
        SonyDev& d = *slot;
        d.h = h; snprintf(d.path, sizeof d.path, "%s", path); d.kind = kind;
        d.inLen = caps.InputReportByteLength; d.outLen = caps.OutputReportByteLength; d.featLen = caps.FeatureReportByteLength;
        d.bt = d.inLen != 64;                                    // USB pads use 64-byte reports
        d.st = {}; d.tick = 0; d.sentL = d.sentR = -1; d.sentLight = -2; d.ledInit = false;
        if (!d.wake) d.wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        d.alive = 1;
        Log("pad: %s (%04X:%04X) over %s - in %u / out %u / feature %u bytes", kind == 1 ? "DualShock 4" : (at.ProductID == 0x0DF2 ? "DualSense Edge" : "DualSense"),
            at.VendorID, at.ProductID, d.bt ? "Bluetooth" : "USB", d.inLen, d.outLen, d.featLen);
        HANDLE t = CreateThread(nullptr, 0, SonyThread, &d, 0, nullptr);
        if (t) CloseHandle(t); else { CloseHandle(h); d.h = INVALID_HANDLE_VALUE; d.alive = 0; }
    }
    SetupDiDestroyDeviceInfoList(set);
}

// ---------------------------------------------------------------- other DirectInput pads
// Map syntax (also for [Controller] Map_VVVVPPPP = ... in the ini): space-separated tokens
//   b<N>:<button>   DirectInput button N -> A B X Y LB RB LT RT BACK START LS RS GUIDE UP DOWN LEFT RIGHT
//   <axis>:<target> axis x y z rx ry rz s0 s1, optionally with + / - for one half of it,
//                   target LX LY RX RY LT RT (a leading - inverts; LY / RY are already flipped to "up = +")
//   pov:dpad        is implied
enum { PT_NONE, PT_LX, PT_LY, PT_RX, PT_RY, PT_LT, PT_RT };
struct DiMap { WORD btn[32]; BYTE btnTrig[32]; struct { BYTE target, half; bool inv; } ax[8]; };
static const char* kDiAxisNames[8] = { "x", "y", "z", "rx", "ry", "rz", "s0", "s1" };
static void DiParseMap(const char* s, DiMap& m)
{
    memset(&m, 0, sizeof m);
    char tok[64];
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        int n = 0;
        while (*s && *s != ' ' && *s != ',' && *s != '\t' && n < 63) tok[n++] = *s++;
        tok[n] = 0;
        char* c = strchr(tok, ':');
        if (!c) continue;
        *c = 0; const char* src = tok; const char* dst = c + 1;
        if ((src[0] == 'b' || src[0] == 'B') && isdigit((unsigned char)src[1])) {
            const int i = atoi(src + 1);
            if (i < 0 || i >= 32) continue;
            if (!_stricmp(dst, "LT")) m.btnTrig[i] = 1; else if (!_stricmp(dst, "RT")) m.btnTrig[i] = 2;
            else m.btn[i] = XbByName(dst);
            continue;
        }
        char an[8] = ""; int half = 0;
        snprintf(an, sizeof an, "%s", src);
        const size_t al = strlen(an);
        if (al && (an[al - 1] == '+' || an[al - 1] == '-')) { half = an[al - 1] == '+' ? 1 : 2; an[al - 1] = 0; }
        int ai = -1;
        for (int k = 0; k < 8; k++) if (!_stricmp(an, kDiAxisNames[k])) ai = k;
        if (ai < 0) continue;
        bool inv = false;
        if (*dst == '-') { inv = true; dst++; }
        int t = PT_NONE;
        if (!_stricmp(dst, "LX")) t = PT_LX; else if (!_stricmp(dst, "LY")) t = PT_LY;
        else if (!_stricmp(dst, "RX")) t = PT_RX; else if (!_stricmp(dst, "RY")) t = PT_RY;
        else if (!_stricmp(dst, "LT")) t = PT_LT; else if (!_stricmp(dst, "RT")) t = PT_RT;
        m.ax[ai].target = (BYTE)t; m.ax[ai].half = (BYTE)half; m.ax[ai].inv = inv;
    }
}
static const struct { DWORD vidpid; const char* name; const char* map; } kDiKnown[] = {
    // Sony through DirectInput (used when [Controller] Sony=0 or the pad can't be opened over HID)
    { 0x054C05C4, "DualShock 4", "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY rx:LT ry:RT" },
    { 0x054C09CC, "DualShock 4", "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY rx:LT ry:RT" },
    { 0x054C0BA0, "DualShock 4 adapter", "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY rx:LT ry:RT" },
    { 0x054C0CE6, "DualSense", "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY rx:LT ry:RT" },
    { 0x054C0DF2, "DualSense Edge", "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY rx:LT ry:RT" },
    { 0x054C0268, "DualShock 3", "b0:Y b1:B b2:A b3:X b4:LT b5:RT b6:LB b7:RB b8:START b9:BACK b10:LS b11:RS b12:GUIDE x:LX y:LY z:RX rz:RY" },
    { 0x057E2009, "Switch Pro Controller", "b0:A b1:B b2:X b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE b13:BACK x:LX y:LY z:RX rz:RY" },
    { 0x00790006, "DragonRise / PS2 adapter", "b0:Y b1:B b2:A b3:X b4:LT b5:RT b6:LB b7:RB b8:BACK b9:START b10:LS b11:RS x:LX y:LY z:RX rz:RY" },
    { 0x08100001, "PS2 adapter", "b0:Y b1:B b2:A b3:X b4:LT b5:RT b6:LB b7:RB b8:BACK b9:START b10:LS b11:RS x:LX y:LY z:RX rz:RY" },
    { 0x25630523, "Genesis P65", "b0:Y b1:B b2:A b3:X b4:LT b5:RT b6:LB b7:RB b8:BACK b9:START b10:LS b11:RS x:LX y:LY z:RX rz:RY" },
    { 0x25630575, "Genesis PV58 / PV65", "b0:Y b1:B b2:A b3:X b4:LT b5:RT b6:LB b7:RB b8:BACK b9:START b10:LS b11:RS x:LX y:LY z:RX rz:RY" },
};
// unknown pads: "D mode" layout (Logitech F310/F710 D, MSI, most generic USB pads) when there's an Rz
// axis, otherwise the Xbox-through-DirectInput layout (combined trigger axis on Z)
static const char* kDiGenericD = "b0:X b1:A b2:B b3:Y b4:LB b5:RB b6:LT b7:RT b8:BACK b9:START b10:LS b11:RS b12:GUIDE x:LX y:LY z:RX rz:RY";
static const char* kDiGenericX = "b0:A b1:B b2:X b3:Y b4:LB b5:RB b6:BACK b7:START b8:LS b9:RS x:LX y:LY rx:RX ry:RY z+:LT z-:RT";

struct DiDev { IDirectInputDevice8W* d; GUID inst; DWORD vidpid; DiMap map; PadState st; bool ok; char name[64]; };
static IDirectInput8W* g_di = nullptr;
static DiDev g_diDev[4] = {};
static int g_diN = 0;

static BOOL CALLBACK DiEnumAxes(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ctx)
{
    auto d = (IDirectInputDevice8W*)ctx;
    DIPROPRANGE r = {}; r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof r.diph;
    r.diph.dwHow = DIPH_BYID; r.diph.dwObj = o->dwType; r.lMin = -32768; r.lMax = 32767;
    d->SetProperty(DIPROP_RANGE, &r.diph);
    return DIENUM_CONTINUE;
}
static BOOL CALLBACK DiEnumDev(LPCDIDEVICEINSTANCEW di, LPVOID)
{
    if (g_diN >= 4) return DIENUM_STOP;
    for (int i = 0; i < g_diN; i++) if (IsEqualGUID(g_diDev[i].inst, di->guidInstance)) return DIENUM_CONTINUE;
    // devices already turned down (XInput pads seen through DirectInput): don't re-create them every scan
    static GUID skipped[16]; static int nSkipped = 0;
    for (int i = 0; i < nSkipped; i++) if (IsEqualGUID(skipped[i], di->guidInstance)) return DIENUM_CONTINUE;
    auto skip = [&]() { if (nSkipped < 16) skipped[nSkipped++] = di->guidInstance; };
    const DWORD pidvid = di->guidProduct.Data1;            // PID << 16 | VID
    const DWORD vid = pidvid & 0xFFFF, pid = pidvid >> 16, vidpid = vid << 16 | pid;
    if (vid == 0x054C && pid != 0x0268 && g_padCfg.sony) return DIENUM_CONTINUE;   // read over HID instead
    IDirectInputDevice8W* d = nullptr;
    if (FAILED(g_di->CreateDevice(di->guidInstance, &d, nullptr)) || !d) return DIENUM_CONTINUE;
    // XInput pads also show up in DirectInput (with "IG_" in their path): XInput reads them properly
    DIPROPGUIDANDPATH gp = {}; gp.diph.dwSize = sizeof gp; gp.diph.dwHeaderSize = sizeof gp.diph; gp.diph.dwHow = DIPH_DEVICE;
    if (SUCCEEDED(d->GetProperty(DIPROP_GUIDANDPATH, &gp.diph)) && (wcsstr(gp.wszPath, L"IG_") || wcsstr(gp.wszPath, L"ig_"))) {
        d->Release(); skip(); return DIENUM_CONTINUE;
    }
    if (FAILED(d->SetDataFormat(&c_dfDIJoystick2))) { d->Release(); skip(); return DIENUM_CONTINUE; }
    if (g_hwnd) d->SetCooperativeLevel(g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    d->EnumObjects(DiEnumAxes, d, DIDFT_AXIS);
    DiDev& e = g_diDev[g_diN];
    e = {}; e.d = d; e.inst = di->guidInstance; e.vidpid = vidpid;
    char key[32]; snprintf(key, sizeof key, "Map_%04lX%04lX", vid, pid);
    char user[512] = "";
    GetPrivateProfileStringA("Controller", key, "", user, sizeof user, g_ini);
    const char* map = nullptr; const char* nm = nullptr;
    for (auto& k : kDiKnown) if (k.vidpid == vidpid) { map = k.map; nm = k.name; }
    if (!map) {
        DIDEVICEOBJECTINSTANCEW oi = {}; oi.dwSize = sizeof oi;
        map = SUCCEEDED(d->GetObjectInfo(&oi, DIJOFS_RZ, DIPH_BYOFFSET)) ? kDiGenericD : kDiGenericX;
    }
    DiParseMap(user[0] ? user : map, e.map);
    WideCharToMultiByte(CP_UTF8, 0, di->tszProductName, -1, e.name, sizeof e.name, nullptr, nullptr);
    Log("pad: DirectInput \"%s\" (%04lX:%04lX)%s%s - %s map%s", e.name, vid, pid, nm ? " = " : "", nm ? nm : "",
        user[0] ? "ini" : (nm ? "built-in" : (map == kDiGenericD ? "generic D-mode" : "generic Xbox-style")),
        user[0] ? "" : "");
    AcquireSRWLockExclusive(&g_padLock);
    g_diN++;
    ReleaseSRWLockExclusive(&g_padLock);
    return DIENUM_CONTINUE;
}

static SHORT DiAxis(LONG v) { return (SHORT)std::min(std::max(v, -32768L), 32767L); }
static void DiPoll()
{
    for (int i = 0; i < g_diN; i++) {
        DiDev& e = g_diDev[i];
        if (!e.d) continue;
        DIJOYSTATE2 js = {};
        HRESULT hr = e.d->Poll();
        if (FAILED(hr)) { e.d->Acquire(); hr = e.d->Poll(); }
        hr = e.d->GetDeviceState(sizeof js, &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) { e.d->Acquire(); hr = e.d->GetDeviceState(sizeof js, &js); }
        if (FAILED(hr)) {
            if (hr != DIERR_NOTACQUIRED && hr != DIERR_INPUTLOST) {               // unplugged
                Log("pad: DirectInput \"%s\" gone (0x%08lx)", e.name, (unsigned long)hr);
                AcquireSRWLockExclusive(&g_padLock);
                e.d->Unacquire(); e.d->Release(); e.d = nullptr; e.ok = false;
                g_diDev[i] = g_diDev[--g_diN]; g_diDev[g_diN] = {};
                ReleaseSRWLockExclusive(&g_padLock);
                i--;
            }
            continue;
        }
        PadState s = {};
        for (int b = 0; b < 32; b++) if (js.rgbButtons[b] & 0x80) {
            s.buttons |= e.map.btn[b];
            if (e.map.btnTrig[b] == 1) s.lt = 255;
            if (e.map.btnTrig[b] == 2) s.rt = 255;
        }
        const DWORD pov = js.rgdwPOV[0];
        if (LOWORD(pov) != 0xFFFF) s.buttons |= PadHat((int)((pov + 2250) % 36000 / 4500));
        const LONG av[8] = { js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1] };
        for (int a = 0; a < 8; a++) {
            const auto& m = e.map.ax[a];
            if (m.target == PT_NONE) continue;
            LONG v = av[a];
            if (m.half == 1) v = std::max(v, 0L) * 2 - 32768;              // + half -> full range
            else if (m.half == 2) v = std::max(-v, 0L) * 2 - 32768;
            if (m.inv) v = -v - 1;
            if (m.target == PT_LT || m.target == PT_RT) {
                const BYTE t = (BYTE)std::min(std::max((v + 32768) >> 8, 0L), 255L);
                BYTE& dst = m.target == PT_LT ? s.lt : s.rt;
                dst = std::max(dst, t);
            } else {
                if (m.target == PT_LY || m.target == PT_RY) v = -v - 1;    // DirectInput: down = +
                const SHORT sv = DiAxis(v);
                if (m.target == PT_LX) s.lx = sv; else if (m.target == PT_LY) s.ly = sv;
                else if (m.target == PT_RX) s.rx = sv; else s.ry = sv;
            }
        }
        AcquireSRWLockExclusive(&g_padLock);
        e.st = s; e.ok = true;
        ReleaseSRWLockExclusive(&g_padLock);
    }
}

// ---------------------------------------------------------------- background thread
static DWORD WINAPI PadThread(LPVOID)
{
    g_padThreadId = GetCurrentThreadId();
    DWORD lastScan = 0;
    bool diTried = false;
    while (!g_exiting) {
        const DWORD now = GetTickCount();
        if (now - lastScan > 3000 || !lastScan) {
            lastScan = now;
            if (g_padCfg.enable && g_padCfg.sony) SonyScan();
            if (g_padCfg.enable && g_padCfg.dinput) {
                if (!g_di && !diTried) {
                    diTried = true;
                    typedef HRESULT (WINAPI *PFN)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
                    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dinput8.dll");
                    HMODULE m = LoadLibraryW(p);
                    PFN f = m ? (PFN)GetProcAddress(m, "DirectInput8Create") : nullptr;
                    if (!f || FAILED(f(g_self, DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&g_di, nullptr))) { g_di = nullptr; Log("pad: DirectInput unavailable"); }
                }
                if (g_di) g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, DiEnumDev, nullptr, DIEDFL_ATTACHEDONLY);
            }
        }
        if (g_di && g_diN) DiPoll();
        Sleep(g_diN ? 4 : 50);
    }
    return 0;
}

// ---------------------------------------------------------------- the virtual pad
static volatile LONG g_xiEnabled = 1;
static DWORD g_padPacket = 0;
static volatile DWORD g_padPolled = 0;  // last time the game asked for the pad
static XIGamepad g_padLast = {};
static WORD g_remapFrom[16], g_remapTo[16]; static int g_remapN = -1; static char g_remapSrc[256];

static void PadStart()
{
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) { while (!g_padStarted) Sleep(1); return; }
    PadLoadConfig();
    PadLoadXInput();
    if (g_padCfg.enable) {
        HANDLE t = CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        Log("pad: controller layer on (XInput %d, Sony HID %d, DirectInput %d, rumble %d%%)", g_padCfg.xinput, g_padCfg.sony, g_padCfg.dinput, g_padCfg.rumble);
    } else Log("pad: controller layer off - XInput passthrough");
    g_padStarted = true;
}

static void PadApplyRemap(WORD& b)
{
    if (g_remapN < 0 || strcmp(g_remapSrc, g_padCfg.remap)) {
        strcpy(g_remapSrc, g_padCfg.remap); g_remapN = 0;
        char tmp[256]; strcpy(tmp, g_remapSrc);
        for (char* t = strtok(tmp, " ,;"); t && g_remapN < 16; t = strtok(nullptr, " ,;")) {
            char* eq = strchr(t, '=');
            if (!eq) continue;
            *eq = 0;
            const WORD f = XbByName(t), to = XbByName(eq + 1);
            if (f && to) { g_remapFrom[g_remapN] = f; g_remapTo[g_remapN] = to; g_remapN++; }
        }
        if (g_remapN) Log("pad: %d button remaps", g_remapN);
    }
    if (!g_remapN) return;
    WORD out = b;
    for (int i = 0; i < g_remapN; i++) out &= ~g_remapFrom[i];
    for (int i = 0; i < g_remapN; i++) if (b & g_remapFrom[i]) out |= g_remapTo[i];
    b = out;
}

static void PadDeadzone(SHORT& x, SHORT& y)
{
    if (g_padCfg.deadzone <= 0) return;
    const float dz = g_padCfg.deadzone / 100.0f;
    float fx = x / 32767.0f, fy = y / 32767.0f;
    const float m = sqrtf(fx * fx + fy * fy);
    if (m <= dz) { x = y = 0; return; }
    const float k = std::min((m - dz) / (1 - dz), 1.0f) / m;
    x = (SHORT)std::min(std::max(fx * k * 32767.0f, -32768.0f), 32767.0f);
    y = (SHORT)std::min(std::max(fy * k * 32767.0f, -32768.0f), 32767.0f);
}

static void PadMergeStick(SHORT& dx, SHORT& dy, SHORT sx, SHORT sy)
{
    if ((int)sx * sx + (int)sy * sy > (int)dx * dx + (int)dy * dy) { dx = sx; dy = sy; }
}

// returns false when nothing is connected
static bool PadRead(XIGamepad& g)
{
    g = {};
    bool any = false;
    PadState m = {};
    auto add = [&](const PadState& s) {
        m.buttons |= s.buttons; m.lt = std::max(m.lt, s.lt); m.rt = std::max(m.rt, s.rt);
        PadMergeStick(m.lx, m.ly, s.lx, s.ly); PadMergeStick(m.rx, m.ry, s.rx, s.ry);
        any = true;
    };
    if (g_xiGet && (g_padCfg.xinput || !g_padCfg.enable)) {
        const DWORD now = GetTickCount();
        for (DWORD i = 0; i < 4; i++) {
            if (!g_xiConn[i] && now - g_xiRetry[i] < 1000 && g_xiRetry[i]) continue;
            struct { XIState s; DWORD pad[4]; } sx = {};       // GetStateEx's struct is a little larger
            const XIState& s = sx.s;
            g_xiConn[i] = g_xiGet(i, &sx.s) == ERROR_SUCCESS;
            if (!g_xiConn[i]) { g_xiRetry[i] = now ? now : 1; continue; }
            PadState p = { s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger,
                           s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY };
            add(p);
        }
    }
    if (g_padCfg.enable) {
        const DWORD now = GetTickCount();
        AcquireSRWLockShared(&g_padLock);
        for (auto& d : g_sony) if (d.alive && d.tick && now - d.tick < 1500) add(d.st);
        for (int i = 0; i < g_diN; i++) if (g_diDev[i].ok) add(g_diDev[i].st);
        ReleaseSRWLockShared(&g_padLock);
    }
    if (!any) return false;
    PadApplyRemap(m.buttons);
    PadDeadzone(m.lx, m.ly); PadDeadzone(m.rx, m.ry);
    g = { m.buttons, m.lt, m.rt, m.lx, m.ly, m.rx, m.ry };
    return true;
}

extern "C" DWORD WINAPI APR_XInputGetState(DWORD idx, XIState* st)
{
    if (!g_padStarted) PadStart();
    if (!st) return ERROR_BAD_ARGUMENTS;
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;       // one merged pad, player 1
    g_padPolled = GetTickCount();
    XIGamepad g;
    if (!PadRead(g)) return ERROR_DEVICE_NOT_CONNECTED;
    // real use, not stick drift: a button, a trigger, or a stick pushed a quarter of the way
    if (g.wButtons || g.bLeftTrigger > 40 || g.bRightTrigger > 40 ||
        abs(g.sThumbLX) > 8000 || abs(g.sThumbLY) > 8000 || abs(g.sThumbRX) > 8000 || abs(g.sThumbRY) > 8000)
        g_padTick = GetTickCount();
    if (!g_xiEnabled || g_menuOpen) g = {};
    if (memcmp(&g, &g_padLast, sizeof g)) { g_padLast = g; g_padPacket++; }
    st->dwPacketNumber = g_padPacket;
    st->Gamepad = g;
    return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI APR_XInputSetState(DWORD idx, XIVibration* v)
{
    if (!g_padStarted) PadStart();
    if (!v) return ERROR_BAD_ARGUMENTS;
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;
    const float k = g_xiEnabled ? g_padCfg.rumble / 100.0f : 0.0f;
    XIVibration s = { (WORD)std::min(v->wLeftMotorSpeed * k, 65535.0f), (WORD)std::min(v->wRightMotorSpeed * k, 65535.0f) };
    bool any = false;
    if (g_xiSet) for (DWORD i = 0; i < 4; i++) if (g_xiConn[i]) { g_xiSet(i, &s); any = true; }
    for (auto& d : g_sony) if (d.alive) {
        const LONG L = s.wLeftMotorSpeed >> 8, R = s.wRightMotorSpeed >> 8;
        const bool cl = InterlockedExchange(&d.wantL, L) != L, cr = InterlockedExchange(&d.wantR, R) != R;
        if (cl || cr) SetEvent(d.wake);
        any = true;
    }
    return any || g_diN ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

struct XICaps { BYTE Type, SubType; WORD Flags; XIGamepad Gamepad; XIVibration Vibration; };
extern "C" DWORD WINAPI APR_XInputGetCapabilities(DWORD idx, DWORD, XICaps* c)
{
    if (!g_padStarted) PadStart();
    XIGamepad g;
    if (!c) return ERROR_BAD_ARGUMENTS;
    if (idx != 0 || !PadRead(g)) return ERROR_DEVICE_NOT_CONNECTED;
    memset(c, 0, sizeof *c);
    c->Type = 1; c->SubType = 1;                              // gamepad
    c->Gamepad = { 0xF3FF, 255, 255, (SHORT)0xFFC0, (SHORT)0xFFC0, (SHORT)0xFFC0, (SHORT)0xFFC0 };
    c->Vibration = { 0xFFFF, 0xFFFF };
    return ERROR_SUCCESS;
}
extern "C" void WINAPI APR_XInputEnable(BOOL on)
{
    g_xiEnabled = on ? 1 : 0;
    if (!on) { XIVibration z = {}; APR_XInputSetState(0, &z); }
}
extern "C" DWORD WINAPI APR_XInputGetDSoundAudioDeviceGuids(DWORD idx, GUID* r, GUID* c)
{
    if (r) memset(r, 0, sizeof *r);
    if (c) memset(c, 0, sizeof *c);
    XIGamepad g;
    return idx == 0 && PadRead(g) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI APR_XInputGetBatteryInformation(DWORD idx, BYTE, BYTE* info)
{
    XIGamepad g;
    if (idx != 0 || !PadRead(g)) return ERROR_DEVICE_NOT_CONNECTED;
    if (info) { info[0] = 1; info[1] = 3; }                   // wired, full
    return ERROR_SUCCESS;
}
extern "C" DWORD WINAPI APR_XInputGetKeystroke(DWORD, DWORD, void*)
{
    return 4306;                                              // ERROR_EMPTY
}

// for the menu: what's connected right now
static void PadDescribe()
{
    g_padInfoN = 0;
    auto add = [](const char* fmt, auto... a) { if (g_padInfoN < 8) snprintf(g_padInfo[g_padInfoN++], sizeof g_padInfo[0], fmt, a...); };
    for (int i = 0; i < 4; i++) if (g_xiConn[i]) add("XInput pad %d (Xbox or a virtual pad)", i + 1);
    AcquireSRWLockShared(&g_padLock);
    const DWORD now = GetTickCount();
    for (auto& d : g_sony) if (d.alive)
        add("%s over %s%s", d.kind == 1 ? "DualShock 4" : "DualSense", d.bt ? "Bluetooth" : "USB", d.tick && now - d.tick < 1500 ? "" : " (no reports yet)");
    for (int i = 0; i < g_diN; i++) add("%s (DirectInput %04lX:%04lX)", g_diDev[i].name, g_diDev[i].vidpid >> 16, g_diDev[i].vidpid & 0xFFFF);
    ReleaseSRWLockShared(&g_padLock);
}

// ---------------------------------------------------------------- automatic gamepad <-> keyboard/mouse
// The game's "Enable gamepad" option (APProfileManager.ProfileSettings.bUsingXboxController, bit 0 at
// +0x13C of the profile manager, global pointer 0x16F9A00 in the Steam/GOG exe) turns keyboard and mouse
// off. The game re-reads it every tick (0xAC7B48 / 0xAC9038: "using gamepad" = pad connected && bit),
// so flipping the bit to whatever was touched last switches modes on the fly, prompts included.
static int g_autoSwOk = -1;           // -1 unchecked, 0 exe doesn't match, 1 ok
static void PadAutoSwitchTick()
{
    if (!g_padCfg.autoSwitch || g_menuOpen || !g_padStarted) return;
    if (g_autoSwOk < 0) {
        static const BYTE a[] = { 0xA1, 0x00, 0x9A, 0x6F, 0x01 };                 // mov eax, [0x16F9A00]
        static const BYTE b[] = { 0xF6, 0x80, 0x3C, 0x01, 0x00, 0x00, 0x01 };     // test byte [eax+0x13C], 1
        MEMORY_BASIC_INFORMATION mbi = {};
        const bool mapped = VirtualQuery((const void*)0xAC7B48, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                            (BYTE*)mbi.AllocationBase == (BYTE*)GetModuleHandleA(nullptr);
        g_autoSwOk = mapped && !memcmp((const void*)0xAC7B48, a, 5) && !memcmp((const void*)0xAC7B51, b, 7) &&
                     !memcmp((const void*)0xAC902F, a, 5) && !memcmp((const void*)0xAC9038, b, 7);
        Log(g_autoSwOk ? "pad: automatic gamepad / keyboard switching ready" :
                         "pad: unknown APGame.exe - automatic gamepad / keyboard switching off");
    }
    if (!g_autoSwOk) return;
    // in keyboard mode the game may stop polling the pad: look ourselves so a button press switches back
    if (GetTickCount() - g_padPolled > 250) { XIState tmp; APR_XInputGetState(0, &tmp); }
    // mouse movement counts once it's more than a nudge within ~a frame
    LONG mv = InterlockedExchange(&g_mouseMoved, 0);
    // in gamepad mode the game may stop reading the mouse: then watch the cursor ourselves
    static POINT lastCur = {}; static bool haveCur = false;
    POINT cur;
    if (GetTickCount() - g_diMouseTick > 250 && GameHasFocus() && GetCursorPos(&cur)) {
        if (haveCur) mv += labs(cur.x - lastCur.x) + labs(cur.y - lastCur.y);
        lastCur = cur; haveCur = true;
    } else haveCur = false;
    if (mv > 6) g_kbmTick = GetTickCount();
    const DWORD pad = g_padTick, kbm = g_kbmTick;
    if (!pad && !kbm) return;
    const bool wantPad = (LONG)(pad - kbm) > 0;
    BYTE* pm = *(BYTE**)0x16F9A00;
    if (!pm) return;
    BYTE& bits = pm[0x13C];
    const bool isPad = bits & 1;
    if (wantPad == isPad) return;
    if (wantPad) bits |= 1; else bits &= ~1;
    Log("pad: switched the game to %s", wantPad ? "gamepad" : "keyboard + mouse");
}
