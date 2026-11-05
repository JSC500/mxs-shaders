# Writing a shader pack

A shader pack is a folder of GLSL files that replaces how the game looks. You don't need to build anything: edit text, reload it from the game's settings panel, and see the result.

## Quick start

The paths below are for Windows. On Linux, `%LOCALAPPDATA%\mxs-shaders` is `~/.local/share/mxs-shaders`, and everything else is the same, packs included.

1. Make a folder in `%LOCALAPPDATA%\mxs-shaders\shaderpacks`, for example `my-pack`.
2. Set `pack=my-pack` and `debug=1` in `%LOCALAPPDATA%\mxs-shaders\mxs-shaders.ini` and restart the game.
3. Copy the file you want to change into your folder. They're in `default-pack.zip` on the Releases page, or in `shaderpacks/default` in this repository.
4. Edit it, press **F8** in the game and click **Reload Current Shader**. The button only appears with `debug=1`.

A pack only needs the files it changes. Anything it doesn't have comes from the built-in default pack, including files it `#include`s.

To use a pack someone else made, put its folder in `shaderpacks`, set `pack=` to the folder's name, and restart. Set `pack=default` to go back. You can also pick a pack on the Shader Pack row of the F8 panel. It lists the folders in `shaderpacks` and switches straight away.

If a shader doesn't compile, the mod keeps what was running and writes the error to `%LOCALAPPDATA%\mxs-shaders\mxs-shaders.log`.

Set `debug=1` in `mxs-shaders.ini` to save the game's own shaders to `%LOCALAPPDATA%\mxs-shaders\shaders`. They show what each stage receives. Don't publish those files, they belong to the game.

Each pack keeps its own slider values. To start yours somewhere other than the mod's defaults, add a `settings.ini` with a `[settings]` section and the keys from the settings table in the README, such as `dof=0`. What a player sets in the game takes priority.

## Game shaders

The mod recognises each game shader by what it contains, and a file named after that role replaces it. If neither your pack nor the default pack has the file, the game's own shader is used.

| Role | Files | Draws |
|---|---|---|
| Terrain | `terrain.vsh`, `terrain.fsh` | The ground |
| Models | `skinned.vsh`, `model.fsh` | Bikes, riders, trees and objects |
| Reflections | `skinned.vsh`, `reflection.fsh` | Reflective bike parts |
| Sprites | `sprite.vsh`, `sprite.fsh` | Sky, particles and the HUD |
| Decals | `decal.vsh`, `decal.fsh` | Marks on the ground |
| Tile map | `tilemap.vsh`, `tilemap.fsh` | Ground textures, drawn into the game's own framebuffer |

`#include "file"` on its own line pulls in another file from the pack, one level deep. The default pack keeps shared code in `common.glsl`, and everything `post_lens.fsh` does in `lens.glsl`, whose `lens_finish` the `psx` pack also calls so the settings keep working.

To change where vertices land without replacing a game vertex shader, add `terrain.vwrap`, `skinned.vwrap` or `decal.vwrap`. It goes after the game's shader, so it can use the game's variables, and it needs a `main()` that calls `game_main()`, which is the game's own `main()`, and then changes `gl_Position`. The `psx` pack shows how it's used. A `.vsh` for the same role takes priority.

## Post-processing

These full-screen passes run in order after the world is drawn. To skip an effect, replace its file with one that passes the image through. If any pass fails to compile, post-processing turns off, or on a reload the previous passes stay in place.

| File | Job |
|---|---|
| `post_ao.fsh`, `post_ao_blur.fsh` | Ambient occlusion and contact shadows, at half resolution |
| `post_shafts.fsh` | Sun shafts, at half resolution, blurred by `post_ao_blur.fsh` |
| `post_bloom_down.fsh`, `post_bloom_up.fsh` | Bloom |
| `post_exposure.fsh` | Brightness in red and focus distance in green, 1×1 pixel |
| `post_final.fsh` | Combines occlusion, atmosphere and bloom, still in linear light |
| `post_lens.fsh` | Motion blur, depth of field, tone mapping and grading |

The HUD and particles are drawn after these passes, so they aren't affected.

## What the mod provides

These uniforms only need declaring to use them.

| Uniform | Used in | Meaning |
|---|---|---|
| `u_shadow_pass` | terrain, model | Nonzero while drawing the shadow map, so only depth matters |
| `u_shadow_map`, `u_shadow_near_map` | world shaders | The wide and sharp sun shadow maps |
| `u_shadow_matrix`, `u_shadow_near_matrix` | world shaders | View space to each map |
| `u_shadow_on` | world shaders | Nonzero when the maps are valid |
| `u_hdr` | world shaders | Nonzero in a race, where post-processing finishes the image. Otherwise the shader must tone map itself |
| `u_night` | world shaders, `post_ao.fsh`, `post_final.fsh`, `post_lens.fsh` | The `time_of_day` setting, 1 at 100. Fade sun effects out with it. At 1 the shadow maps aren't drawn and `u_shadow_on` is 0 |
| `u_parallax` | terrain | The `parallax` setting |
| `u_view_up` | model | World up in view space |
| `u_sprite_mode` | sprite | 0 normal, 1 sky, 2 particle |
| `u_sun_view`, `u_screen` | sprite | Sun direction and screen size |
| `u_fog_amount`, `u_up`, `u_sun`, `u_camera_height` | `post_final.fsh` | The `fog` setting, world up, sun direction, camera height above the ground |
| `u_bloom_amount` | `post_final.fsh` | The `bloom` setting, 1 at 100. At 0 the bloom passes don't run, so `u_bloom` isn't updated |
| `u_motion`, `u_blur_near`, `u_reproject` | `post_lens.fsh` | Blur per frame of movement, sharp distance, this frame's view space to last frame's screen |
| `u_dof`, `u_saturation`, `u_contrast`, `u_split_tone` | `post_lens.fsh` | The settings, 1 at 100 |

Post passes also get `u_color`, `u_depth`, `u_ao`, `u_bloom`, `u_src`, `u_exposure` and `u_shafts` as textures, `u_proj_a` and `u_proj_b` to turn depth into distance, and `u_fx` and `u_fy` from the projection, in the passes that use them. `common.glsl` has the shadow lookups and tone mapping, and `post_shafts.fsh` shows how they're used.

World shaders should return early when `u_shadow_pass` is set, as the default pack does, because only depth matters in that pass.

## Things to watch for

- **The sprite shader also draws ground tiles.** The game uses it to paint terrain into its own framebuffer, not just the sky and HUD. Tie sky and particle effects to `u_sprite_mode`, which is 0 for those tiles, or they get baked into the ground as big rectangles.
- **Skinned models don't use `u_modelview` for their position.** On riders the bones hold the world position, and the modelview alone sits far from the bike. In `skinned.vsh`, do anything position-based on the final result, after the bones are applied.
- **Game units aren't metres.** The camera moves about 65 units per second at a displayed speed of about 39, and the wheelbase is about 4.4 units.
- **Shadow depth has a fixed scale.** `sun_shadow` in `common.glsl` assumes the map covers 1600 units of depth, which the mod sets. Bias values in your own lookups need to follow that scale.

## Sharing a pack

Publish your pack as a folder or zip in its own repository. Users drop it into `shaderpacks` and set `pack=`. Say which mod version it was written for, because uniforms can change between versions.

Packs change what players can see, so check what's allowed before using one in online races.
