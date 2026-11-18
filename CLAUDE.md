# APRenewal — working notes

A `d3d9.dll` proxy mod for **Alpha Protocol** (2010, Unreal Engine 3, D3D9 SM3, 32-bit `APGame.exe`).
Everything lives in `src/main.cpp`; Dear ImGui 1.91.9 is vendored in `src/imgui/`.

## Build and check
- `./build.sh` → `out/d3d9.dll` (i686-w64-mingw32-g++, static, exports via `src/d3d9.def` + `--kill-at`).
- Shaders are HLSL raw strings (`static const char* kXxxPS = R"HLSL(...)HLSL";`) compiled at runtime
  with D3DXCompileShader. Offline check: `tools/shadercheck/shadercheck.py` (needs wine and the game's
  `d3dx9_42.dll` copied next to it). Wine's compiler rejects dynamic loops/branches with "not
  implemented" — that is a Wine limitation, not a shader bug. Prefer branch-free code where cheap.
- `tools/proto/` has numpy replicas of the AO shader (`gtao.py`) run on captured fp16 DDS depth —
  how the GTAO bugs were found. Validate shader math there before shipping.
- No way to run the game here: the user tests. Every feature logs to `Binaries\APRenewal.log`
  with a prefix (`postfx:`, `ssao:`, `depth:`, `hdr:`, `ssaa:`, `audio:`, `menu:`) — always add logs.
- Releases: zip with `Binaries/` (d3d9.dll, APRenewal.ini, OpenAL32.dll), `source/`, README.
  Bump the version string (`"APRenewal X.Y.Z"`, two places) and add a "New in" README section.

## The user's setup (stack order matters)
- `winmm.dll` = **AlphaFixer** — replaced by our View module in 0.9; user is told to delete it.
  It was an alphayellow1-style fix (spdlog/safetyhook, source not public). Reversed: three safetyhook
  mid-hooks — FOV (`F3 0F 59 80 0C 01 00 00 09 98 10 01 00 00`, wrote a fixed HFOV =
  2·atan(tan(37.5°)·FOVFactor·aspect/(16/9)) into every camera module's +0x10C), HUD canvas
  (`89 53 70 8B 15 ? ? ? ? 89 4B 64 89 43 68 8B 52 50` at +9: ecx = width, esi = left), menu viewport
  (`FF D2 85 C0 89 44 24 08 DB 44 24 08` at +2: eax = width; first of 3 matches). HUD width came from
  AlphaFixer.ini or the desktop — that broke supersampling.
- `dinput8.dll` / `xinput1_3.dll` = GOG "dixi" controller remapper (`dixi.ini`).
- MulderConfig launcher edits `APEngine.ini` etc. (`MulderConfig.json`, save file must list every group).
- `d3d9.dll` = APRenewal, chain-loads `RealD3D9=` (user runs DXVK as `d3d9_dxvk.dll`).
- `OpenAL32.dll` = OpenAL Soft 1.25.2 we built (original Creative router kept as backup).
- RTX 4080 SUPER + AMD iGPU, 2560x1440 desktop, 400-nit HDR display, headphones on a Burson DAC.

## Hooks (where things are)
- Code patches: `MidHook(site, len, expect, fn, repl)` — our own mid-function hook (pushfd/pushad/
  XMM save, `fn(HookRegs*)` may edit registers, then original or replacement bytes, jmp back).
  Sites must be whole instructions without relative operands; `expect` bytes are verified first.
  View module (`InstallViewHooks`): the game's normal camera module FOV is 60 deg (measured);
  `[View] FOV` is the target for that camera at 16:9 and all modules get the same tan ratio.
  FOV blend function at 0xA8F020 in the Steam exe — per-module
  FOV scaled via a replacement `mulss xmm0,[g_fovScratch]`, blended result read at its `fld [esp+8]`
  (feeds AO's FOV); HUD and menu hooks only act when wider than 16:9.
- Proxy exports Direct3DCreate9/Ex + D3DPERF_*. IAT hooks on APGame.exe: D3DXCompileShader, QPC,
  GetTickCount, PeekMessageW (focus pause parks the main thread here — never freeze the clock
  globally: UE3's frame limiter spins on it), DirectInput8Create, XInputGetState (ordinal 2),
  GetProcAddress (OpenAL), GetCommandLineW / GetClientRect / ScreenToClient / ClientToScreen (SSAA),
  plus msvcr80's GetCommandLineA/_acmdln (the game builds its command line from `_acmdln`).
- Vtables: IDirect3D9 CreateDevice 16, GetAdapterModeCount 6 / EnumAdapterModes 7 (SSAA fake mode);
  device Reset 16, Present 17, SetGammaRamp 21, CreateDepthStencilSurface 29, StretchRect 34,
  SetRenderTarget 37, Clear 43, DP/DIP/DPUP/DIPUP 81-84, SetPixelShader 107.

## UE3 frame structure (learned from traces: Ctrl+F11 trace, Ctrl+Shift+F11 image capture)
1. Occlusion queries on the backbuffer.
2. Depth prepass into a full-res LDR A8R8G8B8 target, then HDR fp16 base pass (fmt 113).
   HDR scene alpha = linear view depth (fp16 → too coarse for normals at grazing angles).
3. Lights, shadow map (2048 R32F), translucency; the game clears depth+stencil before bloom.
4. Bloom chain at ~1/4 res, finished world into the full-res LDR target, one draw copies it to
   the backbuffer, then UI on top.
- Variants: frames with no bloom (straight to backbuffer); scene-capture frames (a full scene,
  then 1024² targets, then the main scene); UI-pass frames (armory: world → HDR → widgets → bb).
- Before-HUD detection (`BeforeHudOnSetRT`) is order-aware: base pass before bloom, small RT ≤ bbW/3
  is bloom, world RT = first full-res backbuffer-format target after bloom; new-scene reset on a
  ≥80-draw full-res LDR prepass. Detection compares against the game's *logical* backbuffer format
  (HDR swaps the real one to fp16).

## Features and their gotchas
- **AO (GTAO, XeGTAO-style)**: 3 slices × 6 steps, depth-aware blur, highlight protection.
  Precise depth: the game's D24S8 is recreated as an INTZ texture, snapshotted to R32F right after the
  base pass (`ZSnapshot`), calibrated against HDR-alpha depth by least squares 1/d = A + B·z
  (fit: d = 10/(1−z), near plane 10, infinite far). Normal orientation must use dot(n, p), not n.z.
- **Colour grade**: linear light, S-curve contrast pivoting on mid grey (keeps 0 and 1), vibrance.
  Defaults are deliberately mild — this game is dark; stronger contrast crushes night scenes.
- **SSAA** (`RenderScale`): game renders at scale × desktop via `-ResX/-ResY`, window stays at monitor
  size (WM_WINDOWPOSCHANGING clamp, WM_SIZE + mouse lParam rewrite), Mitchell downscale at Present
  into the backbuffer corner, presented with a source rect (swap effect COPY).
  (AlphaFixer forced HUD/menu width to the desktop res — gone with the View module.)
- **HDR**: DXVK only. `ID3D9VkExtInterface::UnlockAdditionalFormats` → fp16 backbuffer;
  `ID3D9VkExtSwapchain::SetColorSpace(scRGB)`. Highlight expansion runs right after the game's
  world→backbuffer copy (before UI), from the HDR scene buffer with an auto-fitted exposure; encode
  at Present. DXVK **v3.1.1 never enables VK_EXT_swapchain_colorspace** (it sat in the device
  extension list; fixed upstream in 3c40ca1, 2026-09-28) and then silently clips HDR to an SDR
  swapchain → neon oversaturation. We check the loader ourselves (`CheckVulkanHdr`). Use a DXVK built
  with 3c40ca1 cherry-picked onto v3.1.1 until 3.2 ships (master's new present-timing code is the
  suspect for a broken Steam overlay). scRGB only: HDR10 colour space with the fp16 backbuffer froze
  presentation (game stuck after a few frames). Encode has a soft shoulder to the peak and a
  saturation trim; paper white near the peak = blown-out look. Since 0.9.2: paper white defaults to
  Windows' SDR white level (DisplayConfig SDR_WHITE_LEVEL), sRGB piecewise curve like Windows, expansion
  knee on luminance (max-channel knee boosted graded blue lights), and an A/B "Reference" toggle.
  User confirmed DXVK 3.1.1 + 3c40ca1 keeps the Steam overlay working; FOV 60 base confirmed. DXVK ignores gamma ramps in windowed
  mode; we still keep the game's ramp out of the HDR path.
  0.9.4 test pattern on the user's PC (monitor on the RTX, Windows HDR on, scRGB swapchain per DXVK log):
  output came out as if decoded as SDR sRGB at SDR white and clipped at 1.0 — the HDR colour space
  gets lost after DXVK. Prime suspect: NVIDIA Vulkan "present method" (native vs layered on DXGI).
- Focus pause must never park the thread while the window is closing/hidden (0.9.5: g_exiting) —
  that kept the process alive after quitting.
- **Audio**: game loads OpenAL at runtime (`DeviceName=Generic Software` in APEngine.ini). With OpenAL
  Soft we map legacy names to the default device and request `ALC_HRTF_SOFT` +
  `ALC_OUTPUT_MODE_SOFT=ALC_STEREO_HRTF_SOFT`; `alcResetDeviceSOFT` toggles it live.

## Reverse engineering
- `docs/apgame-re.md` has the exe findings; `tools/re/apgame_natives.json` maps 2804 UE3 natives
  (`Class::execFunc` → address, Steam exe); `tools/re/dis.py` disassembles with names. Start there.

## Known not-our-bugs
- MulderConfig "Allow higher-res world/weapon textures" breaks textures (see-through gun cabinet,
  blocky pants). Parked.

## Ideas queued
- Maybe absorb dixi (controller remapping) too.
- Shader bytecode replacement, better window glass, physics check at 120 fps.
