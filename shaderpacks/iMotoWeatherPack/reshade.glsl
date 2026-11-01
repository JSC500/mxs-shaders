/*
 Translated from three ReShade effects: Deband, Daltonize and DisplayDepth. They
 run on the finished picture, as in ReShade, but before the HUD. Needs common.glsl
 and lens.glsl before it.

 ReShade's sliders are the constants below. Change one, then Reload Current Shader
 in the F8 panel. DisplayDepth's scale, offset and reversed settings are left out:
 they configure how ReShade finds a game's depth, and here the depth is the game's own.
*/

/* Deband.fx by haasn and JPulowski: blurs flat areas that show steps, like the sky. */
const bool DEBAND = true;
const bool DEBAND_WEBER = true;		/* "Weber ratio" */
const bool DEBAND_DEVIATION = true;	/* "Standard deviation" */
const bool DEBAND_SKY_ONLY = false;	/* "Depth detection" at a banding depth of 1 */
const float DEBAND_DEVIATION_LIMIT = 0.007;
const float DEBAND_WEBER_LIMIT = 0.04;
const float DEBAND_RADIUS = 24.0;	/* pixels */
const int DEBAND_ITERATIONS = 1;
const int DEBAND_DEBUG = 0;		/* 1 the blurred image, 2 the banding map in green */

/* Daltonize.fx: shifts the colours a type of colour blindness can't tell apart. */
const int DALTONIZE = 0;	/* 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia */

/* DisplayDepth.fx: a debug view of the depth buffer, near dark and far bright. */
const int DISPLAY_DEPTH = 0;	/* 0 off, 1 depth, 2 normals, 3 normals left and depth right */
const float DISPLAY_DEPTH_RANGE = 1000.0;	/* game units shown as white */

/* The finished picture without motion blur or depth of field, for neighbouring pixels. */
vec3 reshade_finished(vec2 uv)
{
	return grade(to_gamma(tonemap(texture(u_src, uv).rgb * exposure_scale())));
}

/* CeeJay.dk's ordered dither for an 8 bit screen, shared by Deband and DisplayDepth. */
vec3 reshade_dither(vec2 uv)
{
	vec2 size = 1.0 / u_texel;
	float grid_position = fract(dot(uv, size * vec2(1.0 / 16.0, 10.0 / 36.0) + 0.25));
	float dither_shift = 0.25 * (1.0 / 255.0);
	vec3 dither_shift_rgb = vec3(dither_shift, -dither_shift, dither_shift);

	return mix(2.0 * dither_shift_rgb, -2.0 * dither_shift_rgb, grid_position);
}

float deband_rand(float x)
{
	return fract(x / 41.0);
}

float deband_permute(float x)
{
	return mod((34.0 * x + 1.0) * x, 289.0);
}

vec3 deband(vec3 ori, vec2 uv)
{
	vec3 m, mean, k, sd, ref[4], result, banding_map = vec3(1.0);
	float h, dir;
	vec2 o, pt = vec2(0.0);
	int i;

	if (DEBAND_SKY_ONLY && texture(u_depth, uv).r < 1.0)
		return ori;

	/* ReShade adds a random number each frame here, which this pass doesn't have. */
	m = vec3(uv + 1.0, 1.0);
	h = deband_permute(deband_permute(deband_permute(m.x) + m.y) + m.z);

	dir = deband_rand(deband_permute(h)) * 6.2831853;
	o = vec2(cos(dir), sin(dir));

	for (i = 1; i <= DEBAND_ITERATIONS; i++) {
		pt = deband_rand(h) * DEBAND_RADIUS * float(i) * u_texel;
		h = deband_permute(h);
	}

	/* Samples at quarter turns around the pixel. */
	ref[0] = reshade_finished(uv + pt * o);
	ref[1] = reshade_finished(uv - pt * o);
	ref[2] = reshade_finished(uv + pt * vec2(-o.y, o.x));
	ref[3] = reshade_finished(uv + pt * vec2(o.y, -o.x));

	mean = (ori + ref[0] + ref[1] + ref[2] + ref[3]) * 0.2;
	k = abs(ori - mean);
	for (i = 0; i < 4; i++)
		k += abs(ref[i] - mean);
	k = k * 0.2 / mean;

	sd = vec3(0.0);
	for (i = 0; i < 4; i++)
		sd += (ref[i] - ori) * (ref[i] - ori);
	sd = sqrt(sd * 0.25);

	if (DEBAND_DEBUG == 2)
		result = vec3(0.0, 1.0, 0.0);
	else
		result = (ref[0] + ref[1] + ref[2] + ref[3]) * 0.25;

	/* 1 where a channel is flat enough to have bands, per channel as in ReShade. */
	if (DEBAND_DEBUG != 1) {
		if (DEBAND_WEBER)
			banding_map *= vec3(lessThanEqual(k, vec3(DEBAND_WEBER_LIMIT * float(DEBAND_ITERATIONS))));
		if (DEBAND_DEVIATION)
			banding_map *= vec3(lessThanEqual(sd, vec3(DEBAND_DEVIATION_LIMIT * float(DEBAND_ITERATIONS))));
	}

	return mix(ori, result + reshade_dither(uv), banding_map);
}

vec3 daltonize(vec3 c)
{
	vec3 lms, dalt, error, correction;

	/* RGB to LMS */
	lms.x = 17.8824 * c.r + 43.5161 * c.g + 4.11935 * c.b;
	lms.y = 3.45565 * c.r + 27.1554 * c.g + 3.86714 * c.b;
	lms.z = 0.0299566 * c.r + 0.184309 * c.g + 1.46709 * c.b;

	/* What the eye with the deficiency sees. */
	dalt = lms;
	if (DALTONIZE == 1)
		dalt.x = 2.02344 * lms.y - 2.52581 * lms.z;
	else if (DALTONIZE == 2)
		dalt.y = 0.494207 * lms.x + 1.24827 * lms.z;
	else
		dalt.z = -0.395913 * lms.x + 0.801109 * lms.y;

	/* LMS to RGB */
	error.r = 0.0809444479 * dalt.x - 0.130504409 * dalt.y + 0.116721066 * dalt.z;
	error.g = -0.0102485335 * dalt.x + 0.0540193266 * dalt.y - 0.113614708 * dalt.z;
	error.b = -0.000365296938 * dalt.x - 0.00412161469 * dalt.y + 0.693511405 * dalt.z;

	/* The part of the colour that can't be seen, moved where it can. */
	error = c - error;
	correction = vec3(0.0, error.r * 0.7 + error.g, error.r * 0.7 + error.b);
	return c + correction;
}

vec3 view_position(vec2 uv)
{
	float z = view_distance(uv);
	vec2 ndc = uv * 2.0 - 1.0;

	return vec3(ndc.x * z / u_fx, ndc.y * z / u_fy, -z);
}

vec3 display_depth(vec2 uv)
{
	vec3 center = view_position(uv);
	vec3 east = view_position(uv + vec2(u_texel.x, 0.0));
	vec3 north = view_position(uv + vec2(0.0, u_texel.y));
	vec3 normal = normalize(cross(east - center, north - center)) * 0.5 + 0.5;
	vec3 depth = vec3(clamp(-center.z / DISPLAY_DEPTH_RANGE, 0.0, 1.0)) + reshade_dither(uv);

	if (DISPLAY_DEPTH == 1)
		return depth;
	if (DISPLAY_DEPTH == 2)
		return normal;
	return gl_FragCoord.x < 0.5 / u_texel.x ? normal : depth;
}

/* The default pack's finished picture with the ReShade effects on top, in ReShade's order. */
vec3 reshade_finish(vec2 uv)
{
	vec3 c = lens_finish(uv);

	if (DEBAND)
		c = deband(c, uv);
	if (DALTONIZE != 0)
		c = daltonize(c);
	if (DISPLAY_DEPTH != 0)
		c = display_depth(uv);
	return c;
}
