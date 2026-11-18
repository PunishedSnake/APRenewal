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
  gets lost after DXVK. NVIDIA "layered on DXGI" changed nothing. Measured shape = our linear output
  decoded as sRGB again at ~SDR white; 0.9.6 adds CompensateSRGB (pre-encode relative to paper white).
  With Compensate the pattern matches the SDR reference but the game looks like plain SDR (user).
  0.9.7 adds a "Highlight test" row (1x-4x paper white) to tell whether anything above paper white
  survives this path; all patches equal = the path is SDR-limited, HDR has no gain → advise HDR off.
  RESULT (user, 0.9.7): all six patches identical with and without Compensate → native HDR via
  DXVK's Vulkan swapchain is SDR-limited on this PC. HDR stays default off; user pointed to NVIDIA
  RTX HDR instead. Only remaining native route: our own DXGI/D3D11 fp16 swapchain via interop
  (Special K-style) — big job, not started.
  0.9.8: HDR gated by HdrAllowed() = Wine || [HDR] AllowWindows. Under Proton/gamescope the scRGB
  path should be composited properly (needs DXVK_HDR=1) — untested.
  Quit fix confirmed by the user (log ends with shutdown).
- Focus pause must never park the thread while the window is closing/hidden (0.9.5: g_exiting) —
  that kept the process alive after quitting.
- **Audio**: game loads OpenAL at runtime (`DeviceName=Generic Software` in APEngine.ini). With OpenAL
  Soft we map legacy names to the default device and request `ALC_HRTF_SOFT` +
  `ALC_OUTPUT_MODE_SOFT=ALC_STEREO_HRTF_SOFT`; `alcResetDeviceSOFT` toggles it live.

## Proton / Steam Deck (0.9.8, untested)
- `DetectCompat()`: ntdll `wine_get_version` → `g_wineVer`, env `SteamDeck=1` → `g_steamDeck`; logs `compat:`.
- Launch options `WINEDLLOVERRIDES="d3d9,openal32=n,b" %command%`, `RealD3D9` empty → system d3d9 = Proton's
  DXVK. `LoadReal` refuses if the system path resolves to our own module.
- Windows-only bits (DisplayConfig SDR white, DXGI output diag) fail soft under Wine (paper white falls back to 200).

## Game module (0.10.0, Steam exe addresses, all verified by bytes before patching)
- Intro skip: movie player vtable slot 0x12173B0 = GameThreadPlayMovie 0x4E49F0 (thiscall, mode,
  name, startFrame; ret 0xC). Refused (return 0) until `g_worldEverSeen` (first HDR base pass in
  BeforeHudOnSetRT) — not enough: the logos `slate_sega/obsidian/ap.sfd` (mode 0) start just after the
  menu's first 3D frame, so 0.10.1 also skips by name prefix ([Game] SkipMovies=slate_). Loading movie =
  `simple_loading_v02.sfd` (mode 129). User confirmed console + 4x shadows work (0.10.0). `-nomovie` would install the null player and kill in-game .sfd movies too.
- Shadows: the shadow map is a square R32F RT texture (2048) + square depth buffer. With
  [Shadows] Scale they're created k× larger (CreateTexture 23 / CreateDS 29); while RT0 is the big
  shadow map, SetViewport 47 / GetViewport 48 / SetScissorRect 75 / GetScissorRect 76 / Clear rects
  are scaled. Shadow UVs are normalised. Unknown: whether the PCF shader uses a texel-size constant
  (filter would then stay 2048-sized — still fine). User's MaxShadowResolution=4096 had no effect.
- Console: vtable +0x110 on the player controller = ConsoleCommand(FString* ret, const FString& cmd,
  UBOOL log) (thiscall). PC pointer captured by MidHooks on AAPPlayerController-only exec thunks
  (ShouldAllowPauseMenu 0xA50230, ShouldAllowPDAMenu 0xA50270, IsPlayerPastTitleScreen 0xA50490,
  UpdateAIData 0xA50970; InPDAMenu is shared with AAPAIController — don't use). Pointer used only if
  seen in the last 3 s with the first-seen vtable. Commands run in hk_PeekMessageW on the main
  thread. Result FString freed with GMalloc (0x16E8030) vtable +0xC.
- dis.py must run with `python3 -P` (its name shadows the stdlib `dis` module).

## Shader patching (0.11.0)
- CreatePixelShader 106 → `ParseCtab` (constant names/registers) → classify. SetPixelShader 107 swaps
  in our replacement; SetPixelShaderConstantF 109 is mirrored into `g_psc`; `PreDraw` (all 4 draw
  hooks) feeds our values, `PostDrawRestore` (AfterDraw) puts the game's back.
- Replacements are generated: a header with every CTAB constant at its original register, then our
  body; compiled with D3DXCompileShader at runtime and rejected unless our CTAB is a subset of the
  game's (same set/register). Our own extra constant `apTM` goes to max used c-register + 1.
  Original shaders are AddRef'd so their pointers are never reused.
- Final blend (HSBContrast + GammaColorScaleAndInverse + LensFilterImage + FilterColor): plain
  (AP_BloomBlendPixelShader) and DOF (DOFAndBloomBlendPixelShader Main: NewDOF maps, PackedParameters,
  vignette, c_finalColorScaleLow). The Epic variant without LensFilterImage is not touched.
  Original order: scene + bloom(RGBE, ×4) → apApplySaturation → (overlay + c)·scale → saturate →
  lerp(x, sqrt x, GammaInverseOn) → brightness, PS contrast, x² contrast, colour filter.
- Shadow projection (ScreenToShadowMatrix + ShadowDepthTexture): SampleOffsets / RefiningSampleOffsets /
  EdgeSampleOffsets × f, ShadowBufferSize × Scale; f = 1/Scale … 1 by [Shadows] Softness.
- No offline HLSL compiler works here (Wine segfaults, apt's vkd3d 1.2 has no HLSL): replacement
  compile errors only show in the user's log (`postfx: final blend compile failed`).
- 0.12.0 effects: final pass params apTM (roll-off), apTM2 (sRGB / 2.2 weights, bloom intensity, DOF
  strength), apTM3 (quarter-res texel size = 1/(pp/4+2), bloom / DOF tent smoothing) at paramReg..+2.
  Constant overrides in PreDraw (PdSet = write + restore after draw): fog FogDistanceScale × density;
  motion blur (AP MainPixelShader_DirectionalEDAA: VelocityBuffer + PrevViewProjMatrix) Static/Dynamic
  VelocityParameters.xy × strength, apOffsetAndWeightTable[0].w = edge-AA weight, apUnsharpMaskAmount;
  bloom gather (BloomThreshold + SceneColorTexture only) threshold += offset, .y = 1/(4−t).
  The AP "motion blur" pass is also the game's EDAA (directional edge blur) + unsharp mask.
- Menu: Video (sub-tabs Image/Colour/Effects/Shadows & AO/View/HDR), Audio, Game, Diagnostics, Miscellaneous.
- User (0.11.0 log): both final blends + shadows recognised and used; logged shadow offsets ×1.000
  (Softness at 100) → "difference not noticeable".
- 0.13.0: soft shadows = generated replacement of the shadow projection (kShadowBodyHLSL: depth from
  scene alpha, ScreenToShadowMatrix as float4[4] column-major → dot per register, Vogel disc NTAPS
  rotated by IGN(VPOS), HW = tex2Dproj compare, else tex2Dlod compare; MODSHADOW/ATTEN/SPOT/CUBE/
  CLAMPUV from the CTAB). Built lazily in PreDraw per [hw][quality]; HW decided by the bound texture's
  format; re-bound with o_SetPS before every draw. Beast area / non-mod branching keep the game's.
  Character lighting: material shaders with c_SHCoefficients_A*/B*/Cc (AP irradiance volumes, D3DX SH
  layout, base pass) and Upper/LowerSkyColor — constants scaled per draw (even = directional ×(1−e),
  fill = constant ×(1+f)). 2687 SH + 8786 sky shaders in RefShaderCache.
  Census: Diagnostics button → next frame's PS list (ptr, FNV hash of bytecode, draws, CTAB names).
  Glass: material CTABs only have generic names (UniformVector_N, Texture2D_N) — needs census + hash.
- 0.14.0: UE3 compiles shaders with ROW-MAJOR matrix packing (disassembly: mad chain x·c18 + y·c19 +
  z·c20 + c21) — register i = row i, mul(v,M) = Σ v_i·row_i. 0.13.0's dot() version put shadows in
  the wrong place (user: "filters make it disappear"). Shadow scale used at runtime = g_shScaleRun
  (fixed at device creation); 0.13.0 used live cfg → 1x/4x in the menu broke the viewport scaling.
  CtabConst.type from the CTAB type info (14 = samplerCUBE) for generated headers.
  Glass: census → hashes (FNV-1a of the bytecode, ShaderHash) cb2f41c2 (glass) / 23836016 (glass with
  Fresnel) in RefShaderCache, unique shapes = the game's glass materials. Disassembled with
  /tmp/apwork/dis9.py-style decoder: COLOR0 = TangentX (packed, w = sign), COLOR1 = TangentZ,
  TEXCOORD0 uv, TEXCOORD4 fog (rgb add, w mul), TEXCOORD6 camera vector (tangent space); Texture2D_0
  normal (xy × UniformScalar_1), Texture2D_1 mask (.x reflection, .w opacity), Texture2D_2 diffuse,
  TextureCube_0 env; refl = env·lum(env) (crushes dim reflections). Distortion accumulate shaders
  3e0261ff / 98160836: refraction scale UniformScalar_1 (c0).
  User: character lighting sliders work but can't fix the probe direction — accepted, not pursued.
  User (0.14.0): glass tweaks great; soft shadows work perfectly (also very soft).
- 0.14.1 AO behind glass: glass replacement writes COLOR1 = 1; PreDraw binds g_glassMaskSurf (same
  size/format as RT0, cleared once per frame via ColorFill, g_frameNo from TraceFrameBoundary) as RT1
  with o_SetRT, PostDrawRestore unbinds it. AO composite samples it at s2: ao = lerp(ao, 1, mask·c4.z).
  0.14.2: mask keyed by frame×64 + g_scenesThisFrame (scene-capture glass leaked into the main view as
  a light trapezoid without AO), only bound when RT0 = g_ppW×g_ppH, mask alpha = saturate(alpha·4).
- 0.15.0:
  - Fog: one-layer height fog (HeightFogPixelShader OneLayerMain) rebuilt (kFogBodyHLSL) to write its
    transmittance to RT1 (g_fogMask, cleared to 1 per scene; blend ONE/SRCALPHA → mask written as
    (0,0,0,sc), otherwise (sc,sc,sc,1)). AO composite s3: ao = lerp(1, ao, lerp(1, fogT, c4.w)).
  - Door glass cc79d4cd: two normal maps (Texture2D_0 × US1 + Texture2D_1 × US3, z summed), mask
    Texture2D_2, diffuse Texture2D_3, cube s4 (DUALNORMAL in kGlassBodyHLSL); refraction 855d48c7 →
    UniformScalar_5 is its overall scale.
  - Pop-in: occlusion queries' GetData (IDirect3DQuery9 vtable 7, patched on the first occlusion query
    from CreateQuery 118) reports ≥64 pixels. The game's own TOGGLEOCCLUSION flips 0x16F95C4.
  - Vertex shaders: CreateVertexShader 91 (CTAB ViewProjectionMatrix reg), SetVertexShader 92,
    SetVertexShaderConstantF 94 mirrored (g_vsc). PreDraw copies the VP (row-major, world→clip) while
    the RT is the HDR scene → g_vp/g_vpKey. SSR (kSSRPS): wp = ((sx·w, sy·w, w) − t)·inv(M3) with M3
    rows (r_i.x, r_i.y, r_i.w), t = (r3.x, r3.y, r3.w); normals from depth, world Z up.
  - Wide bloom: BuildModernBloom in the final blend's PreDraw (XPass chain, 6 levels, fp16), bound at
    s10 (apBloomTex), apTM4.x intensity. Our "ap*" constants/samplers may sit above the game's (c ≥
    paramReg, s ≥ 8). hk_SetPS / PreDraw / PostDrawRestore / constant mirrors ignore g_inPostFX work.
  - FXAA 3.11 quality (kFxaaQPS, [PostFX] FXAAQuality).
  - Console: commands queue (g_conQueue). The game's viewport Exec (0x50E000–0x513800) has AP
    commands: HQDLE (0x137A6BC, default 1), USEMAGICRIM (0x137A6A8, 1), TOGGLEUI (0x137ABD8),
    TOGGLEOCCLUSION, DOORLIGHTINGFIX, TOGGLEDOFFIX, EDAA/DEDAA/EDAAOFF, value commands MINAMBIENT,
    IRRADIANCESCALE/CONTRAST, LIGHTSCALE, STATICLIGHTSCALE, SHADOWINTENSITY(MIN), SHARPNESS,
    DESATURATE, VIGNETTEFADE*, MOTIONBLURAMOUNT/STRETCH, MAGICRIMSCALE/Z/ROTATION, SHADOWFALLOFFRANGE.
- 0.15.1: SSR failed to compile in 0.15.0 ("redefinition of 'k'": the legacy D3DX compiler scopes
  for-loop variables to the function — never reuse a loop variable's name later). `ssr:` log lines
  now report the VP capture and why SSR is off. SSR can't reach glass (translucent, no depth, drawn
  before our pass); glass reflections stay cubemap-only.
  Script packages: UE3 v539/licensee 91, zlib-chunked. `tools/re/upk.py` (tables + decompression),
  `execs.py` (FUNC_Exec 0x200; flags = u32 at end−12, or end−14 with FUNC_Net), `sigs.py` (params:
  CPF_Parm 0x80 in the u64 at +24 of a property export, children listed in reverse). Function
  export: bytecode size at +40, code at +44. Commands → docs/script-commands.md. Pawn/HUD/
  CheatManager execs are in the console's exec chain; CheatManager needs EnableCheats.
  User's Materials folder (MasterMATS, Material_Skin, Material_Cloth, Material_VehicleAndWeapon,
  PhysicalMATS, GenericMATS = Glass_0001/0002) has the material graphs (expressions) — the source
  for mapping Texture2D_N / UniformScalar_N of material shaders.
- 0.16.0:
  - VS CTABs were never parsed (ParseCtab demanded a ps version token 0xFFFF....; vs is 0xFFFE....),
    so ViewProjectionMatrix was never found and SSR never ran. Accept both.
  - RefShaderCache-PC-D3D-SM3.upk (uncompressed): shader records = [type FName 8][Id GUID 16][skip 4]
    [u8 platform][u8 freq][u32 size][bytecode]; shader maps end with [FName][MaterialId GUID][FString
    friendly name] and reference shaders as [type FName][GUID][type FName]. tools/re/shadercache.py:
    material_maps() → (material name, [(shader type, FNV hash)]) for 1972 maps / 1510 materials.
    tools/re/matclass.py → src/matclass.h (TBasePassPixelShader* hashes owned only by skin / metal
    materials; ~420 skin, ~480 metal). Census hash 9e6fdc78 = Mike's skin (Standard_Skin_MST_THOR).
  - Material mask: BuildTaggedShader patches the game's bytecode (def cK = tag after the header,
    mov oC1, cK before the end; K above CTAB + defs; skipped if the shader writes oC1 or uses relative
    constant addressing). hk_SetPS binds the tagged copy; PreDraw binds g_matMask (fp16, r skin, g metal)
    as RT1 when RT0 is the full-res HDR scene. Skin materials are MLM_Custom with their own layered
    scattering in the light passes - the mask comes from the base pass only.
  - Skin SSS (kSSSPS): separable 13-tap blur, per-channel sigma (1, .45, .25), radius = width x focal /
    depth (focal from |VP column y| x H/2), depth + mask continuity, approx linear (x²). After AO.
  - SSR (kSSRPS): c17 = (metal strength, mask valid), s2 = material mask; metal pixels reflect at any
    angle, tinted by their colour. Runs when floors or metal need it.
  - Glass SSR: BuildReplacement header adds apX[12] at paramReg+4 (if ≤ c223). PostFXRun copies the
    finished HDR scene to g_prevHdr (half res) + g_vpPrev; the glass PreDraw binds s12 = zcopy (24-bit z,
    1/d = A + B·z), s13 = g_prevHdr. Glass world position = camera − (TEXCOORD6 through the tangent basis
    and LocalToWorld): the VS writes camera−position projected on the (unnormalised) tangent basis.
  - Draw distance: "set PlayerController LODDistanceFactor 100/pct" queued whenever a new player
    controller is seen (UE3 scales cull/LOD distance by the view's LODDistanceFactor). Unverified that
    SET works in this shipping build — the console logs the reply.
- Next shader targets: bloom gather (box filter at 1/4 res, AP_BloomGatherPixelShader), DOF gather,
  height fog, motion blur; material shaders (RefShaderCache) are per-material and much harder.

## Reverse engineering
- `docs/apgame-re.md` has the exe findings; `tools/re/apgame_natives.json` maps 2804 UE3 natives
  (`Class::execFunc` → address, Steam exe); `tools/re/dis.py` disassembles with names. Start there.

## Known not-our-bugs
- MulderConfig "Allow higher-res world/weapon textures" breaks textures (see-through gun cabinet,
  blocky pants). Parked.

## Ideas queued
- Maybe absorb dixi (controller remapping) too.
- Shader bytecode replacement, better window glass, physics check at 120 fps.
