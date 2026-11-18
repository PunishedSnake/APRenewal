# APRenewal 0.10.1 — d3d9 proxy for Alpha Protocol (GOG 2023)

A research-and-fix base layer. It sits alongside the mods you already have:

| Slot | Who owns it |
|---|---|
| `winmm.dll` (AlphaFixer, not needed from 0.9) | AlphaProtocolFixer (FOV / HUD / widescreen) |
| `dinput8.dll`, `xinput1_3.dll` | GOG "dixi" input wrapper |
| `d3d9.dll` | **APRenewal** |
| `OpenAL32.dll` | **OpenAL Soft 1.25.2** (optional, replaces Creative's router; for HRTF) |

## Install
Copy `d3d9.dll` + `APRenewal.ini` into `Alpha Protocol\Binaries\`. Uninstall = delete both.
The log goes to `Binaries\APRenewal\APRenewal.log`.

## Steam Deck / Linux (Proton) — untested yet, please send logs
1. Copy `d3d9.dll`, `APRenewal.ini` and `OpenAL32.dll` into `Alpha Protocol/Binaries/`.
   Leave `RealD3D9` **empty**: Proton's own DXVK is used. Don't copy `winmm.dll`, `dinput8.dll` or
   `xinput1_3.dll` (Steam Input handles the controller).
2. Steam > Alpha Protocol > Properties > Launch options:
   `WINEDLLOVERRIDES="d3d9,openal32=n,b" %command%`
3. Check `APRenewal.log`: it should say `compat: running under Wine ...` and `system d3d9 ... -> <address>`.
- Performance on the Deck: keep `RenderScale=100`; if the frame rate drops, turn AO off first.
- HDR (Deck OLED with HDR on, or a Linux desktop running gamescope with HDR): set `[HDR] Enable=1` and
  add `DXVK_HDR=1` before `%command%`. Gamescope composites scRGB properly, so this is where the HDR
  path can actually show highlights. Check with the test pattern + "Highlight test".

## New in 0.10.1
- Intro skip fixed: the logo movies (`slate_sega`, `slate_obsidian`, `slate_ap`) start *after* the
  main menu's 3D scene, so 0.10.0 let them through. They are now skipped by name
  (`[Game] SkipMovies=slate_`); loading-screen and story movies still play.

## New in 0.10.0
New **Game** tab in the menu (Insert):
- **Skip intro logos** (`[Game] SkipIntros=1`, on by default). Movies that start before the first
  3D scene are refused, so the game goes straight to the main menu. In-game movies (news reports)
  still play. Each movie shows in the log as `movie: '<name>' skipped/playing`.
- **Shadow resolution** (`[Shadows] Scale=2`, applies at next launch). The game's shadow map is 2048²
  no matter what MaxShadowResolution says. APRenewal creates it 2x (4096²) or 4x (8192²) larger and
  scales every draw into it, so shadow edges get sharper. Log: `shadow:`. If shadows break
  (missing, shifted, striped), set Scale=1 and send the log.
- **Developer console** (`[Game] Console=1`). Type Unreal Engine 3 commands into the Game tab once a
  save is loaded (`slomo 0.5`, `stat fps`, `show fog`…). The release build stripped some commands; the
  reply (or "(done)") shows under the input. Log: `console:`.

## New in 0.9.8
- HDR is off on native Windows. The 0.9.7 highlight test showed the output never gets brighter than
  SDR white there (all patches equal, with or without Compensate), so it only cost performance. Use
  NVIDIA RTX HDR (NVIDIA app) instead. `[HDR] AllowWindows=1` brings it back for experiments.
  DXVK stays an optional presenter on Windows (`RealD3D9`).
- Proton / Steam Deck support: Wine is detected (`compat:` in the log), HDR stays available there
  (gamescope), and chain-loading guards against Wine handing back our own d3d9. See the Steam Deck section.

## New in 0.9.7
- HDR test pattern: "Highlight test" checkbox (shown while the pattern is on). The bottom row becomes
  six patches at 1x, 1.25x, 1.5x, 2x, 3x and 4x paper white. Look at the monitor, not a screenshot
  (captures clip at SDR white). If the patches get brighter left to right, real highlights reach
  the display and Compensate + HDR is worth keeping. If they all look the same, this output path is
  limited to SDR white and HDR gives nothing over the normal SDR mode: turn it off.

## New in 0.9.6
- HDR: "Compensate" toggle (`[HDR] CompensateSRGB`). The 0.9.4/0.9.5 test pattern fits one model
  exactly: our linear scRGB output is decoded as sRGB a second time somewhere after DXVK and shown
  at SDR white. Compensate pre-encodes the output with the sRGB curve relative to paper white, so
  that second decode cancels out. Test it with the test pattern: all 32 grey steps visible and the
  bars matching hdr_testpattern.png = fixed. (NVIDIA "layered on DXGI" made no difference.)

## New in 0.9.5
- Fixed: the game could stay in memory after quitting. While closing, its window loses focus,
  and the focus pause parked the game thread waiting for focus that never came back. The pause
  now stands down once the window is closing or hidden.
- HDR: the user's test pattern showed the output being decoded as SDR sRGB at SDR white and
  clipped at 1.0 (grey ramp flat above ~57%, 75% bars → full primaries), so the HDR colour space
  is lost after DXVK. The HDR tab now explains the NVIDIA "Vulkan/OpenGL present method → Prefer
  layered on DXGI Swapchain" setting, which routes Vulkan windows through DXGI (HDR-aware).

## New in 0.9.4
- HDR diagnostics: the log and HDR tab now say which GPU the game's monitor is plugged into and
  whether Windows HDR is really on for it (DXGI output colour space). A monitor on the iGPU while
  the RTX renders, or HDR off, both turn HDR output into clipped, neon SDR.

## New in 0.9.3
- HDR test pattern (HDR tab): replaces the frame with colour bars, a grey ramp, primaries and skin
  tones, sent through the same HDR path as plain SDR. `docs/hdr_testpattern.png` is the identical
  image: open it in the Windows Photos app (an SDR app) with HDR on and compare side by side.
  Same look = APRenewal's HDR path is correct and the punchiness is the game's own colours;
  different = something between APRenewal and the screen (DXVK, driver, display mode).

## New in 0.9.2
- HDR, still too punchy in 0.9.1:
  - Paper white now defaults to Windows' own "SDR content brightness" (`PaperWhite=0`), so the
    game's whites match the desktop's.
  - SDR → light conversion uses the sRGB curve Windows uses for SDR apps (`Curve=0`). The old
    gamma 2.2 (`Curve=1`) crushes shadows and adds contrast, which reads as "overblown".
  - Highlight expansion only touches pixels that are bright in *luminance*. 0.9.1 judged by the
    brightest channel, so strongly graded blue lights counted as highlights and got boosted.
  - A/B switch in the HDR tab: "Reference" shows the plain SDR image the way Windows would show
    it in HDR mode. If that already looks oversaturated, the cause is outside APRenewal.

## New in 0.9.1
- FOV is now set in degrees (`[View] FOV`, default 85). The game's camera is 60 degrees and
  AlphaFixer simply forced it to ~85 (with FOVFactor 1.20); 0.9 multiplied the game's 60 by 1.2,
  which gave a cramped ~69. Every camera mode is scaled by the same ratio, so aim/zoom still work.
  The log lists each camera FOV the game uses and what it became.
- HDR: removed the HDR10 option (froze the game with DXVK); scRGB only. Highlights now roll off
  softly below the display peak instead of clipping, there is a saturation trim (`[HDR] Saturation`),
  and the menu warns when paper white is too close to the display peak (blown-out whites).
- Recommended DXVK: 3.1.1 + the upstream HDR fix only (`d3d9_dxvk.dll` in the dxvk zip). The
  master build added newer present-timing code; it is the first suspect for the Steam overlay.

## New in 0.9
- **AlphaFixer is no longer needed.** Delete `winmm.dll` and `AlphaFixer.ini` from Binaries;
  APRenewal does its job (menu: View tab, ini: `[View]`), and does it differently:
  - FOV is scaled per camera module, live, from the size the game is actually rendering at.
    AlphaFixer overwrote every module with one fixed number worked out at startup, which flattened
    aim/zoom FOV changes. The FOV factor (default 120%, AlphaFixer's 1.20) now changes live.
  - HUD and menu width follow the real render size, so supersampling no longer squeezes them.
    On 16:9 they are left alone; wider screens get a centred 16:9 HUD and menus.
  - AO now uses the game's live camera FOV instead of a fixed 85 degrees (`[AO] FOVAuto`).
  - If winmm.dll is still there, APRenewal leaves FOV/HUD alone and says so in the log.

## New in 0.8.3
- HDR: found the oversaturation. DXVK's log shows Vulkan never offered HDR colour spaces
  (VK_EXT_swapchain_colorspace missing), so DXVK quietly presented an SDR swapchain and
  clipped APRenewal's HDR values to SDR white channel by channel - neon colours, HUD included.
  APRenewal now asks the Vulkan loader itself and stays SDR when HDR can't actually reach the
  display (the log and the HDR tab say why, including which vulkan-1.dll is loaded).
  Root cause: DXVK v3.1.1 never enables that extension (fixed upstream after the release, commit
  3c40ca1). Use a DXVK d3d9.dll built from master (renamed `d3d9_dxvk.dll`) until 3.2 is out.
- Supersampling: the window is kept at monitor size even when the game tries to resize it to
  the render size (which stretched the picture and threw off the mouse).

## New in 0.8.2
- HDR: back to scRGB by default - the HDR10 path in 0.8.1 gave a black screen (likely the DXVK
  version pairing an fp16 backbuffer with the HDR10 colour space). HDR10 stays as `Encoding=0`.
- HDR: the game's gamma ramp (its brightness slider) is kept out of DXVK while HDR is on and
  applied as an exponent in our own encode instead. Some DXVK versions run ramps through an SDR
  lookup table, which clips every bright colour channel separately.

## New in 0.8.1
- HDR: fixed the oversaturated image (HUD included). APRenewal now outputs HDR10 itself - PQ curve,
  Rec.2020 primaries - instead of handing linear scRGB to DXVK and trusting the conversion
  further down. The old path is still there as `[HDR] Encoding=1`. The peak clamp now keeps
  hue (scales the whole colour instead of clipping channels).
- Supersampling: 0.8 asked for the bigger resolution through the wrong door (the game reads its
  command line through msvcr80's `_acmdln`, not GetCommandLineW), so it kept rendering at native.
  Now the arguments go in there, and the render size is also listed as an adapter display mode,
  in case the game only accepts resolutions it finds in that list.
- Supersampling + HDR: the downscale now runs before the HDR encode.

## New in 0.8
Four new features, each with its own switch. Test them one at a time.

- **Colour grading** (menu: Colour tab, `[Grade]`). On by default, mildly: +0.1 EV, gentle S-curve
  contrast, +45% vibrance, slightly deeper blacks. Runs on the 3D image only, before the HUD.
  "Neutral" in the menu turns it into a no-op so you can compare.
- **Supersampling** (`[Display] RenderScale`, Display tab). Set e.g. 150 and restart: APRenewal
  hands the game `-ResX/-ResY` at 1.5x your desktop, keeps the window at desktop size, tells the
  game its window is the bigger size (window size and mouse coordinates are remapped), and
  downscales each frame with a Mitchell filter in linear light. Needs Borderless=1.
- **HDR output** (`[HDR]`, HDR tab). Experimental, needs DXVK as RealD3D9 and Windows HDR on.
  The backbuffer becomes fp16 scRGB through DXVK's D3D9 extension. The game's image is placed at
  paper white; near-white pixels of the 3D view are then pushed towards their real brightness,
  taken from the game's own HDR scene buffer (exposure fitted automatically), with a roll-off
  below the display peak. The HUD is drawn after that step, so it stays at paper white.
  Menus with a 3D background (armory) stay SDR-in-HDR.
- **HRTF 3D audio** (`[Audio] HRTF`, Audio tab). The game uses OpenAL through Creative's router.
  Back up `Binaries\OpenAL32.dll`, then copy the one from this package (OpenAL Soft 1.25.2,
  32-bit, built from source, built-in HRTF data). APRenewal maps the game's legacy device name
  ("Generic Software") to the default device and asks for stereo HRTF output; the menu can
  switch it live. Note: OpenAL Soft also runs the game's EFX reverb, which the Creative software
  path never did, so rooms may sound more reverberant than you remember. To undo, put the
  original OpenAL32.dll back.

## New in 0.7.1
- Fixed: AO turning floors, ceilings and stair faces pitch black at some camera pitches. The rebuilt
  surface normal was oriented by its z sign only, which flips on near-level floors; it is now turned
  to face the viewer.
- Fixed: the 24-bit depth calibration never completed (the game clears the depth buffer before the
  frame ends), so AO silently stayed on 16-bit depth. The depth is now snapshotted right after the
  base pass and both calibration and AO read the snapshot. Calibration problems are logged.

## New in 0.7
- **GTAO**: horizon-based ambient occlusion (after Jimenez et al. 2016 / XeGTAO), validated offline on real
  captured depth before shipping: contact shading on characters, no halos, grounded objects.
- **Precise depth** (`[AO] PreciseDepth=1`): the game's full-screen depth buffer is created as an INTZ
  texture (same trick ReShade uses in D3D9), giving 24-bit depth instead of UE3's 16-bit HDR alpha. That
  precision is what grazing walls need. It is calibrated against the HDR-alpha depth every 30 frames
  (least-squares fit of 1/d = A + B*z, checked before use) and falls back to 16-bit depth if anything fails.
- If the GTAO shader fails to compile on a driver, the previous AO shader is used instead (logged).

## New in 0.6.5
- SSAO: the sphere shrinks with the capped on-screen footprint, so objects close to the camera (Mike) get
  AO again; samples landing on something far in front of the sphere are ignored (fading weight), which
  removes the dark halo behind characters and the lost AO on objects next to their outline.

## New in 0.6.4
- Saving from the menu now covers everything: the post-processing master switch (`[PostFX] Enabled`) and
  the AO mode (`[AO] Mode`) were missing and always came back on at launch.
- Every saved value is read back from the file; failures, mismatches and a read-only ini are logged and
  shown in the menu. The menu shows which ini file it writes to.
- MulderConfig no longer manages `FPSLimit` (it rewrote it on every launch); set the frame cap in game.

## New in 0.6.3
- SSAO is now volumetric obscurance: it measures how much of a small sphere around each pixel lies behind
  the visible surface. No normals are rebuilt from depth any more, so there is no faceting on low-poly
  characters and no ring banding from fp16 depth steps on walls seen at grazing angles.
- Route counters: frames with effects switched off (F11) are counted as "off", not "skipped".

## New in 0.6.2
- Frames that render the world twice (a scene capture for reflections / screens first, then the real view)
  no longer lose SSAO/FXAA: a depth pre-pass starts a new scene and resets detection, so depth and the
  world image come from the last scene. Only quarter-res targets count as the bloom chain (1024x1024
  capture textures used to be mistaken for it, and the effects landed on the pre-pass buffer).

## New in 0.6.1
- Menu cursor: the DirectInput mouse buffer is drained completely while the menu is open (fixes the slow,
  jittery, ~1 s delayed cursor with high-polling-rate mice).
- Diagnostics: every frame is counted by the route that applied the effects (switch / at Present /
  no-bloom / SKIPPED / no 3D), in the log and in the menu's Diagnostics tab.

## New in 0.6
- **In-game menu** (Insert): every APRenewal option live (post-processing, FXAA, sharpening, SSAO and all its
  sliders incl. debug view, frame cap, focus pause, cursor lock, menu cursor speed), Save / Reload ini,
  diagnostics (effect stats, input status, one-click frame trace and capture). Built with Dear ImGui
  (MIT, see `src/imgui/LICENSE.txt`). While open, the game receives no keyboard, mouse or gamepad input:
  APRenewal captures the DirectInput mouse (via the game's dinput8 import = GOG dixi), window messages
  and XInput. Escape or Insert closes it.
- Frames without a bloom chain (final pass straight to the screen) now get SSAO/FXAA too.

## New in 0.5
- **SSAO** (`[AO]`): ambient occlusion on the 3D world, applied before the UI together with FXAA.
  Depth comes from UE3's HDR scene buffer, whose alpha channel holds scene distance (confirmed from an
  fp16 capture). APRenewal copies that buffer right after the base pass; AO = 12-tap normal-oriented
  samples, depth-aware blur, multiplied into the image (bright lights are protected).
  F10 cycles ON / OFF / DEBUG VIEW.
- 0.5.3: frames where UE3 writes its final post pass straight to the backbuffer (some post steps skipped,
  camera-angle dependent) are now processed too. When a 3D frame is still skipped, the log prints its
  render-target sequence once per 10 s. SSAO ignores very close samples (no faceting on low-poly
  characters, no rings on distant walls).
- 0.5.2: frame detection is order-aware (base pass must come before the bloom chain). Scenes that draw small
  render targets early no longer make PostFX/SSAO skip frames (= flicker). Logs `postfx stats` every 10 s
  if some frames were skipped.
- 0.5.1: off-screen samples are ignored (no more dark smears at screen edges), 16 samples with a tighter
  radius cap (no blotches up close), near-camera fade, softer/configurable highlight protection and a higher
  bias (no flicker on bright/shiny surfaces at grazing angles).

## New in 0.4
- **Before-HUD PostFX** (`[PostFX] BeforeHUD=1`): FXAA + sharpening now run on the finished 3D image,
  before Alpha Protocol draws its UI, so HUD and menu text stay crisp. Found via frame captures: the world
  ends in a full-res LDR target right after the quarter-res bloom chain; APRenewal processes that target
  in place when the game switches away from it. The log says `before-HUD target found` once it locks on.

## New in 0.3
- **Cursor confinement** (`[Focus] ConfineCursor=1`): the cursor stays inside the game window while it has
  focus, so it can't wander onto other monitors (hacking minigames!). Released on alt-tab/pause and on exit.
- **Render trace**: Ctrl+F11 logs one frame's render-target switches, clears, copies and draw counts.
- **Frame capture** (0.3.1): Ctrl+Shift+F11 also saves each render target of that frame as an image
  (`APRenewal\capture_HHMMSS\segNN_WxH_fmtF.png`, float targets fall back to .dds).

## New in 0.2
- **Pause on focus loss** (`[Focus] PauseOnFocusLoss=1`): when another window is in front, the game's main
  thread is parked inside its own message pump (APRenewal keeps the window responsive), and the game's clock
  is frozen for exactly that time, so gameplay *and* dialogue timers resume where they stopped.
  Audio already playing (e.g. a voice line) finishes.
  (0.2.1: fixes 0.2 hanging on refocus - it froze the clock while the engine kept running, and UE3's frame
  limiter spin-waits on the clock.)
- **PostFX** on the final frame: FXAA + contrast-adaptive sharpening (`[PostFX]`), toggle with **F11**.
  It's applied after the HUD is drawn, so HUD text gets a touch softer with FXAA on.
- If a shader fails to compile, the log says why and PostFX just switches itself off.

## What 0.1 does
- **Chain loading**: uses system d3d9, or DXVK (`RealD3D9=d3d9_dxvk.dll`) for Vulkan-backed frame pacing.
- **Shader hook**: hooks `D3DXCompileShader` in APGame.exe's import table. Logs every runtime compile,
  can dump the HLSL (`DumpSource=1`), and swaps in `APRenewal\override\<hash>.hlsl` if one exists
  (if it fails to compile, the error goes to the log and the original is used).
- **Frame limiter**: high-resolution waitable timer + short spin, runs after Present.
- **Borderless / VSync override** (experimental).
- **Diagnostics**: logs the game's present parameters before/after changes, and every Reset.

## First test run (please send back the log)
1. Install with the default ini, launch, load a save, walk around for a minute, quit.
2. In the log, check:
   - `Direct3DCreate9` appears → the game is on the D3D9 path (if it's missing, it went D3D10).
   - The `compile #` lines and the `total D3DXCompileShader calls` at shutdown:
     **0** → everything comes from the precompiled shader cache; overrides need a cache rebuild.
     **>0** → runtime shader replacement works right away.
3. Optional: `DumpSource=1` and send a few files from `APRenewal\dump`.

## Notes
- AlphaFixer already does FOV/HUD; APRenewal doesn't touch them.
- Above 60 fps: test physics, animations, cutscenes and minigames (hacking/lockpicking timers!) before keeping a higher cap.
- MSAA isn't forced: UE3's post chain would break it. Post-process AA comes in a later version.

## Build
`sh build.sh` (needs `g++-mingw-w64-i686`). Output: `out/d3d9.dll`.
