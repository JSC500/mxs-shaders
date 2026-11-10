#version 150

/* The scene in linear light, and it leaves that way: post_lens.fsh finishes it. */

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_color, u_ao, u_bloom, u_depth, u_shafts;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;
uniform float u_camera_height;	/* above the ground around the rider */
uniform float u_fog_amount;	/* the fog setting in mxs-shaders.ini, 1 at 100 */
uniform float u_sharpen, u_bloom_amount;	/* the settings, 1 at 100 */
uniform vec3 u_up, u_sun, u_fog_color;	/* u_sun is where the sun really is, in view space */
uniform vec3 u_light;		/* the light the scene is shaded with, the moon at night, in view space */
uniform int u_sky_mask_on;	/* 0 until the skybox has been read */
uniform int u_sky_mask_view;	/* debugging: sky_mask_view in mxs-shaders.ini */
uniform vec3 u_moon;		/* world space, below the horizon too */
uniform int u_clouds_on;	/* post_clouds.fsh drew this frame's clouds, the sky already has them */
uniform float u_cloud_margin;	/* how much wider than the screen they were drawn */

#include "common.glsl"
#include "sky.glsl"

const float AO_STRENGTH = 0.85;
const float SHADOW_FLOOR = 0.45;	/* what full shade keeps of a pixel's colour */
const float CONTACT_STRENGTH = 0.6;
const float BLOOM_STRENGTH = 0.12;


/* Haze per game unit at ground level, thinning by e every FOG_HEIGHT units. */
const float FOG_DENSITY = 0.0002;
const float FOG_HEIGHT = 14.0;
const float SKY_DISTANCE = 900.0;	/* how much air a ray to the sky goes through */
const float FOG_MAX_BRIGHT = 0.85;	/* gamma encoded, near white would undo to a huge value */
const float FOG_MAX_DESCENT = 2.0;	/* in FOG_HEIGHTs, the air stops thickening below that */

const vec3 SUN_SCATTER = vec3(1.0, 0.82, 0.6);
const float FOG_SUN = 0.6;	/* how much the haze brightens toward the sun */
const float SHAFT_STRENGTH = 0.35;
const float SHAFT_AMBIENT = 0.12;	/* beams away from the sun, nearly invisible */

/* At 100, the most a pixel's neighbours take away. Above 0.25 the result would flip. */
const float SHARPEN_PEAK = 0.09;

/*
 Contrast adaptive sharpening: each pixel pushes away from its four neighbours,
 less where they already differ a lot, so edges don't ring and flat areas stay
 clean. The min/max ratio works the same at any HDR brightness.
*/
vec3 sharpen(vec3 c)
{
	vec2 t = 1.0 / vec2(textureSize(u_color, 0));
	vec3 n = texture(u_color, v_uv + vec2(0.0, t.y)).rgb;
	vec3 s = texture(u_color, v_uv - vec2(0.0, t.y)).rgb;
	vec3 e = texture(u_color, v_uv + vec2(t.x, 0.0)).rgb;
	vec3 w = texture(u_color, v_uv - vec2(t.x, 0.0)).rgb;
	vec3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
	vec3 weight = -sqrt(clamp(lo / max(hi, vec3(0.0001)), 0.0, 1.0)) * min(SHARPEN_PEAK * u_sharpen, 0.2);

	return max((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), vec3(0.0));
}

/* Rain greys the haze and thickens it, snow whitens it. Gamma encoded like the game's fog. */
const vec3 RAIN_FOG = vec3(0.55, 0.58, 0.62), SNOW_FOG = vec3(0.82, 0.85, 0.9);
const float RAIN_HAZE = 3.0, SNOW_HAZE = 4.0;	/* times the haze, at full strength */

const float STAR_GRID = 350.0;		/* cells per radian */
const float STAR_DENSITY = 0.004;	/* of the cells that hold a star */
const float STAR_BRIGHT = 2.0;
const float MOON_RADIUS = 0.016, MOON_BRIGHT = 4.0;	/* larger than the real one, which reads as a dot */

const float LIGHTNING_EVERY = 8.0;	/* seconds, each has one chance of a strike */
const float LIGHTNING_BRIGHT = 1.5;
const float LIGHTNING_TOP = 0.35;	/* how high the bolt reaches, as the sine of its angle */

const float RAIN_SPEED = 55.0, SNOW_SPEED = 3.0;	/* units per second */
const float RAIN_SLANT = 0.12;
const vec3 RAIN_LIGHT = vec3(0.12), SNOW_FLAKE = vec3(0.9);

vec3 weather_fog_color()
{
	if (u_weather == WEATHER_SNOW)
		return mix(u_fog_color, SNOW_FOG, u_weather_amount);
	return mix(u_fog_color, RAIN_FOG, weather_gloom() * 0.8);
}

float weather_haze()
{
	if (u_weather == WEATHER_SNOW)
		return mix(1.0, SNOW_HAZE, u_weather_amount);
	if (u_weather >= WEATHER_RAIN)
		return mix(1.0, RAIN_HAZE, u_weather_amount);
	return 1.0;
}

/* How bright the lightning is right now. slot says which strike it is. */
float lightning(out float slot)
{
	float t;

	slot = floor(u_time / LIGHTNING_EVERY);
	if (u_weather != WEATHER_STORM || weather_hash(vec2(slot, 1.7)) > 0.25 + 0.6 * u_weather_amount)
		return 0.0;
	t = u_time - slot * LIGHTNING_EVERY - weather_hash(vec2(slot, 4.3)) * (LIGHTNING_EVERY - 1.0);
	if (t < 0.0 || t > 1.0)
		return 0.0;
	/* A first flash, then a second one a moment later. */
	return exp(-t * 12.0) + step(0.15, t) * exp(-(t - 0.15) * 9.0) * 0.7;
}

/* The bolt, a jagged line down from the clouds at a bearing of its own. */
float lightning_bolt(vec3 dir, float slot)
{
	float bearing = weather_hash(vec2(slot, 8.1)) * 2.0 * PI, off, a;

	if (dir.y < 0.0 || dir.y > LIGHTNING_TOP)
		return 0.0;
	off = (weather_noise(vec2(dir.y * 18.0, slot)) - 0.5) * 0.12 + (weather_noise(vec2(dir.y * 70.0, slot + 3.0)) - 0.5) * 0.03;
	a = mod(atan(dir.z, dir.x) - bearing - off + PI, 2.0 * PI) - PI;
	return smoothstep(0.0045, 0.0015, abs(a)) + smoothstep(0.05, 0.0, abs(a)) * 0.15;
}

/*
 The sky behind the scene: clouds, the overcast of bad weather, and at night stars and a moon.
 open is how much of the pixel is sky, not scenery painted into the skybox.
*/
vec3 weather_sky(vec3 c, vec3 dir, float flash, float slot, float open)
{
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722)), gloom = weather_gloom(), density = 0.0;
	float night = smoothstep(0.5, 0.9, u_night) * smoothstep(0.0, 0.08, dir.y) * open;
	vec3 camera = to_world(vec3(0.0));

	/* Volumetric clouds are in the sky already, what they let through hides what's behind. */
	if (u_clouds_on != 0)
		density = (1.0 - texture(u_clouds, (v_uv - 0.5) / u_cloud_margin + 0.5).a) * open;
	else if (dir.y > 0.0)
		density = cloud_density(camera.xz + dir.xz / max(dir.y, 0.02) * (CLOUD_HEIGHT - camera.y)) * smoothstep(0.0, 0.12, dir.y) * open;
	if (u_clouds_on == 0)
		c = mix(c, vec3(l) * mix(1.15, 0.4, gloom), density * 0.9);
	c = mix(c, vec3(l) * mix(1.0, 0.55, gloom), gloom * 0.5);

	if (night > 0.0) {
		vec2 a = vec2(atan(dir.z, dir.x), asin(clamp(dir.y, -1.0, 1.0))) * STAR_GRID;
		vec2 cell = floor(a);
		vec2 f = fract(a) - 0.5 - (vec2(weather_hash(cell + 5.0), weather_hash(cell + 9.0)) - 0.5) * 0.6;
		float h = weather_hash(cell), star;

		star = step(1.0 - STAR_DENSITY, h) * smoothstep(0.25, 0.0, length(f)) * (0.4 + 0.6 * fract(h * 997.0));
		c += vec3(star * STAR_BRIGHT) * night * (1.0 - density);
	}

	/* The moon shows from dusk, before the stars. */
	if (u_night > 0.0 && dir.y > 0.0) {
		float angle = acos(clamp(dot(dir, normalize(u_moon)), -1.0, 1.0)), moon;

		moon = (1.0 - smoothstep(MOON_RADIUS * 0.9, MOON_RADIUS, angle)) * (0.75 + 0.25 * weather_fbm(dir.xz * 400.0)) * MOON_BRIGHT;
		moon += exp(-angle / 0.08) * 0.15;
		c += vec3(0.85, 0.9, 1.0) * moon * smoothstep(0.1, 0.5, u_night) * open * (1.0 - density);
	}

	if (flash > 0.0)
		c += vec3(0.8, 0.85, 1.0) * lightning_bolt(dir, slot) * flash * 20.0 * open * (1.0 - density);
	return c;
}

/* Falling rain or snow in layers at set distances, each hidden by anything nearer. */
vec3 precipitation(vec3 c, vec3 dir, float dist, float flash)
{
	const float layer_dist[3] = float[3](3.0, 7.0, 16.0);
	bool snow = u_weather == WEATHER_SNOW;
	vec2 cell_size = snow ? vec2(0.5) : vec2(0.3, 2.0);
	float around = atan(dir.z, dir.x), rise = dir.y / max(length(dir.xz), 0.05), amount = 0.0;
	int i;

	if (u_weather < WEATHER_RAIN || u_weather_amount <= 0.0)
		return c;
	for (i = 0; i < 3 && dist > layer_dist[i]; i++) {
		/* A cylinder around the camera, so drops are the same size in game units at each layer. */
		vec2 p = vec2(around, rise) * layer_dist[i], cell, f;
		float x, drop;

		if (snow) {
			p.y += u_time * SNOW_SPEED;
			p.x += sin(u_time * 0.9 + p.y * 0.8) * 0.25;
		} else {
			p.y += u_time * RAIN_SPEED;
			p.x += p.y * RAIN_SLANT;
		}
		p.y += float(i) * 37.0;
		cell = floor(p / cell_size);
		f = fract(p / cell_size);
		if (weather_hash(cell + float(i) * 13.0) > u_weather_amount * (snow ? 0.5 : 0.35))
			continue;
		x = 0.2 + 0.6 * weather_hash(cell + 2.7);
		if (snow)
			drop = smoothstep(0.24, 0.1, length(f - vec2(x, 0.5)));
		else
			drop = smoothstep(0.06, 0.0, abs(f.x - x)) * smoothstep(0.0, 0.15, f.y) * smoothstep(0.55, 0.3, f.y);
		amount += drop / (1.0 + float(i) * 0.6);
	}
	if (snow)
		return mix(c, SNOW_FLAKE * (1.0 - 0.8 * u_night), min(amount, 1.0) * 0.85);
	return c + RAIN_LIGHT * amount * (1.0 + flash * 4.0);
}

/*
 How much of a pixel at depth 1 is open sky, not scenery painted into the skybox.
 It's worked out for every pixel, because fwidth needs its neighbours.
*/
float sky_open(vec3 dir)
{
	return u_sky_mask_on != 0 ? skyline_open(dir) : 1.0;
}

vec3 atmosphere(vec3 c)
{
	float depth = texture(u_depth, v_uv).r;
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 ray = vec3(ndc.x / u_fx, ndc.y / u_fy, -1.0);
	float dist = SKY_DISTANCE * length(ray);
	vec3 dir = normalize(ray);
	vec2 shafts = texture(u_shafts, v_uv).rg;
	float climb, thinning, optical, sun, clouds = sun_through_clouds(u_light);
	vec3 light = light_tint();	/* the haze glows with the light that's there, orange low down, faint from the moon */

	if (depth < 1.0)
		dist = u_proj_b / (depth * 2.0 - 1.0 + u_proj_a) * length(ray);

	/* Density falls with height, so this is its exact integral along the ray. */
	climb = max(dot(dir, normalize(u_up)) * dist / FOG_HEIGHT, -FOG_MAX_DESCENT);
	thinning = abs(climb) < 0.001 ? 1.0 : (1.0 - exp(-climb)) / climb;
	optical = FOG_DENSITY * u_fog_amount * weather_haze() * exp(-u_camera_height / FOG_HEIGHT) * dist * thinning;

	sun = pow(max(dot(dir, normalize(u_light)), 0.0), 8.0) * clouds;
	c = mix(scene_from_gamma(min(weather_fog_color(), vec3(FOG_MAX_BRIGHT))) * (1.0 - u_ambient_loss) + SUN_SCATTER * light * sun * FOG_SUN, c, exp(-optical));
	return c + SUN_SCATTER * light * shafts.r * shafts.g * (SHAFT_AMBIENT + sun) * SHAFT_STRENGTH * clouds;
}

void main()
{
	vec3 c = texture(u_color, v_uv).rgb;
	vec2 occlusion = pow(texture(u_ao, v_uv).rg, vec2(2.2));
	float depth = texture(u_depth, v_uv).r, slot, flash = lightning(slot);
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 ray = vec3(ndc.x / u_fx, ndc.y / u_fy, -1.0);
	vec3 dir = normalize(to_world_dir(ray));
	float dist = depth < 1.0 ? u_proj_b / (depth * 2.0 - 1.0 + u_proj_a) * length(ray) : 1e6;
	float open = sky_open(dir);

	if (u_sharpen > 0.0)
		c = sharpen(c);
	c *= mix(1.0, occlusion.r, AO_STRENGTH);
	/* Contact shadows only take away direct light, which is gone after sunset. */
	c *= mix(1.0, occlusion.g, CONTACT_STRENGTH * clamp(dot(light_tint(), vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0));
	/*
	 The sun's shadow comes from the map drawn at the end of this frame, from this
	 frame's own draw calls, so it is never a frame behind the bike. The surface and its
	 normal are rebuilt from the depth buffer, in view space: that is what the map's
	 matrix (the same one the game's shaders used to get) takes. At night the light is
	 the moon's, which the map is lit by too.

	 Only the direct light goes: shade still has the sky's light, which at dawn is all
	 there is. Scaling the whole colour turned every shadow into a black hole, where the
	 game's shaders had only ever scaled their sun term, so shade keeps a floor of it.
	 The floor is a constant and not the surface's own facing, which the last version
	 scaled by: this normal is flat across each triangle, because it is rebuilt from the
	 depth buffer, so scaling by it drew those triangles across the jersey and the ground,
	 and a surface turned away from the sun lost its shadow instead of darkening.
	*/
	if (depth < 1.0 && u_shadow_on != 0) {
		vec3 vpos = ray * (u_proj_b / (depth * 2.0 - 1.0 + u_proj_a));
		vec3 n = normalize(cross(dFdx(vpos), dFdy(vpos)));
		float lost;

		if (dot(n, vpos) > 0.0)
			n = -n;	/* the lookup pushes along it, so it has to face the camera */
		/*
		 This surface is rebuilt from a 24-bit depth buffer, so it sits up to this many game
		 units from the one that was drawn: one step of the buffer covers this much distance
		 here, taken twice over. The lookup has to allow for it. Left out, ground in the far
		 map counts as its own caster, faintly at 4096 and as full shade at 8192.
		*/
		lost = 2.0 * dist * dist / u_proj_b * (1.0 / 8388608.0);
		c *= mix(SHADOW_FLOOR, 1.0, sun_shadow_depth(vpos, n, lost));
	}
	/* The atmosphere itself is already there: sprite.fsh drew it in the game's own sky. */
	if (depth >= 1.0) {
		c = weather_sky(c, dir, flash, slot, open);
		/* Magenta is open sky, yellow means the skybox hasn't been read. */
		if (u_sky_mask_view != 0)
			c = mix(c, u_sky_mask_on != 0 ? vec3(1.0, 0.0, 1.0) : vec3(1.0, 1.0, 0.0), open * 0.6);
	}
	c = atmosphere(c);
	c = precipitation(c, dir, dist, flash);
	c *= 1.0 + flash * LIGHTNING_BRIGHT;
	if (u_bloom_amount > 0.0)
		c += texture(u_bloom, v_uv).rgb * BLOOM_STRENGTH * u_bloom_amount;

	f_color = vec4(c, 1.0);
}
