# APGame.exe — reverse-engineering notes

Steam build, 32-bit, unpacked (no SteamStub). Sections: `.text` 0x401000–0xE0BA0E (~10 MB code),
`.rdata`, `.data`. MSVC 2005 runtime (msvcr80). Most *gameplay* logic is UnrealScript inside the
cooked `.u`/`.upk` packages (CookedPC), not in the exe — the exe holds the engine plus the
game's native C++ classes (`AAP*`, `UAP*`).

## Tools (tools/re/)
- `natives.py APGame.exe` → `apgame_natives.json`: **2804 native functions mapped by name**
  (`Class::execFunction` → address). UE3 registers every native with a UTF-16 name
  `int<Class>exec<Func>` next to its function pointer; the script pairs them up.
- `dis.py 0xADDR[:len] …` (env `APGAME=path`) — disassembles with native names annotated on calls.
- `exec` thunks read their parameters from the script stack and then call the real C++ method,
  usually a virtual call — follow that call to get the actual implementation.

## Findings

### Camera (View module)
- The camera is **animation-node driven**: `UAPAnimNodeCamera` (an AnimNodeBlendList subclass).
  `execGetWeightedFOV` (0xA48670) calls vtable +0x18C = 0xA8F020, the FOV blend over child nodes
  (each child's FOV at +0x10C, weight at +0x90). This is the function the View module hooks.
- Vtable at 0x129AE84; +0x188 = 0x7113F0 is the generic blend-list `SetActiveChild`.
- Camera position/offset most likely also comes from animation (camera bone), so "camera
  distance" is best done on the final view: `ACamera::execGetCameraViewPoint` (0x4DAEE0),
  `AAnimatedCamera::execApplyCameraModifiersNative` (0x4DB230),
  `AAPPawn_Player::execPredictiveCameraModifications` (0xA4CCD0).
- Dialogue camera: `AAPDSSCamera` (matinee-style anim control natives).

### Dialogue (DSS = Dialogue Stance System)
- `UAPUIAction_DSSManager::execSetTimerValues` (0xA511B0): script passes one float, the native
  calls vtable +0x2E8 with it. **Prime candidate for the stance-choice timer** — scaling that float
  (hook site: `fld [esp+8]` at 0xA51200) could lengthen or freeze the dialogue timer.
  Needs an in-game test to confirm what the float means.
- `execSetNewStanceAsFocused` (0xA51140), `AAPGameInfo::exec{Get,Set}FastForwardDialogueState`
  (0xA33D30 / 0xA3A9E0), `AHUD::execNativeDrawSubtitles`, `APSeqAct_ShowStanceChoice`.

### HUD
- `AHUD::execShouldDrawHUD` (0xA356D0) → vtable +0x33C: one place to hide the whole HUD.
- `AAPHUD::exec*` draws (dialogue HUD, awareness meter, enemy indicators, hit locator…),
  `UAPSeqAct_ToggleHUD`. HUD canvas setup is the View module's HUD hook (0x50CA7F).

### Console / engine
- `APlayerController::execConsoleCommand` (0x4DB730) → vtable +0x110
  `FString ConsoleCommand(const FString&, UBOOL)`. The shipping build has no console UI, but the
  command path exists: calling it with a PlayerController pointer would give our menu a console
  (`slomo`, `show …`, `set …`, `toggleui`, …). Needs a PlayerController pointer and FString.
- Command-line switch **`nomovie`** is present (UE3: skip startup movies). We already own the
  command line (`_acmdln`), so "skip intro logos" is a one-line feature.
- `AAPGameInfo::execBenchmarkingConsoleCommand`, `UAPMissionManager::execTurnOnIntelCheats` /
  `execTurnOnTrophyCheats`, `UCheatManager`, `EnableGodMode` exist (debug leftovers).
- SystemSettings keys present: `MaxShadowResolution`, `ShadowFilterQualityBias`,
  `ShadowTexelsPerPixel`, `MaxAnisotropy`, `DetailMode` (user's APEngine.ini: 4096 / 0 / 2.0 / 4 / 2).

### Minigames
- `AAPPlayerController::execStartNewHackingMiniGame` (0xA50790),
  `UAPUIAction_HackingMiniGameManager::execDrawInterface` (0xA51220), lockpick/hacking UI scenes.

### Movies (0.10.0)
- `nomovie` is checked at 0xD73FD0: it installs a null movie player instead of the real one
  (0x4E4790), which would also kill in-game Sofdec movies (`Movie_*Aftermath*.sfd`, news reels).
- Real player vtable: slot 0x12173B0 = GameThreadPlayMovie 0x4E49F0 (compares the name to
  `OpeningMovie`). APRenewal refuses calls before the first 3D scene.

### Shadows (0.10.0)
- The shadow depth buffer is a 2048² R32F render-target texture regardless of MaxShadowResolution.
  SystemSettings are bound by name→field tables (e.g. around 0x498230); the 2048 clamp itself is not found yet.

### Shaders and caches (user's files, 0.10.1 — not in the repo, they're Epic/Obsidian's)
- The game ships its shader **source** (`Shaders/*.usf`, UE3 2008 + Obsidian `AP_*` files) and the
  compiled caches: `GlobalShaderCache-PC-D3D-SM3.bin` (334 global shaders with CTAB, compiler
  9.27.952.3022) and `RefShaderCache-PC-D3D-SM3.upk` (60 MB, material shaders). The game never calls
  D3DXCompileShader (log: 0 calls) — everything comes from the caches via CreatePixelShader.
- CTAB constant names identify each shader at creation time, e.g.:
  - Final HDR→LDR blend (3 variants: bloom only `AP_BloomBlendPixelShader`, AP DOF+bloom+vignette,
    Epic DOF/uber): constants HSBContrast, GammaColorScaleAndInverse, LensFilterImage, FilterColor…
    It does `saturate(LinearColor)` → **hard clip of highlights**, then `sqrt` as a cheap gamma 2.0,
    then brightness/contrast/colour filter. Scene colour range is [0, 4] (MAX_SCENE_COLOR).
  - Shadow projection (`TBranchingPCFModProjectionPixelShader<Light, Quality, Manual/Hw/Fetch4 PCF>`,
    modulated shadows): SampleOffsets / RefiningSampleOffsets / EdgeSampleOffsets / ShadowBufferSize,
    all in texels of the 2048 map → with [Shadows] Scale they could be divided by the scale.
- Packages: Arch_Glass.upk is a 109-byte stub; glass materials live in the material caches.
