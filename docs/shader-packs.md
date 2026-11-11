# Writing a shader pack

A shader pack is a folder of GLSL files that replaces how the game looks. You don't need to build anything: edit text, reload it from the game's settings panel, and see the result.

## Quick start

The paths below are for Windows. On Linux, `%LOCALAPPDATA%\MX Simulator\mxs-shaders` is `~/.local/share/mxs-shaders`, and everything else is the same, packs included.

1. Make a folder in `%LOCALAPPDATA%\MX Simulator\mxs-shaders\shaderpacks`, for example `my-pack`.
2. Set `pack=my-pack` and `debug=1` in `%LOCALAPPDATA%\MX Simulator\mxs-shaders\mxs-shaders.ini` and restart the game.
3. Copy the file you want to change into your folder. They're in `default-pack.zip` on the Releases page, or in `shaderpacks/default` in this repository.
4. Edit it, press **F8** in the game and click **Reload Current Shader**. The button only appears with `debug=1`.

A pack only needs the files it changes. Anything it doesn't have comes from the built-in default pack, including files it `#include`s.

To use a pack someone else made, put its folder in `shaderpacks`, set `pack=` to the folder's name, and restart. Set `pack=default` to go back. You can also pick a pack on the Shader Pack row of the F8 panel. It lists the folders in `shaderpacks` and switches straight away.

If a shader doesn't compile, the mod keeps what was running and writes the error to `%LOCALAPPDATA%\MX Simulator\mxs-shaders\mxs-shaders.log`.

Set `debug=1` in `mxs-shaders.ini` to save the game's own shaders to `%LOCALAPPDATA%\MX Simulator\mxs-shaders\shaders`. They show what each stage receives. Don't publish those files, they belong to the game.

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
| `post_sky.fsh` | The clear sky in every direction, 256×128, from `sky.glsl`. `sprite.fsh` draws it in the next frame's skybox |
| `post_clouds.fsh` | Volumetric clouds for a view a little wider than the screen, at the Cloud Quality's size, blended with the last frame. Light in rgb, how much of the sky behind shows in alpha. `sprite.fsh` draws them in the next frame's skybox |
| `post_final.fsh` | Combines the sun shadow, occlusion, atmosphere and bloom, still in linear light |
| `post_lens.fsh` | Motion blur, depth of field, tone mapping and grading |

The HUD and particles are drawn after these passes, so they aren't affected.

## What the mod provides

These uniforms only need declaring to use them.

| Uniform | Used in | Meaning |
|---|---|---|
| `u_shadow_pass` | terrain, model | Nonzero while drawing the shadow map, so only depth matters |
| `u_shadow_map`, `u_shadow_near_map` | `post_final.fsh`, `post_shafts.fsh` | The wide and sharp sun shadow maps |
| `u_shadow_matrix`, `u_shadow_near_matrix` | `post_final.fsh`, `post_shafts.fsh` | View space to each map, for this frame's own map |
| `u_shadow_on` | `post_final.fsh`, `post_shafts.fsh`, and world shaders when post-processing is off | Nonzero when the maps are valid. With post-processing on, world shaders get 0: the mod applies the sun shadow in `post_final.fsh`, from a map drawn at the end of the frame, so it is the same frame. Without a post pass (`msaa` under 2) the maps go to the world shaders instead and this is 1 |
| `u_hdr` | world shaders | Nonzero in a race, where post-processing finishes the image. Otherwise the shader must tone map itself |
| `u_night` | world shaders, `post_ao.fsh`, `post_final.fsh`, `post_lens.fsh` | 0 by day, rising through twilight to 1 when the sun is 12 degrees down, from the Time of Day setting. For stars and the like: the shadow maps keep running at night, lit by the moon |
| `u_direct_loss`, `u_ambient_loss` | world shaders, `post_final.fsh` | How much of the game's sun and sky light the Time of Day takes away, per colour. 0 leaves the game's light. `common.glsl`'s `sun_boost()`, `light_tint()` and `sky_light()` apply them, so a pack that uses those gets dusk and night for free |
| `u_sky`, `u_sky_mask`, `u_sky_sun` | sprite, `post_final.fsh` | The clear sky in every direction, the skybox's skyline, and the real sun in world space. `sky.glsl` reads them |
| `u_cloud_cover` | world shaders, post passes | The Cloud Cover setting, 1 at 100. `common.glsl`'s `cloud_cover()` raises it to an overcast in rain and snow, and `cloud_density()` is the cloud field the cloud shadows and the volumetric clouds share |
| `u_clouds`, `u_cloud_view`, `u_cloud_scale` | sprite | The last volumetric clouds, the view they were drawn from, and its scale, with z 0 when there are none. `sky.glsl`'s `clouds_at()` reads them by direction |
| `u_sky_face_on`, `u_sky_face` | sprite | Set for each skybox face once the skybox has been read: the face's corners and texture span, so `sprite.fsh` can draw the atmosphere above the skyline |
| `u_sun_strength`, `u_sun_height` | world shaders | The Sun Strength setting, 1 at 100, and the sun's world height, 0 at the horizon and negative at night. `common.glsl`'s `sun_boost()`, `sun_power()` and `sky_light()` apply them: 100 is the pack's tuned sun, and the sky's fill is trimmed as the sun climbs |
| `u_parallax` | terrain | The `parallax` setting |
| `u_view_up` | model | World up in view space |
| `u_sprite_mode` | sprite | 0 normal, 1 sky, 2 particle |
| `u_sun_view`, `u_screen` | sprite | Sun direction and screen size |
| `u_fog_amount`, `u_up`, `u_sun`, `u_camera_height` | `post_final.fsh` | The `fog` setting, world up, sun direction, camera height above the ground |
| `u_bloom_amount` | `post_final.fsh` | The `bloom` setting, 1 at 100. At 0 the bloom passes don't run, so `u_bloom` isn't updated |
| `u_shadow_strength` | `post_final.fsh` | The `shadow_strength` setting, 1 at 100: how much of a pixel's colour a sun shadow takes away |
| `u_motion`, `u_blur_near`, `u_reproject` | `post_lens.fsh` | Blur per frame of movement, sharp distance, this frame's view space to last frame's screen |
| `u_dof`, `u_saturation`, `u_contrast`, `u_split_tone` | `post_lens.fsh` | The settings, 1 at 100 |

Post passes also get `u_color`, `u_depth`, `u_ao`, `u_bloom`, `u_src`, `u_exposure` and `u_shafts` as textures, `u_proj_a` and `u_proj_b` to turn depth into distance, and `u_fx` and `u_fy` from the projection, in the passes that use them. `common.glsl` has the shadow lookups and tone mapping, `post_final.fsh` uses them for the sun shadow and `post_shafts.fsh` for the beams.

World shaders should return early when `u_shadow_pass` is set, as the default pack does, because only depth matters in that pass.

## Things to watch for

- **The sprite shader also draws ground tiles.** The game uses it to paint terrain into its own framebuffer, not just the sky and HUD. Tie sky and particle effects to `u_sprite_mode`, which is 0 for those tiles, or they get baked into the ground as big rectangles.
- **Skinned models don't use `u_modelview` for their position.** On riders the bones hold the world position, and the modelview alone sits far from the bike. In `skinned.vsh`, do anything position-based on the final result, after the bones are applied.
- **Game units aren't metres.** The camera moves about 65 units per second at a displayed speed of about 39, and the wheelbase is about 4.4 units.
- **Shadow depth has a fixed scale.** `sun_shadow` in `common.glsl` assumes the map covers 1600 units of depth, which the mod sets. Bias values in your own lookups need to follow that scale.

## Sharing a pack

Publish your pack as a folder or zip in its own repository. Users drop it into `shaderpacks` and set `pack=`. Say which mod version it was written for, because uniforms can change between versions.

Packs change what players can see, so check what's allowed before using one in online races.
