/*
 Shared by the default pack. The game's lighting works directly on gamma
 encoded values; these helpers let us light in linear space and go back.
*/
const float EXPOSURE = 1.15;
const float PI = 3.14159265;

/* Linear space shrinks the game's ambient far more than its sun, so restore the balance. */
const float AMBIENT_BOOST = 3.2;
const float SUN_BOOST = 1.7;
const float SATURATION = 0.88;

uniform float u_night;	/* the time of day setting, 1 at 100: the game's light isn't a sun */

/* At night the game's light is only a fill, the boost would make it read as daylight. */
float sun_boost()
{
	return mix(SUN_BOOST, 1.0, u_night);
}

vec3 to_linear(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(2.2));
}

vec3 to_gamma(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

/* Squaring a tinted color exaggerates the tint, which turns full shade purple. */
vec3 sky_light(vec3 ambient)
{
	vec3 c = to_linear(ambient) * AMBIENT_BOOST;

	return mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, 0.5);
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
}

float sun_shadow(vec3 pos, vec3 n, float ignore_meters)
{
	const float depth_per_meter = 1.0 / 800.0;	/* SHADOW_DEPTH_RANGE * 2 in the mod */
	float bias = 0.00004 + ignore_meters * depth_per_meter;
	vec3 near_c, far_c;
	float edge, far_edge, far_radius, tilt, near_shadow = 1.0, far_shadow = 1.0;
	vec3 map_x, map_z;

	if (u_shadow_on == 0)
		return 1.0;

	/* Pushing along the normal keeps surfaces from shadowing themselves. */
	near_c = shadow_coord(u_shadow_near_matrix, pos + n * 0.025);
	far_c = shadow_coord(u_shadow_matrix, pos + n * 0.06);

	/* Sharp map close by, fading to the wide one before its border. */
	edge = max(abs(near_c.x - 0.5), abs(near_c.y - 0.5)) * 2.0;
	edge = smoothstep(0.8, 0.95, edge);

	if (edge < 1.0)
		near_shadow = shadow_lookup(u_shadow_near_map, near_c, 4.0 / 4096.0, bias);
	/* The wide map fades out before its border, so its end isn't a visible line. */
	far_edge = max(abs(far_c.x - 0.5), abs(far_c.y - 0.5)) * 2.0;
	if (edge > 0.0 && far_edge < 1.0) {
		/*
		 Samples spread across the map land deeper on a surface tilted from the
		 sun, which would count as shadow. The bias has to clear that spread, in
		 world units, for any shadow_range. The matrix's rows hold the map's scale
		 and the sun's direction.
		*/
		far_radius = 2.5 / float(textureSize(u_shadow_map, 0).x);
		map_x = vec3(u_shadow_matrix[0][0], u_shadow_matrix[1][0], u_shadow_matrix[2][0]);
		map_z = normalize(vec3(u_shadow_matrix[0][2], u_shadow_matrix[1][2], u_shadow_matrix[2][2]));
		tilt = max(abs(dot(n, map_z)), 0.25);
		bias += far_radius * 2.0 / length(map_x) * sqrt(1.0 - tilt * tilt) / tilt * depth_per_meter;
		far_shadow = mix(shadow_lookup(u_shadow_map, far_c, far_radius, bias), 1.0, smoothstep(0.85, 1.0, far_edge));
	}

	return mix(mix(near_shadow, far_shadow, edge), 1.0, u_night);
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
	return mix(texture(u_shadow_map, vec3(c.xy, c.z - 0.0002)), 1.0, u_night);
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
	if (u_hdr != 0)
		return apply_fog(lit, scene_from_gamma(fog_color), density);
	return apply_fog(to_gamma(tonemap(lit)), fog_color, density);
}

vec3 apply_fog(vec3 c, vec3 fog_color, float density)
{
	float z = gl_FragCoord.z / gl_FragCoord.w;

	return mix(fog_color, c, min(exp(-density * z), 1.0));
}
