/*
 Shared by the default pack. The game's lighting works directly on gamma
 encoded values; these helpers let us light in linear space and go back.
*/
const float EXPOSURE = 1.15;
const float PI = 3.14159265;

/* Linear space shrinks the game's ambient far more than its sun, so restore the balance. */
const float AMBIENT_BOOST = 3.2;
const float SUN_BOOST = 1.3;		/* at Sun Strength 200, below the 1.7 this pack had */
const float SUN_BOOST_OLD = 1.7;	/* what that was, so the sheen keeps its share of the sun */
const float SKY_HIGH_TRIM = 0.45;	/* of the sky's fill left under an overhead sun */
const float SATURATION = 0.88;

uniform float u_night;	/* 0 by day, 1 once twilight has gone */

/*
 The Sun Strength setting, 0 to 2, 1 at 200: the direct sun and the sheen that comes off
 the ground and the rider. The scale is doubled, so 200 is the tuned sun, dimmer than the
 pack's old look, which sits above it on the slider, and the default 100 is half of it.
*/
uniform float u_sun_strength;

/* The sun's world height: about 1 overhead, 0 at the horizon, negative at night. */
uniform float u_sun_height;

/*
 How much of the game's sun and sky light the Time of Day setting takes away: a low sun
 shines orange and weak, at night the light is the moon's. 0 leaves the game's light.
*/
uniform vec3 u_direct_loss, u_ambient_loss;

/* The game's sun as the time of day leaves it. */
vec3 light_tint()
{
	return 1.0 - u_direct_loss;
}

/* The sun for the specular sheen, which shares in the setting but not the boost's value. */
vec3 sun_power()
{
	return light_tint() * u_sun_strength * (SUN_BOOST / SUN_BOOST_OLD);
}

vec3 sun_boost()
{
	return SUN_BOOST * u_sun_strength * light_tint();
}

/*
 With little direct light the sky still lights from one side: the glow where the sun
 went down, or the moon. Without that, ruts and bumps vanish at dusk and at night. It
 grows as the direct light fades, slopes toward the light brighten and away darken,
 and flat ground keeps its light. light is the game's, the moon's at night.
*/
const float SKY_RELIEF = 0.5;

float sky_relief(vec3 n, vec3 up, vec3 light)
{
	float amount = SKY_RELIEF * (1.0 - clamp(dot(light_tint(), vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0));

	return max(1.0 + amount * 2.0 * (dot(n, light) - dot(up, light)), 0.2);
}

vec3 to_linear(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(2.2));
}

vec3 to_gamma(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

/*
 The sun's climb from a low sun to overhead, 0 to 1. The sky's fill is a flat floor and the
 sun is strongest up there, so the two together are what flattens the ground at noon.
*/
float sun_high()
{
	return smoothstep(0.6, 0.87, u_sun_height);
}

/* Squaring a tinted color exaggerates the tint, which turns full shade purple. */
vec3 sky_light(vec3 ambient)
{
	vec3 c = to_linear(ambient) * AMBIENT_BOOST * mix(1.0, SKY_HIGH_TRIM, sun_high());

	return mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, 0.5) * (1.0 - u_ambient_loss);
}

/* Narkowicz's fit of the ACES filmic curve. */
vec3 tonemap(vec3 c)
{
	/* The curve boosts saturation per channel, so take some out first. */
	c = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, SATURATION);
	c *= EXPOSURE * 0.6;
	return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
}

/*
 Exactly undoes tonemap() and gamma, so something already finished, like the sky,
 can pass through the HDR buffer and come out as it went in. Very bright values
 are limited, the curve is nearly flat up there and they would become huge.
*/
vec3 scene_from_gamma(vec3 color)
{
	vec3 y = clamp(to_linear(color), vec3(0.0), vec3(0.97));
	vec3 a = 2.43 * y - 2.51, b = 0.59 * y - 0.03, c = 0.14 * y;
	vec3 x = (-b - sqrt(b * b - 4.0 * a * c)) / (2.0 * a) / (EXPOSURE * 0.6);
	float l = dot(x, vec3(0.2126, 0.7152, 0.0722));

	return max(vec3(l) + (x - vec3(l)) / SATURATION, vec3(0.0));
}

/* Normalized Blinn-Phong, so tight highlights get brighter instead of just smaller. */
float specular(vec3 n, vec3 viewer, vec3 light, float power)
{
	vec3 h = normalize(light - normalize(viewer));

	return (power + 8.0) / (8.0 * PI) * pow(max(dot(n, h), 0.0), power);
}

float fresnel(vec3 n, vec3 viewer, float f0)
{
	float c = 1.0 - max(dot(n, -normalize(viewer)), 0.0);

	return f0 + (1.0 - f0) * c * c * c * c * c;
}

/*
 Sun shadows from the map the mod renders. Takes the surface position in view
 space and returns 0 in full shadow, 1 in full sun.
*/
uniform sampler2DShadow u_shadow_map, u_shadow_near_map;
uniform mat4 u_shadow_matrix, u_shadow_near_matrix;
uniform int u_shadow_on;
uniform int u_shadow_pass;	/* set while the mod renders the map: only depth matters */

float shadow_lookup(sampler2DShadow map, vec3 c, float radius, float bias)
{
	/* Rotated per pixel so banding turns into fine noise. */
	const vec2 disk[8] = vec2[8](
		vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
		vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893));
	float angle, sum = 0.0;
	vec2 r;
	int i;

	angle = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 2.0 * PI;
	r = vec2(cos(angle), sin(angle));
	for (i = 0; i < 8; i++) {
		vec2 d = vec2(disk[i].x * r.x - disk[i].y * r.y, disk[i].x * r.y + disk[i].y * r.x);

		sum += texture(map, vec3(c.xy + d * radius, c.z - bias));
	}
	return sum / 8.0;
}

vec3 shadow_coord(mat4 matrix, vec3 pos)
{
	return (matrix * vec4(pos, 1.0)).xyz * 0.5 + 0.5;
}/*
 The depth a sample is compared at has to clear how far its filter reaches across a surface
 tilted from the map, and how far the surface's own position may be off. Both put samples
 deeper than the surface they belong to, which reads as shadow.
*/
float shadow_depth_bias(float spread, vec3 map_z, vec3 n)
{
	const float depth_per_meter = 1.0 / 1600.0;	/* SHADOW_DEPTH_RANGE * 2 in the mod */
	float tilt = max(abs(dot(n, map_z)), 0.25);

	return spread * sqrt(1.0 - tilt * tilt) / tilt * depth_per_meter;
}

/*
 pos_error is how far the surface's position is off, in game units, when it was rebuilt
 from a depth buffer rather than interpolated: 0 where the shader has the real thing.
*/
float sun_shadow_at(vec3 pos, vec3 n, float ignore_meters, float pos_error)
{
	float bias = 0.00002 + ignore_meters / 1600.0;
	float edge, far_edge, spread, far_bias, near_radius, near_spread, inset, near_shadow = 1.0, far_shadow = 1.0;
	vec3 near_c, far_c, map_x, map_z, near_x;

	if (u_shadow_on == 0)
		return 1.0;

	/* Pushing along the normal keeps surfaces from shadowing themselves. */
	near_c = shadow_coord(u_shadow_near_matrix, pos + n * 0.025);
	far_c = shadow_coord(u_shadow_matrix, pos + n * 0.06);

	/* The matrices' rows hold each map's scale and the sun's direction. */
	map_x = vec3(u_shadow_matrix[0][0], u_shadow_matrix[1][0], u_shadow_matrix[2][0]);
	map_z = normalize(vec3(u_shadow_matrix[0][2], u_shadow_matrix[1][2], u_shadow_matrix[2][2]));
	near_x = vec3(u_shadow_near_matrix[0][0], u_shadow_near_matrix[1][0], u_shadow_near_matrix[2][0]);

	/*
	 How far the wide map's filter reaches across the ground, and never less than this. What
	 the map is compared against is off by more than its texels say: the surface comes from
	 the depth buffer, and terrain can be filled for the map at a coarser mesh than the game
	 drew. Left to the texels alone, Ultra's bias fell under that and the track went to shade.
	*/
	spread = max(2.5 / float(textureSize(u_shadow_map, 0).x) * 2.0 / length(map_x), 0.75);
	far_bias = bias + shadow_depth_bias(spread + pos_error, map_z, n);

	/*
	 Sharp map close by, fading to the wide one in the last few game units before its border.
	 The band is a fixed width rather than a share of the map, or a longer reach would spend
	 most of itself fading and look no sharper than the map it hands over to. Its filter
	 widens across the band to the wide map's spread in world units, and its bias grows to the
	 wide map's at the same rate: two maps meeting at different softnesses, or at shadows
	 pushed different distances from their casters, show the change as a square edge across
	 the ground, worst when riding at speed.
	*/
	inset = min(min(near_c.x, 1.0 - near_c.x), min(near_c.y, 1.0 - near_c.y)) * 2.0 / length(near_x);
	edge = 1.0 - smoothstep(0.0, 4.0, inset);
	if (edge < 1.0) {
		near_radius = mix(4.0 / float(textureSize(u_shadow_near_map, 0).x),
				  2.5 / float(textureSize(u_shadow_map, 0).x) * length(near_x) / length(map_x),
				  edge * edge);
		/*
		 Its own texels are what it is compared against as well, and a longer reach makes them
		 larger: without the tilt term the rider's surface counted as its own caster, and the
		 jersey wore the map's grid as flat squares.
		*/
		near_spread = near_radius * 2.0 / length(near_x);
		near_shadow = shadow_lookup(u_shadow_near_map, near_c, near_radius,
					    mix(bias, far_bias, edge * edge) +
					    shadow_depth_bias(near_spread + pos_error, map_z, n));
	}
	/* The wide map fades out before its border, so its end isn't a visible line. */
	far_edge = max(abs(far_c.x - 0.5), abs(far_c.y - 0.5)) * 2.0;
	if (edge > 0.0 && far_edge < 1.0)
		far_shadow = mix(shadow_lookup(u_shadow_map, far_c, 2.5 / float(textureSize(u_shadow_map, 0).x), far_bias),
				 1.0, smoothstep(0.85, 1.0, far_edge));

	return mix(near_shadow, far_shadow, edge);
}

float sun_shadow(vec3 pos, vec3 n, float ignore_meters)
{
	return sun_shadow_at(pos, n, ignore_meters, 0.0);
}

/* sun_shadow for a surface rebuilt from a depth buffer, which is pos_error units out. */
float sun_shadow_depth(vec3 pos, vec3 n, float pos_error)
{
	return sun_shadow_at(pos, n, 0.0, pos_error);
}

/* One lookup is enough for the soft shapes of dust. */
float sun_shadow_fast(vec3 pos)
{
	vec3 c;

	if (u_shadow_on == 0)
		return 1.0;
	c = shadow_coord(u_shadow_matrix, pos);
	if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0)
		return 1.0;
	return texture(u_shadow_map, vec3(c.xy, c.z - 0.0001));
}

/*
 Fog is applied last on the gamma encoded result, as in the game's shaders, so
 distant terrain still blends into the unchanged sky.
*/
vec3 apply_fog(vec3 c, vec3 fog_color, float density);

/* In a race the frame is finished in post, anywhere else the shader does it itself. */
uniform int u_hdr;

vec3 finish_color(vec3 lit, vec3 fog_color, float density)
{
	/* The haze is lit by the sky, so it darkens with it. */
	if (u_hdr != 0)
		return apply_fog(lit, scene_from_gamma(fog_color) * (1.0 - u_ambient_loss), density);
	return apply_fog(to_gamma(tonemap(lit)), fog_color, density);
}

vec3 apply_fog(vec3 c, vec3 fog_color, float density)
{
	float z = gl_FragCoord.z / gl_FragCoord.w;

	return mix(fog_color, c, min(exp(-density * z), 1.0));
}

/*
 Weather. u_world_from_view takes view space to the world, where y is up, so
 clouds, puddles and snow stay put on the track as the camera moves.
*/
uniform mat4 u_world_from_view;
uniform float u_time;		/* seconds, for drifting clouds and falling rain */
uniform int u_weather;
uniform float u_weather_amount;	/* the Weather Strength setting, 1 at 100 */
uniform float u_wet;		/* 0 dry to 1 soaked, from Wet Ground or the rain */
uniform sampler2D u_weather_noise;	/* four octaves of tiling value noise, made by the mod */
uniform float u_cloud_cover;	/* the Cloud Cover setting, 1 at 100 */

/* 1 was Cloudy, now Cloud Cover. Its number stays unused so old settings read the same. */
const int WEATHER_CLEAR = 0, WEATHER_RAIN = 2, WEATHER_STORM = 3, WEATHER_SNOW = 4;
const vec2 WIND = vec2(9.0, 4.0);	/* game units per second */
const float CLOUD_BASE = 2500.0, CLOUD_TOP = 5000.0;	/* the cumulus layer, game units */
const float CLOUD_HEIGHT = 3200.0;	/* where its shadows and the flat clouds are worked out */
const float CLOUD_SIZE = 2500.0;	/* across a typical cloud */
const float CLOUD_OPACITY = 0.85;	/* of the sun a thick cloud takes away */
const float PUDDLE_SIZE = 18.0;
const float PUDDLE_GLOOM = 0.55;	/* of the sky a puddle mirrors under a fully grey day */
const float SNOW_PATCH_SIZE = 40.0;
const vec3 SNOW_ALBEDO = vec3(0.8, 0.83, 0.88);
const float NOISE_CELLS = 8.0;	/* the noise texture's lowest octave cells across, match WEATHER_NOISE_CELLS */

float weather_hash(vec2 p)
{
	vec3 q = fract(p.xyx * vec3(0.1031, 0.1030, 0.0973));

	q += dot(q, q.yzx + 33.33);
	return fract((q.x + q.y) * q.z);
}

float weather_noise(vec2 p)
{
	vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);

	return mix(mix(weather_hash(i), weather_hash(i + vec2(1.0, 0.0)), u.x),
		   mix(weather_hash(i + vec2(0.0, 1.0)), weather_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

/* Four octaves from 0 to 1, p in lowest octave cells. One read, the mod built the sum once. */
float weather_fbm(vec2 p)
{
	return texture(u_weather_noise, p / NOISE_CELLS).r;
}

vec3 to_world(vec3 view_pos)
{
	return (u_world_from_view * vec4(view_pos, 1.0)).xyz;
}

vec3 to_world_dir(vec3 view_dir)
{
	return mat3(u_world_from_view) * view_dir;
}

/* How much of the sky the clouds cover. Rain and snow need at least an overcast. */
float cloud_cover()
{
	if (u_weather >= WEATHER_RAIN)
		return max(u_cloud_cover, 0.75 + 0.25 * u_weather_amount);
	return u_cloud_cover;
}

/*
 How thick the cloud is at a point of its layer, 0 in clear sky. Two scales of noise at
 odd sizes, so the pattern doesn't visibly repeat across the sky, and a soft edge.
*/
float cloud_density(vec2 layer)
{
	float cover = cloud_cover(), field;
	vec2 p = (layer + WIND * u_time) / CLOUD_SIZE;

	if (cover <= 0.0)
		return 0.0;
	field = weather_fbm(p) * 0.65 + weather_fbm(p * 0.183 + vec2(3.7, 1.3)) * 0.35;
	return smoothstep(1.0 - cover, 1.0 - cover + 0.35, field);
}

/* The sun's world direction, pointing up whichever way the game's vector runs. */
vec3 sun_world(vec3 view_sun)
{
	vec3 sun = normalize(to_world_dir(view_sun));

	return sun.y < 0.0 ? -sun : sun;
}

/* How much of the sun a thick cloud takes away. An overcast closes into one sheet that lets almost none through. */
float cloud_opacity()
{
	return mix(CLOUD_OPACITY, 0.98, smoothstep(0.7, 1.0, cloud_cover()));
}

/* The sunlight a point gets through the clouds between it and the sun. */
float cloud_shadow(vec3 view_pos, vec3 view_sun)
{
	vec3 p, sun;

	if (cloud_cover() <= 0.0)
		return 1.0;
	p = to_world(view_pos);
	sun = sun_world(view_sun);
	return 1.0 - cloud_density(p.xz + sun.xz / max(sun.y, 0.1) * (CLOUD_HEIGHT - p.y)) * cloud_opacity();
}

/* The same for the sun itself, seen from the camera. */
float sun_through_clouds(vec3 view_sun)
{
	return cloud_shadow(vec3(0.0), view_sun);
}

/* How grey and dull the day is, 0 under a clear or broken sky: rain, or a full overcast. */
float weather_gloom()
{
	float overcast = smoothstep(0.8, 1.0, u_cloud_cover) * 0.6;

	return max(u_weather >= WEATHER_RAIN ? 0.5 + 0.5 * u_weather_amount : 0.0, overcast);
}

/* Water on flat ground, in patches fixed to the track. flatness is the ground normal's height. */
float puddle(vec3 world, float flatness)
{
	float level = 0.78 - 0.28 * u_wet;

	if (u_wet <= 0.0)
		return 0.0;
	return smoothstep(0.9, 0.97, flatness) * smoothstep(level, level + 0.04, weather_fbm(world.xz / PUDDLE_SIZE));
}

/* Snow lying on whatever faces up, from patches to full cover as the strength rises. */
float snow_cover(vec3 world, float facing)
{
	float level = 0.85 - 0.8 * u_weather_amount;

	if (u_weather != WEATHER_SNOW)
		return 0.0;
	return smoothstep(0.3, 0.7, facing) * smoothstep(level - 0.1, level + 0.1, weather_fbm(world.xz / SNOW_PATCH_SIZE));
}

/*
 The sky a puddle mirrors, close to the horizon's colour. A grey day mirrors a grey sky:
 the fog colour a shader is given is the track's, which rain leaves bright.
*/
vec3 sky_reflection(vec3 fog_color)
{
	return (u_hdr != 0 ? scene_from_gamma(fog_color) : to_linear(fog_color)) *
	       (1.0 - u_ambient_loss) * mix(1.0, PUDDLE_GLOOM, weather_gloom());
}
