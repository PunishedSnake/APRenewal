# APRenewal

A `d3d9.dll` mod for **Alpha Protocol** (2010, Unreal Engine 3): modern rendering, widescreen,
controller support, HRTF audio and quality-of-life fixes, all switchable live from an in-game menu.

Works with both the **GOG** and the **Steam** release. Patches inside the game's code (FOV/HUD, intro
skip, console, gamepad/keyboard switching) find their spots by name or byte pattern and check them
before writing; on an unknown build a feature switches itself off with a log line instead.

## Install
1. Copy into `Alpha Protocol\Binaries\`:
   - `d3d9.dll` and `APRenewal.ini`
   - `xinput1_3.dll` (controller support)
   - `OpenAL32.dll` (optional, for HRTF audio; back up the original first)
2. Remove what APRenewal replaces:
   - AlphaFixer: `winmm.dll` and `AlphaFixer.ini`
   - GOG's "dixi" controller wrapper: `dinput8.dll` and dixi's `xinput1_3.dll`
3. Start the game and press **Insert** to open the menu.

Settings live in `APRenewal.ini` (the menu saves there). The log goes to `Binaries\APRenewal\APRenewal.log`.

**DXVK (optional):** rename DXVK's 32-bit `d3d9.dll` to `d3d9_dxvk.dll`, put it in `Binaries\` and set
`[Chain] RealD3D9=d3d9_dxvk.dll`.

**Steam Deck / Linux (Proton, untested):** copy `d3d9.dll`, `APRenewal.ini` and `OpenAL32.dll`, leave
`RealD3D9` empty, and set the launch options to `WINEDLLOVERRIDES="d3d9,openal32=n,b" %command%`.
Steam Input handles the controller.

## Features

### Image
- **Anti-aliasing:** SMAA, FXAA 3.11 or fast FXAA, plus a temporal stabiliser (reprojected,
  variance-clamped history) and contrast-adaptive sharpening. All of it runs before the HUD, so text
  stays crisp.
- **Supersampling:** renders at up to 2× your desktop resolution and downscales with a Mitchell filter
  (`[Display] RenderScale`).
- **Anisotropic filtering** forced on the game's textures, with mip bias.
- **Borderless window**, precise frame cap, VSync override.

### Lighting and materials
- **Ambient occlusion:** GTAO (horizon-based) from the game's 24-bit depth buffer. It is removed behind
  window glass and fades with fog.
- **Soft shadows:** shadow maps up to 4× resolution, a rotated-disc soft filter and contact shadows.
- **Screen-space reflections** on floors and metal, with roughness blur. Rough materials (snow, dirt,
  carpet, concrete…) never reflect.
- **Skin:** subsurface scattering and tighter, glossier highlights.
- **Weapons and metal:** glossier highlights and metal reflections.
- **Window glass:** clearer, angle-dependent (Fresnel) reflections, screen-space reflections of the
  surrounding environment from the current frame (characters are left out; cubemaps fill in where the
  screen has nothing to show), refraction control.
- **Character lighting:** fill and evenness controls for the game's light probes.

### Post-processing
- **Tonemapping:** a smooth highlight roll-off instead of the game's hard clip, hue preservation,
  selectable gamma, and a soft toe instead of its black clip (`[Colour] ShadowDetail`).
- **Colour grading:** exposure, S-curve contrast, vibrance, saturation, a soft black level and
  temperature, all in linear light.
- **Bloom:** the game's bloom tent-filtered, plus a wide HDR bloom.
- **Light shafts** from lamps and windows, anchored to their sources in the world.
- **Depth of field** with round bokeh, plus the game's motion blur, fog and edge AA, each with its own
  strength.
- **HDR output** (scRGB through DXVK). Works under Proton with gamescope HDR; off by default on Windows.

### View and game
- **Field of view:** Hor+ widescreen with a configurable FOV that applies to every camera. On wider
  screens the HUD and menus stay centred at 16:9. Replaces AlphaFixer.
- **Skips the intro logo movies.**
- **No pop-in:** occlusion culling turned off, longer draw and detail distances.
- **Developer console** in the menu, including Alpha Protocol's own rendering commands.
- **Raw mouse:** Unreal's mouse smoothing switched off for direct 1:1 aiming.
- **Pause on focus loss:** game and dialogue timers stop while you alt-tab. Also keeps the cursor
  inside the window.

### Controllers
- One merged player-1 pad built from everything plugged in:
  - **Xbox** pads (any connection)
  - **DualSense, DualSense Edge and DualShock 4** over USB or Bluetooth, with rumble and light bar colour
  - Other **DirectInput** pads: Switch Pro, DualShock 3, Logitech, PS2 adapters, generic, with your own
    maps in the ini
- **Switches automatically** between gamepad and keyboard + mouse, depending on what you touched last.
- Rumble strength, extra dead zone, Swap A/B and free button remapping.

### Audio
- **HRTF 3D audio** for headphones through OpenAL Soft. The included `OpenAL32.dll` is OpenAL Soft
  1.25.2. It can be switched live.

### Tools
- In-game menu (Insert) with every option live, plus Save / Reload and **quality presets**
  (Performance / Balanced / Quality / Ultra).
- **Shader disk cache:** APRenewal's shaders compile once and load instantly on later launches.
- F11 toggles all post-processing; F10 cycles AO between on, off and a debug view.
- Ctrl+F11 logs a one-frame render trace. Ctrl+Shift+F11 also saves every render target of that frame.
- A shader census: each pixel shader of a frame, with its hash and constants.
- Shader dump and override (`[Shaders]`).

## Build
`sh build.sh` (needs `g++-mingw-w64-i686`) → `out/d3d9.dll` and `out/xinput1_3.dll`.

Third-party code: Dear ImGui (MIT, `src/imgui/`) and SMAA (MIT, `docs/SMAA-LICENSE.txt`).
