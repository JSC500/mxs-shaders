# MXS Shaders

Better graphics for MX Simulator: real-time sun shadows, ambient occlusion, HDR lighting with bloom, height fog, sun shafts, "normal maps" (parallax effect) for the dirt, motion blur, depth of field and colour grading.
Also, it makes Windowed Mode fullscreen, so alt-tabbing doesn't take 10 seconds anymore on HDR monitors.

AND YOU CAN FINALLY HIDE THE PESKY CURSOR WHEN RACING 😂

The mod is one DLL. It doesn't change `mx.exe` or the game's memory, and it only affects how the game is drawn. It works with older snapshots of the game too.

> This is an unofficial fan project, not made by or connected to the developer of MX Simulator. Wait for confirmation by JLV and rf to use it in any multiplayer race.

## Screenshots

| Game's own look | With the mod |
|---|---|
| ![The game's own look](example%20screenshots/normal.webp) | ![The same scene with the mod](example%20screenshots/shader.webp) |

## Install

1. Download `opengl32.dll` from the [Releases](../../releases) page.
2. Copy it into your game folder, next to `mx.exe`. This is usually `C:\Program Files (x86)\MX Simulator`, and Windows will ask for permission.
3. Start the game as usual.

The first person shadow only works properly after you have switched to 3rd person once (per model- afterwards it also works immediately). That's because in first person, only the riders arms are loaded. Once you switch to 3rd person, the mod caches all meshes.

To uninstall, delete `opengl32.dll` from the game folder. To remove the settings too, delete `%LOCALAPPDATA%\MX Simulator\mxs-shaders`, and `%LOCALAPPDATA%\mxs-shaders` if an older version left it.

### Linux

For the game's native Linux build, without Wine or Proton. This build is new, so if something looks wrong, please report it with `~/.local/share/mxs-shaders/mxs-shaders.log`.

1. Download `libmxs-shaders.so` and `mxs-shaders.sh` from the [Releases](../../releases) page, or build them (see [Building](#building)).
2. Copy both into your game folder, next to `mx` and `mxbin`, and make the script executable: `chmod +x mxs-shaders.sh`.3. Start the game with `./mxs-shaders.sh` instead of `./mx`. A desktop shortcut can point at the script too.

`./mx` still starts the game without the mod. The script loads the library through a link in `~/.local/share/mxs-shaders`, because `LD_PRELOAD` can't handle the space in a folder called `MX Simulator`. If the script isn't in the game folder, tell it where the game is: `MX_HOME="/path/to/MX Simulator" ./mxs-shaders.sh`.

The F8 panel needs FreeType and a bold italic font: DejaVu Sans, Liberation Sans or Noto Sans, which most distributions install. Another font can go in `~/.local/share/mxs-shaders/fonts/panel.ttf`. Borderless Window isn't needed on Linux, where fullscreen doesn't change the display mode, so it isn't in the panel.

To uninstall, delete the two files from the game folder. To remove the settings too, delete `~/.local/share/mxs-shaders`.

## Settings

Press **F8** in the game to open the settings panel. Changes apply straight away and are saved automatically, except those marked with a red `*`, which need a restart. It works best in a menu or while paused.

To use another key, click **Panel Key** at the top of the panel and press the new key. Pick one the game doesn't use, because the game sees it too. Escape or a click cancels.

**Turn Shaders Off** at the bottom of the panel switches straight to the game's own look, and the same button switches back. The panel stays available, and the choice is kept for the next start. Borderless Window is the one thing that stays, since it's about the window and not the look.

Settings are stored in `%LOCALAPPDATA%\MX Simulator\mxs-shaders\mxs-shaders.ini` (on Linux `~/.local/share/mxs-shaders/mxs-shaders.ini`), which you can also edit by hand. The file is the same on both, so one can be copied to the other. For the strength settings, 0 is off.

Older versions kept everything in `%LOCALAPPDATA%\mxs-shaders`. Newer ones don't read it and start with fresh settings, so put any shader packs you added into the new folder and delete the old one.

Each shader pack remembers its own sliders. In the file, the default pack's are in `[mxs-shaders]` with the other settings, and another pack's are in a section of their own, such as `[pack psx]`.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `enabled` | 1 | 0 or 1 | 0 shows the game's own look. Turn Shaders Off in the panel sets it for you |
| `clock` | 0 | 0 to 96 | Time of Day in the panel. 0 is the track's own light, then quarter hours: 1 is 00:00, 49 is noon and 96 is 23:45. The sun rises around 05:00 and sets around 19:00, with twilight after, and at night a full moon lights the track and casts the shadows. Sun Turn turns the sun's whole path. It applies to every track, so change it when you change tracks. It replaces Day to Night and Sun Height, whose old values are carried over |
| `dynamic_sky` | 1 | 0 or 1 | Dynamic Sky in the panel. 1 draws a real sky with volumetric clouds over each track's skybox, keeping any mountains or trees painted into it. 0 keeps the track's own painted sky, which still darkens with Time of Day |
| `cloud_cover` | 20 | 0 to 100 | Cloud Cover in the panel: how much of the sky is cloud, from clear through scattered clouds to a grey overcast. Rain, Storm and Snow cover the sky at least as much as an overcast. It replaces the old Cloudy weather, whose setting is carried over |
| `cloud_quality` | 2 | 0 to 3 | Cloud Quality in the panel: Off, Low, Medium or High. Off draws flat clouds, which cost almost nothing. Low suits older GPUs, High is sharpest |
| `fog` | 10 | 0 to 300 | Haze strength |
| `parallax` | 200 | 0 to 200 | Depth of ruts and dirt |
| `motion_blur` | 10 | 0 to 200 | Camera blur, 100 is a 1/60 second shutter |
| `dof` | 15 | 0 to 200 | Depth of field |
| `saturation` | 110 | 0 to 200 | Colour strength. 100 leaves colours as they are, 0 is black and white |
| `contrast` | 20 | 0 to 100 | Darker shadows and brighter highlights |
| `split_tone` | 20 | 0 to 100 | Cool shadows and warm highlights |
| `bloom` | 100 | 0 to 200 | Glow around bright things, 0 turns it off |
| `shadow_range` | 500 | 20 to 500 | How far shadows reach. Raise `shadow_size` with it to keep shadows sharp |
| `shadow_size` | 8192 | 1024, 2048, 4096, 8192 | Shadow sharpness. Lower it on older GPUs, 8192 uses about 512 MB of video memory |
| `msaa` | 8 | 0, 2, 4, 8 | Edge smoothing, 0 turns it off |
| `anisotropy` | 16 | 1, 2, 4, 8, 16 | Texture sharpness at an angle, 1 turns it off |
| `borderless` | 1 | 0 or 1 | See below |
| `hide_hud` | 0 | 0 or 1 | Hides the game's HUD in races, such as the tachometer, pitboard, messages and chat. Menus with rows stay visible |
| `hide_cursor` | 1 | 0 or 1 | Hides the mouse cursor while riding. It comes back in menus |
| `ui_key` | 119 | Windows virtual key code, on Linux too | The key that opens the panel, 119 is F8. Panel Key in the panel sets it for you |

Only `shadow_range`'s lower limit of 20 is enforced, and `msaa` and `anisotropy` are capped at what your GPU supports. Higher values for the others still work but may look wrong. Above 500, shadows far away may go missing, and higher values make shadows softer.

After editing the file by hand, restart the game to apply your changes.

### PSX look

For a PS1 style, download `psx-pack.zip` from the Releases page and extract the `psx` folder into `%LOCALAPPDATA%\MX Simulator\mxs-shaders\shaderpacks`. Then pick `psx` on the Shader Pack row of the F8 panel, and pick `default` to go back.

![The PSX look](example%20screenshots/psx.webp)

### iMoto Weather Pack

The default look with Deband, Daltonize and DisplayDepth translated from ReShade. Download `iMotoWeatherPack-pack.zip` from the Releases page, extract the `iMotoWeatherPack` folder into `%LOCALAPPDATA%\MX Simulator\mxs-shaders\shaderpacks`, and pick it on the Shader Pack row of the F8 panel.

### Borderless fullscreen to avoid black screen when alt-tabbing (Windows)

Set the game's video mode to windowed. With `borderless` on, which is the default, the window then covers the whole screen and the monitor stays on. Set it to 0 for a normal window.

## Shader packs

The look comes from a shader pack. To use another one, put its folder in `%LOCALAPPDATA%\MX Simulator\mxs-shaders\shaderpacks` (on Linux `~/.local/share/mxs-shaders/shaderpacks`), set `pack=` to the folder's name in `mxs-shaders.ini`, and restart the game. Set `pack=default` to go back.

To write your own, see [docs/shader-packs.md](docs/shader-packs.md).

## Building

Needs Visual Studio 2022 or its Build Tools with the C++ workload. Run `build.bat` to get `build\opengl32.dll`.

On Linux, install gcc, make and the X11 and FreeType headers (Debian and Ubuntu: `sudo apt install build-essential libx11-dev libfreetype-dev`), then run `make` to get `build/libmxs-shaders.so`. `make test` runs the tests against a stand-in for the game, and needs `xvfb`, `libxtst6` and ImageMagick.

On either system, set `debug=1` in `mxs-shaders.ini` for frame captures with **F9**, shader dumps, GL error logging and a **Reload Current Shader** button in the panel.

## License

[MIT](LICENSE)
