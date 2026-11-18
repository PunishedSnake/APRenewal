# APRenewal — working notes

`d3d9.dll` proxy mod for **Alpha Protocol** (2010, UE3, D3D9 SM3, 32-bit `APGame.exe`).
Code: `src/main.cpp` (everything), `src/pad.h` (controllers), `src/smaa.h`, generated `src/matclass.h` /
`src/specregs.h`, `src/xinput/` (xinput1_3.dll forwarder). Dear ImGui 1.91.9 vendored in `src/imgui/`.
README = current feature list only (no changelog). These notes = current knowledge, not history.

## Build, test, release
- `./build.sh` → `out/d3d9.dll` + `out/xinput1_3.dll` (i686-w64-mingw32, static, exports via `src/d3d9.def` + `--kill-at`).
- No way to run the game here, and Wine in this sandbox crashes on any exe: the user tests. Every feature
  logs to `Binaries\APRenewal\APRenewal.log` with a prefix (`postfx:`, `ssao:`, `ssr:`, `depth:`, `hdr:`,
  `ssaa:`, `audio:`, `menu:`, `pad:`, `compat:`, `census:`, `crash?:`) — always add logs.
- No offline HLSL compiler works here: shader compile errors only show in the user's log.
- `tools/proto/` numpy replicas of the AO shader on captured fp16 depth — validate shader math there.
- Release: bump `"APRenewal X.Y.Z"` (4× in main.cpp incl. the glass lock string, ini header), zip
  `APRenewal-X.Y.Z/` with `Binaries/` (d3d9.dll, xinput1_3.dll, APRenewal.ini, OpenAL32.dll), `source/`
  (git archive), README. Commit as PunishedSnake, push with `git -c push.negotiate=true push`.
- Default branch `main`; work on a branch, merge by PR (REST API — gh GraphQL is blocked here).

## HLSL rules (both compilers)
- `[Shaders] Compiler=auto`: System32 d3dcompiler_47 D3DCompile (flag 1<<12 backwards compat) unless Wine;
  falls back to the game's d3dx9_42 (2009). One critical section around every compile.
- No bitwise ops (ps_3_0). Never reuse a for-loop variable name later in a function (legacy scoping).
  `near`/`far` are reserved. The 2009 compiler crashed on loops with `break` + early returns in helpers
  (game-thread compiles): keep them branch-free/unrolled.

## Exe addresses (the user's GOG APGame.exe — verify bytes before every patch; Steam exe coming)
- Mid-hooks: `MidHook(site, len, expect, fn, repl)` (pushfd/pushad/XMM save, fn may edit regs, original or
  replacement bytes, jmp back). Sites = whole instructions without relative operands.
- View (replaces AlphaFixer): FOV blend function 0xA8F020, per-module FOV via replacement
  `mulss xmm0,[g_fovScratch]`, blended result read at its `fld [esp+8]` (feeds AO FOV). Normal camera
  FOV = 60°. HUD canvas sig `89 53 70 8B 15 ? ? ? ? 89 4B 64 89 43 68 8B 52 50` (+9: ecx width, esi left),
  menu viewport sig `FF D2 85 C0 89 44 24 08 DB 44 24 08` (+2: eax width, first of 3). HUD/menu only act
  wider than 16:9.
- Movies: player vtable slot 0x12173B0 = GameThreadPlayMovie 0x4E49F0 (thiscall mode, name, startFrame;
  ret 0xC). Logos `slate_*.sfd` skipped by name prefix; loading movie `simple_loading_v02.sfd` (mode 129).
- Console: player controller vtable +0x110 = ConsoleCommand(FString* ret, const FString&, UBOOL) thiscall.
  PC pointer from MidHooks on AAPPlayerController-only exec thunks (ShouldAllowPauseMenu 0xA50230,
  ShouldAllowPDAMenu 0xA50270, IsPlayerPastTitleScreen 0xA50490, UpdateAIData 0xA50970; not InPDAMenu).
  Used only if seen in the last 3 s with the first-seen vtable; commands queued, run in hk_PeekMessageW
  on the main thread; result FString freed with GMalloc (0x16E8030) vtable +0xC. Viewport Exec
  (0x50E000–0x513800) has AP commands (HQDLE, USEMAGICRIM, TOGGLEUI, TOGGLEOCCLUSION 0x16F95C4, EDAA,
  MINAMBIENT, LIGHTSCALE, SHADOWINTENSITY, SHARPNESS, …) → docs/script-commands.md.
- Gamepad option: UAPProfileManager singleton [0x16F9A00] (cf. execGetProfileManager 0xAB3E40), bit 0 of
  +0x13C = "Enable gamepad". Re-read every frame by 0xAC7B00 / 0xAC8DC0: pad connected (0xA274F0) && bit
  → clear flag 0x800 at input object +0x1FC (gamepad), else set (KBM). Sites 0xAC7B48/51, 0xAC902F/38.
- `docs/apgame-re.md`, `tools/re/apgame_natives.json` (2804 `Class::execFunc` → address),
  `tools/re/apdis.py` (capstone disassembly with native names; `APGAME=path`).
- Script packages: UE3 v539 / licensee 91, zlib-chunked: `tools/re/upk.py`, `execs.py`, `sigs.py`.

## Hooks
- Proxy exports Direct3DCreate9/Ex, D3DPERF_*, APR_XInput*. Chain-loads `[Chain] RealD3D9` (user: DXVK as
  `d3d9_dxvk.dll`); under Wine empty = Proton's DXVK (`LoadReal` refuses our own module).
- IAT (APGame.exe): D3DXCompileShader, QPC, GetTickCount, PeekMessageW (focus pause parks the main thread
  here — never freeze the clock globally, UE3's limiter spins on it; never park while closing: g_exiting),
  DirectInput8Create, xinput1_3 ordinals 2/3, GetProcAddress (OpenAL), GetCommandLineW + msvcr80
  GetCommandLineA/_acmdln, GetClientRect / ScreenToClient / ClientToScreen (SSAA), GetCursorPos /
  SetCursorPos (frozen while the menu is open), MessageBoxW/A (logs UE3 error boxes).
- D3D vtables: IDirect3D9 CreateDevice 16, GetAdapterModeCount 6 / EnumAdapterModes 7. Device: Reset 16,
  Present 17, SetGammaRamp 21, CreateTexture 23, CreateDepthStencilSurface 29, StretchRect 34, SetRT 37,
  Clear 43, SetViewport 47/48, SetTexture 65, SetSamplerState 69, SetScissorRect 75/76, DP/DIP/DPUP/DIPUP
  81-84, CreateVertexShader 91, SetVS 92, SetVSConstantF 94, CreatePixelShader 106, SetPS 107,
  SetPSConstantF 109, CreateQuery 118 (→ query GetData 7).
- DirectInput: pad.h's scan thread shares dinput8's vtables, so hk_DICreateDevice ignores its thread
  (g_padThreadId) and only the main thread may replace the game's mouse/keyboard (the scan used to steal
  g_diMouse every 2 s → menu didn't blank the real mouse → camera moved with the menu open).
- Menu input blocking: WndProc subclass, DI mouse/keyboard GetDeviceState/GetDeviceData blanked (mouse
  buffer drained), XInput zeroed. Menu close logs what the game read while open (`MenuInputReport`).

## UE3 frame structure
1. Occlusion queries; 2. depth prepass (full-res LDR), HDR fp16 base pass (alpha = linear view depth);
3. lights, shadow map (2048 R32F), translucency; depth+stencil cleared before bloom; 4. bloom at ~1/4
res, world into full-res LDR target, one draw to the backbuffer, then UI.
- Variants: no-bloom frames, scene-capture frames (1024² targets first), UI-pass frames (armory).
- `BeforeHudOnSetRT` is order-aware (base pass before bloom; small RT ≤ bbW/3 = bloom; world RT = first
  full-res backbuffer-format target after bloom; new scene on a ≥80-draw full-res LDR prepass), compared
  against the game's logical backbuffer format.

## Shader patching
- CreatePixelShader → `ParseCtab` (ps 0xFFFF and vs 0xFFFE tokens) → classify by CTAB names or FNV-1a
  bytecode hash. SetPS swaps in replacements; constants mirrored into `g_psc`/`g_vsc`; `PreDraw` feeds ours
  (`PdSet` = write + restore after the draw), `PostDrawRestore` puts the game's back.
- Generated replacements: header with every CTAB constant at its original register + our body; rejected
  unless our CTAB ⊆ the game's. Our constants (`apTM`…`apTM4`, `apX[12]`) go above the game's
  (c ≥ paramReg, s ≥ 8). Originals AddRef'd. g_inPostFX work is ignored by all hooks.
- UE3 packs matrices ROW-MAJOR: register i = row i, mul(v,M) = Σ v_i·row_i.
- Final blend (AP_BloomBlend / DOFAndBloomBlend): scene + bloom(RGBE ×4) → apApplySaturation (its
  (c − SceneShadowsAndDesaturation) clips HDR < ~0.006 to 0) → (overlay + c)·scale → saturate → lerp(x,
  √x, GammaInverseOn) → brightness, contrast, colour filter. Ours adds roll-off, gamma choice, shadow toe,
  bloom/DOF tent smoothing, wide bloom (s10), bokeh.
- Shadow projection: kShadowBodyHLSL (Vogel disc, HW compare via tex2Dproj when the texture is a depth
  format, CONTACT variant with Inverse4(ScreenToShadowMatrix)). Shadow map scale fixed at device creation
  (g_shScaleRun); viewport/scissor/clear scaled while RT0 is the big map.
- Glass (hashes cb2f41c2, 23836016, door glass cc79d4cd; refraction 3e0261ff / 98160836 / 855d48c7):
  COLOR0 TangentX, COLOR1 TangentZ, TEXCOORD6 camera vector (tangent space); Texture2D_0 normal,
  _1 mask (.x reflection, .w opacity), _2 diffuse, cube env. Writes COLOR1 = 1 into a glass mask (AO
  behind glass), glass SSR variant (`#if GLASSSSR`, unrolled, built after the plain one, skipped if
  `APRenewal_glass_compile.lock` survives a crash).
  Glass SSR colour source = this frame's HDR scene StretchRect'd (half res, `g_glassSrc`) at the first glass
  draw of each scene key (RT0 == g_hdrSceneSurf), apX[4..7] = current VP; fallback g_prevHdr + g_vpPrev.
  With the same-frame source apX[11].x = 1 and apX[4..6] = inverse rows (A), apX[7] = camera: back-face
  test at the hit (normal from 3 depth taps, oriented to the camera; ray must hit the front: −n·R > 0) —
  rays from a window back toward Mike land on his back from behind (the "dark Mike ghost"). Rays almost
  straight back to the camera faded too. Jitter IGN(pixel + frame) so TAA averages it; 5-tap cross on the hit.
- Fog OneLayerMain rebuilt to write transmittance to RT1 (AO fades with fog).
- Material mask: `BuildTaggedShader` patches base-pass bytecode (`def cK`, `mov oC1, cK`) for skin / metal /
  rough hashes (src/matclass.h from tools/re/matclass.py over RefShaderCache); RT1 = g_matMask (r skin,
  g metal, b rough) when RT0 is the full-res HDR scene.
- Gloss: src/specregs.h (tools/re/specregs.py) = per skin/metal light shader the registers of the specular
  pow exponent (base from max(), not Fresnel's 1−x); scaled per draw. Skin light 54793f09: spec exp c40
  (=7), Fresnel pow 12. Weapon light 2198188e: exponent lerp(US42, US41) 3–10, ramp map, Fresnel US0/US1/US2.
- Character lighting: SH coefficients (c_SHCoefficients_*) and sky colours scaled per draw.
- RefShaderCache-PC-D3D-SM3.upk layout and tools: `tools/re/shadercache.py` (material → shader hashes).
- Census (Diagnostics): next frame's PS list with hashes, draws, CTAB names and UniformScalar/Vector values.

## PostFX chain (`PostFXRun`)
AO → SSS (2 passes) → SSR → light shafts → grade → SMAA/FXAA → TAA → sharpen. **Every pass that calls
`nextOut` must be counted in `remaining`** (missing ones silently drop later passes).
- Depth: game D24S8 recreated as INTZ, snapshotted to R32F after the base pass (`ZSnapshot`), calibrated
  against HDR-alpha depth (1/d = A + B·z; d = 10/(1−z)). Normals: orient with dot(n, p).
- World position from depth (SSR, TAA, shafts): wp = ((sx·w, sy·w, w) − t)·inv(M3), M3 rows (r_i.x, r_i.y,
  r_i.w), t = (r3.x, r3.y, r3.w), VP captured from the vertex shaders while the RT is the HDR scene.
- SSS: separable 13-tap, radius 0.2 × width × focal / depth (cap 12 px), 65 % luminance restored (the user
  rejected blurry faces). TAA history reduced on skin.
- SSR: neighbour-picked normals, tMin = 2 + 1 % depth, hits within 3 px rejected, disc blur by roughness.
- Light shafts: bright pass (excludes skin) → per-cell max with uv + depth → read back a frame late →
  sources unprojected and tracked in world space, re-projected every frame → per light: 64-tap radial blur
  + 8-tap fill across one tap spacing, added up → screen-blend composite.
- Grade: linear light; contrast S-curve around mid grey; black level = soft toe c²/(c+b)·(1+b), never a
  clip (`[Grade] BlackLevel2`, 1/10000). The game is dark — keep defaults mild.
- SSAA: game renders at scale × desktop via `-ResX/-ResY` (through `_acmdln`), window kept at monitor size,
  Mitchell downscale at Present (swap effect COPY).

## HDR (DXVK)
- `UnlockAdditionalFormats` → fp16 backbuffer, `SetColorSpace(scRGB)`; highlights rebuilt from the HDR
  scene buffer before the UI; encode at Present. HDR10 + fp16 froze presentation.
- On the user's Windows PC the DXVK path is SDR-limited (highlight test patches all equal) → HDR off by
  default, gated by HdrAllowed() = Wine || `[HDR] AllowWindows`. DXVK v3.1.1 needs commit 3c40ca1
  (swapchain colorspace). Only remaining native route: own DXGI/D3D11 swapchain via interop (not started).

## Controllers (src/pad.h, `pad:`)
- Game imports xinput1_3 ordinals 2 (GetState) and 3 (SetState) and dinput8 (mouse only). Our
  xinput1_3.dll forwards to d3d9.APR_XInput*; otherwise the IAT is redirected. GOG dixi (dinput8.dll
  exporting XInputGetState) present → we stay out of the way.
- One merged pad on slot 0: system XInput (xinput1_4 → 9_1_0 → 1_3 by System32 path, ordinal 100 for
  Guide), Sony HID thread per device (DS4 05C4/09CC/0BA0, DualSense 0CE6/0DF2; BT = input report length
  ≠ 64; feature 0x02/0x05 enables full BT reports 0x11/0x31; output 0x05/0x11 DS4, 0x02/0x31 DS5, BT CRC32
  seeded 0xA2), DirectInput pads polled every 4 ms with maps (kDiKnown, generic D-mode vs Xbox-style,
  ini `Map_VVVVPPPP`); XInput (`IG_`) and Sony devices skipped in DI. Buttons OR, triggers max, sticks
  by magnitude.
- Auto switch (`PadAutoSwitchTick`, main thread): pad = button / trigger > 40 / stick > 8000; KBM = key
  down (no repeat), mouse buttons/wheel, DI mouse motion, or cursor motion when DI is idle. Confirmed working.

## Audio
- OpenAL loaded at runtime (`DeviceName=Generic Software`); with OpenAL Soft 1.25.2 legacy names map to the
  default device, `ALC_HRTF_SOFT` + stereo HRTF output, `alcResetDeviceSOFT` toggles live.

## Proton / Steam Deck (untested)
- `DetectCompat()`: `wine_get_version`, `SteamDeck=1`. Launch `WINEDLLOVERRIDES="d3d9,openal32=n,b" %command%`.
  Windows-only bits (DisplayConfig SDR white, DXGI diag) fail soft.

## The user's setup
- RTX 4080 SUPER + AMD iGPU, 2560×1440, 400-nit HDR monitor, headphones on a Burson DAC.
- DXVK as RealD3D9, OpenAL Soft (Creative router kept as backup), MulderConfig launcher (edits APEngine.ini;
  its "higher-res textures" option breaks textures — not our bug).
- Pads: DualSense (USB, sometimes BT) and Xbox Series X; wants generic support, not tailored.
- AlphaFixer (winmm.dll) and dixi are replaced; user told to delete them.

## Open items
- Steam APGame.exe: user will supply it → make exe patches version-agnostic (signature scans instead of
  fixed addresses).
- HUD/subtitle scale (Scaleform canvas RE), 120 fps physics check.
- Menu + mouse: fix for the stolen g_diMouse in 0.21.1 — confirm with the user.
