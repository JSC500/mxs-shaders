# MXS Shaders

This project aims to bring improved visuals such as shadows from real time dynamic lighting to the MX Simulator game.

## Hard goals
1. Do not modify the .exe or the memory of the running process. You can read memory from the running process but don't modify it.
2. The mod should work with multiple versions of MX simulator, not just the latest snapshot.
3. The path where the game lives is normally C:\Program Files (x86)\MX Simulator, but can live other places. Ask the user if this folder does not exist.

## Layout
- `src/`: the `opengl32.dll` proxy. `hooks.c` is the GL hooks, shadows and recording, `post.c` the post passes, `ui.c` the F8 panel, `pack.c` shader packs.
- `src/` for Linux: the same shared files build into `libmxs-shaders.so`, loaded with `LD_PRELOAD` (see "Linux" below). `preload.c` replaces `proxy.c`, the stubs and the wgl hooks. `win_compat.h` gives the shared code the Win32 names it uses, on top of `platform_linux.c`, `input_x11.c` and `log_linux.c`. The few Windows-only parts (wgl hooks, `borderless_apply`, the GDI font, the input window) are behind `#ifdef _WIN32`.
- `shaderpacks/default/`: the default pack, built into the DLL by `scripts/gen_pack.ps1` (into the Linux library by `scripts/gen_pack.c`).
- `shaderpacks/psx/`: an optional PS1 look. It isn't built in, it ships as `psx-pack.zip`.
- `shaderpacks/iMotoWeatherPack/`: Deband, Daltonize and DisplayDepth translated from ReShade, run after the default lens. Their settings are constants in `reshade.glsl`. It isn't built in, it ships as `iMotoWeatherPack-pack.zip`. The release packs every folder in `shaderpacks/` as `<name>-pack.zip`.
- `%LOCALAPPDATA%\mxs-shaders`: everything at runtime: `game\` (the dev copy of the game, if there is one), `mxs-shaders.ini`, `mxs-shaders.log`, `frames\`, `shaders\`, `riders\`, `shaderpacks\`.

Only the repo's own code belongs here. Andre's MXS Effects is a closed, licensed ReShade add-on: its features can be rebuilt from what they're described to do, but don't decompile it or commit its files, or any other author's.

## Build
Run this from PowerShell in the repository root. There's no Python or `strings` on the dev machine.

```powershell
cmd /c "`"$PWD\build.bat`"" | Select-String -Pattern 'error|warning|Built'
```

`build.bat` finds Visual Studio with the C++ tools through `vswhere`, and the `vswhere.exe` line in the output, from Visual Studio's own setup script, is harmless. It deploys the pack to `%LOCALAPPDATA%\mxs-shaders`, overwriting the deployed pack, and the DLL to the dev copy when there is one. Shader-only changes don't need a build: copy the file to `%LOCALAPPDATA%\mxs-shaders\shaderpacks\default`, and **Reload Current Shader** in the F8 panel, shown with `debug=1`, reloads it in a running game. Don't change the user's own `mxs-shaders.ini` values without saying so.

## Testing with the user
Test in the dev copy when there is one, because the default game folder needs admin. Without one, the game folder is the test install: ask before copying `build\opengl32.dll` into it, or let the user copy it. Most features need a person in the game, so:

1. Check `Get-Process mx` first. A running game locks the DLL.
2. Tell the user before launching. List what to do and what to look for in short numbered steps, including anything that has to happen in a set order, such as chase view then first person.
3. Launch with PowerShell, `run_in_background: true`, and wait for the notification when they quit. Don't poll.

With no dev copy, set `$g` to the game folder instead.

```powershell
$g="$env:LOCALAPPDATA\mxs-shaders\game"; $p=Start-Process -FilePath "$g\mx.exe" -WorkingDirectory $g -PassThru; $p.WaitForExit(); "exit $($p.ExitCode)"
```

4. When it exits, read the log and captures, then report what the log proves and ask about what only the user can see.

A completion notification is not the user's feedback. Don't claim something looks right on screen until they've said so.

## Checking results yourself
- `mxs-shaders.log`: grep for `failed`, `^GL error`, `^frame \d+:` (fps and GPU time) and `^rigid`. GL errors #1 (`INVALID_ENUM`, frame 0) and one texture name error on leaving a race are known. It is overwritten on each start.
- Captures need `debug=1`: automatic at frame 300 and every 1800 frames, F9 on demand. They're in `frames\` as `frame_NNNNNN.txt` (every GL call) and `.bmp`. Desktop screenshots can't see the game.
- Read can't open BMPs. Convert first, into the scratchpad:

```powershell
Add-Type -AssemblyName System.Drawing
$b = New-Object System.Drawing.Bitmap $bmp
$s = New-Object System.Drawing.Bitmap $b, 1100, ([int](1100*$b.Height/$b.Width))
$s.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
```

- The F8 panel is drawn after the capture, so it isn't in BMPs. Ask the user for a screenshot.
- For temporary logging, mark it `/* TEMP */` and remove it before finishing. To see the view a capture shows, compare the VAOs drawn. With the test bike, chase view draws 186-192 and first person 196-202, but those numbers change with the track and bike.

## Testing without the user
For anything that happens before a race, such as first-run behaviour, packs and defaults, start the game against an empty profile by pointing `LOCALAPPDATA` at a scratch folder, wait for `wglSwapBuffers reached` in that folder's log, then close it. This leaves the user's settings alone. Warn the user first, because a game window opens briefly.

The game compiles every shader at startup, so this also checks that pack changes compile: look for `post-processing ready` and the `role ... using pack shader` lines, and no `failed`. If the installed DLL is older than the source, put the changed default-pack files in the scratch folder's `shaderpacks\default`. Files on disk override the built-in pack, so the new shaders are tested without touching the install.

## Linux
The native Linux game is `mxbin`, started by the script `mx`. It draws with a legacy `glXCreateContext` context into a child of its top-level window, under XWayland on GNOME. Everything at runtime goes to `$XDG_DATA_HOME/mxs-shaders`, normally `~/.local/share/mxs-shaders`, with the same layout and ini as on Windows.

- **Build:** `make` gives `build/libmxs-shaders.so`. `make exports` lists the exported symbols, which must be only the hooked GL, GLX and X functions. `make test` runs `tests/run.sh`: a stand-in `mxbin` on Xvfb with Mesa, real X input through XTest, and a frame around its window like GNOME's. With `MXS_GAME_SHADERS=<probe shaders folder>` it also compiles the game's shaders, so the packs have to compile and link on Mesa. Those files belong to the game: keep them out of the repo.
- **Keep Windows unchanged:** every Linux change to a shared file goes in an `#ifndef _WIN32` branch or into `win_compat.h`. There's no Windows compiler on the Linux side, so check that what MSVC sees is the same as before.
- **Launch:** `mxs-shaders.sh` in the game folder starts `mxbin` with the library preloaded through a link in the data folder, because `LD_PRELOAD` can't name a path with a space. `MX_HOME` points it at a game folder elsewhere. The library does nothing in any process not called `mxbin`.
- **Log:** the same lines as on Windows, except `glXSwapBuffers reached the library` in place of `wglSwapBuffers reached`. Linux-only lines: `focus on the game` and `focus elsewhere` with window ids, `panel open` and `panel closed: N pointer events were held back`, `cursor hidden` and `cursor shown again`, and `ui: font <path>`.
- **Testing without the user:** point `XDG_DATA_HOME` at a scratch folder instead of `LOCALAPPDATA`.

How the Linux game behaves, from a probe run (NVIDIA under XWayland on GNOME, gamepad rider):
- **Input:** it never grabs or warps the pointer. It reads clicks both as events and from `XQueryPointer`'s button mask, twice a frame. So while the panel is open, `preload.c` drops button events and its `XQueryPointer` hook reports no buttons and a still pointer. Keys still reach the game, as on Windows. Mouse-look riders haven't been tested.
- **Focus:** GNOME frames the top-level window and focuses it, not the drawing child. Any window from the child up to the frame counts as the game.
- **Window size:** it changes around fullscreen toggles (1920x1043 and 1920x1080), so it's read from X, not assumed.
- **Cursor:** the game gives its drawing window a cursor when creating it and imports no `XDefineCursor`. Hiding swaps in a blank cursor and puts the recorded one back.
- **Rendering:** the same shader names and draw order as on Windows, and its own 4096x4096 framebuffer before the main pass. VAO numbers depend on the bike, so detection must keep working from draw order.

## How the mod works
- **Texture units:** the shadow maps use 8-9, the post passes 10-16, the F8 panel's font 17 and the weather noise 18. A new texture that stays bound needs its own unit above these.
- **Performance:** dust particles and models overdraw a lot, so anything per pixel in `terrain.fsh`, `model.fsh` or `sprite.fsh` multiplies. Procedural noise there cost a big drop in fps: precompute it into a texture, like `weather_noise_create`, and put work behind uniform checks so it costs nothing when it's off.
- **Weather:** `common.glsl` holds the weather code that every pack shader shares. Anything that has to stay fixed to the track goes through `u_world_from_view`, which `weather_apply` sends to the game's programs each frame and `post.c` sends to the post passes. Rain and snow are drawn around the camera in post, so they don't stay in place as the camera moves, and puddles reflect only the sky.
- **Sun override:** Sun Height and Sun Turn change the sun's world direction in `sun_effective`, and the result replaces `u_light_position` everywhere, including the game's own shading. The light can arrive before the frame's camera, so that shading may be a frame behind while the camera turns.
- **Exposure texture:** a 1x1 RGBA16F target eased over frames: red is the average brightness, green the distance in focus, blue how much of the sun is in sight, for the lens flare.
- **F8 panel:** settings are split into the Look, Sun & Weather and Display tabs, and each new item needs one. Row height follows the longest tab. Sliders are saved per pack, toggles and choices in the main section.

## How the game behaves
- **Sprites:** the sprite shader also draws terrain tiles into the game's own framebuffer, so screen-only effects must check the framebuffer first. `sprite_set_mode` handles this. Missing it caused big rectangles on the ground.
- **Skinned draws:** their modelview sits about 2,000 units from the bike, because the bone translations hold the rider's world position. Measure distances from posed bones.
- **Physics:** it can tick twice in one rendered frame, which caused shadow flicker at speed.
- **First-person meshes:** the game loads chase and first-person sets of the rider, and draws the trimmed set in first person. Not all gear has a trimmed set, so the reliable sign of chase view is a rigid draw right after the rider, the helmet. The full set is found from `glBufferData` sizes on join, 10 VAOs before the trimmed one for the test bike.
- **Riders and track objects:** tracks can have animated objects that are skinned too, and they appear and disappear with the camera. The rider is the first unbroken run of skinned draws with one bone count (9 here), and only those count as rider draws.
- **Rigid parts:** first person skips the helmet, frame and rear wheel. They're the draws next to the rider in chase view, and each group learns the bone it moves with. The result is saved in `riders\` with the bone scores, keyed by the trimmed meshes, so a new combination needs one trip to chase view and more riding there refines the choice. A short look from a standing start can pick a poor bone, because nothing moves. No rider bone is fixed to the bike: against the bone the helmet follows, the rear wheel moves about 1 unit up and down and 3 along the bike while riding, so a part's shadow can sit off by that much when the rider's posture changes. Parts follow their bone at their average position, and the rear wheel shows no suspension travel in the shadow. The fork assembly is drawn live in first person and would be a truer anchor, but it turns with the steering. Only skinned draws count as moving, so bike parts miss the camera-step shadow correction. The number of rider draws depends on the gear.
- **Fullscreen:** the game calls `ChangeDisplaySettingsA` directly, which `opengl32` can't intercept, and hooking it would break hard goal 1. Windowed mode with `borderless=1` avoids the black screen. Real HDR output would need a D3D11 swap chain through `WGL_NV_DX_interop2`.
- **Hidden rider option:** "hide first person rider" skips the rider, helmet and rear bike draws, and the bone pose is sent right before them, so it's probably missing too. No hidden frame has been captured with uniform logging. Only a frozen stored pose attached to the bike could work there.
- **Shadow range:** `shadow_range` reaches 1.5 times its value straight ahead. `SHADOW_DEPTH_RANGE` is 800, and `common.glsl`, `post_shafts.fsh` and the map matrix depend on it, so changing it means rescaling those biases. It covers the map's corners at the default range of 500. A test at range 1000 with depth 1600 didn't change the remaining pop-in, so distance isn't its cause. Casters past the sunward end are kept by `GL_DEPTH_CLAMP` during the shadow pass, but the far end has no such help.
- **Overlay and menus:** after the last `glEnable(GL_CULL_FACE)` of a race frame, the game draws only the 2D overlay. HUD and menus share one font, but menu rows use plain 1x1 boxes, and `hud_draw_hidden` and `cursor_update` use that count to tell them apart. Text-only screens have no boxes.
- **Terrain:** the game draws only the terrain blocks near the view, 161 to 599 draws a frame depending on the camera, while model draws stay at about 1,250. A hill leaving the view would take its shadow with it, block by block. All terrain comes from one height texture, with blocks placed by `u_meshoffset` and `u_meshscale` (30 to 241 units on the test track, each mesh one scale across), so `draw_terrain_fill` adds the blocks the game skipped to the shadow maps, in the largest size that fits the gaps.
- **Shadow pass cost:** the replay is the largest stage of a frame, and on the test track most of the near map's cost was scenery that can't reach it, about 2,500 of 2,660 model draws a frame. `replay_stream` skips terrain blocks and model draws that are off a map's sides. Models use a sphere per VAO, read back once with `mesh_sphere_current`, and meshes that can't be measured are always drawn. Only the sides are tested, because casters anywhere along the sun's line still shade the map.
- **Foliage:** trees are model draws with cut-out alpha, blended, and the game draws them solid from alpha 0.75 up. `model.fsh` turns alpha into MSAA coverage for smooth edges, and the output alpha is both coverage and blend. An edge as wide as `fwidth` on distant, mip-blurred leaves thinned whole crowns, and the HDR sky, several times brighter than a leaf, showed through as blue speckle toward a low sun. So the edge is capped at `CUTOUT_EDGE_MAX` and cut-outs aren't thinned past it.
- **Sprites and shadows:** sprites don't cast shadows, and the camera-dependent sprite draws aren't what was removing them.
- **Baked terrain shadows:** the decal shader writes a baked shadow into the green channel of the terrain normal maps, and the game and `terrain.fsh` multiply sunlight by it. Ignoring it in a test didn't change the shadows that vanish in rectangles.
- **Time of day:** the game gives one light per track and no way to tell a night track, so `time_of_day` is a global setting the player changes. It fades the sun effects through `u_night`, and at 100 the shadow maps aren't drawn. From 50 up, post adds stars and a moon where the sky is clear. Stadium floodlights have no data in the game, so they'd need placing by hand.
- **Reflections:** the game draws nearly every model a second time with the reflection shader, added over the lit one. Screen-space reflections were tried there, marching through last frame's scene, and removed because they were barely visible. In practice only the bikes reflect strongly, and their surfaces mostly face the camera or the sky, which reflect what isn't on screen.
- **Units:** game units aren't metres. The camera moves about 65 units per second at a displayed speed of about 39.
