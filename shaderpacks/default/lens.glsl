/*
 What a camera does to the finished scene, shared by the packs' post_lens.fsh. Motion
 blur and depth of field are one loop, since a sample along the movement and across
 the lens are the same thing to the sensor. Then exposure, tone mapping and colour
 grading. Needs common.glsl before it.
*/

uniform sampler2D u_src, u_depth, u_exposure;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;
uniform vec2 u_texel;
uniform mat4 u_reproject;	/* this frame's view space to last frame's screen */
uniform float u_motion;		/* blur per frame of movement, from the motion_blur setting */
uniform float u_blur_near;	/* the bike moves with the camera, so it stays sharp */
uniform float u_dof, u_saturation, u_contrast, u_split_tone, u_flare;	/* the settings, 1 at 100 */
uniform vec3 u_sun, u_up;	/* in view space */
uniform int u_goggles;		/* the setting is on and the camera is first person */
uniform float u_sun_moved;	/* 1 when the player moved the sun, which brings its colours with it */

/* Auto exposure aims for this average brightness, within limits so it never pumps hard. */
const float EXPOSURE_KEY = 0.42;
const float EXPOSURE_MIN = 0.75, EXPOSURE_MAX = 1.5;

const int SAMPLES = 16;
const float SKY_DISTANCE = 2000.0;
const float MOTION_MAX = 48.0;		/* pixels */
const float DOF_APERTURE = 24.0;	/* blur in pixels at one unit of 1/distance */
const float DOF_MAX = 7.0;		/* pixels */
const float SHARP = 0.5;		/* pixels, below this a single sample will do */

const vec3 GRADE_SHADOWS = vec3(0.9, 1.0, 1.15), GRADE_HIGHLIGHTS = vec3(1.15, 1.0, 0.85);

/* A low sun lights warm and leaves the shade to a blue sky. */
const vec3 LOW_SUN_LIGHT = vec3(1.2, 0.94, 0.7), LOW_SUN_SHADE = vec3(0.82, 0.9, 1.12);
const float LOW_SUN_START = 0.6, LOW_SUN_FULL = 0.08;	/* sine of the sun's height */

const vec3 FLARE_COLOR = vec3(1.0, 0.85, 0.65);

/* Under heavy cloud colours go cool and dull. */
const vec3 GLOOM_TINT = vec3(0.93, 0.97, 1.05);
const float GLOOM_DESATURATE = 0.3;

/* The goggles' opening as a share of the screen, and the nose in screen heights from the middle. */
const vec2 GOGGLE_OPENING = vec2(0.97, 0.94);
const float GOGGLE_SOFT = 0.2;		/* out of focus, the frame's edge is a blur this wide */
const vec2 GOGGLE_NOSE = vec2(0.0, -0.6);
const float GOGGLE_NOSE_RADIUS = 0.22;
const vec3 GOGGLE_FRAME = vec3(0.025, 0.025, 0.03);

vec2 g_uv;	/* where on the screen this pixel reads from */

float view_distance(vec2 uv)
{
	float d = texture(u_depth, uv).r;

	return d >= 1.0 ? SKY_DISTANCE : u_proj_b / (d * 2.0 - 1.0 + u_proj_a);
}

vec2 screen_motion(float z)
{
	vec2 ndc = g_uv * 2.0 - 1.0;
	vec4 last = u_reproject * vec4(ndc.x * z / u_fx, ndc.y * z / u_fy, -z, 1.0);
	vec2 motion, pixels;
	float len;

	if (last.w <= 0.0 || u_motion <= 0.0)
		return vec2(0.0);
	motion = (ndc - last.xy / last.w) * 0.5 * u_motion * smoothstep(u_blur_near, u_blur_near * 1.6, z);
	pixels = motion / u_texel;
	len = length(pixels);
	return len > MOTION_MAX ? motion * MOTION_MAX / len : motion;
}

float blur_radius(float z, float focus)
{
	return min(abs(1.0 / focus - 1.0 / z) * DOF_APERTURE * u_dof, DOF_MAX);
}

vec3 lens(float z)
{
	const float golden = 2.39996323;
	float focus = texture(u_exposure, vec2(0.5)).g;
	float radius = blur_radius(z, focus), weight = 0.0, noise;
	vec2 motion = screen_motion(z);
	vec3 sum = vec3(0.0);
	int i;

	if (radius < SHARP && length(motion / u_texel) < SHARP)
		return texture(u_src, g_uv).rgb;

	noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	for (i = 0; i < SAMPLES; i++) {
		float t = (float(i) + noise) / float(SAMPLES);
		float angle = float(i) * golden + noise * 6.2831853;
		vec2 disc = vec2(cos(angle), sin(angle)) * sqrt(t);
		vec2 uv = g_uv + motion * (t - 0.5) + disc * radius * u_texel;
		float sample_z = view_distance(uv), w = 1.0;

		/* Something sharp in front only reaches here if it is blurred itself. */
		if (sample_z < z)
			w = clamp(blur_radius(sample_z, focus) / max(radius, 0.001), 0.0, 1.0);
		if (sample_z < u_blur_near && z >= u_blur_near)
			w = 0.0;
		sum += texture(u_src, uv).rgb * w;
		weight += w;
	}
	return weight > 0.0 ? sum / weight : texture(u_src, g_uv).rgb;
}

/* How far into golden hour the sun is, only when the player chose its height. */
float low_sun()
{
	float height = dot(normalize(u_sun), normalize(u_up));

	return (1.0 - smoothstep(LOW_SUN_FULL, LOW_SUN_START, height)) * u_sun_moved * (1.0 - u_night);
}

vec3 grade(vec3 c)
{
	vec3 g = clamp(c, 0.0, 1.0);
	float l = dot(g, vec3(0.2126, 0.7152, 0.0722));

	g = mix(vec3(l), g, u_saturation * (1.0 - GLOOM_DESATURATE * weather_gloom()));
	g *= mix(vec3(1.0), GLOOM_TINT, weather_gloom());
	g = clamp(g, 0.0, 1.0);
	g = mix(g, g * g * (3.0 - 2.0 * g), u_contrast);
	g *= mix(vec3(1.0), mix(GRADE_SHADOWS, GRADE_HIGHLIGHTS, l), u_split_tone);
	return g * mix(vec3(1.0), mix(LOW_SUN_SHADE, LOW_SUN_LIGHT, l), low_sun());
}

/*
 The camera lens catching the sun: a glow with star spikes, a streak across, and
 ghosts along the line from the sun through the middle of the screen. The exposure
 pass eases how much of the sun is in sight, so hiding it fades the flare.
*/
vec3 lens_flare(vec2 uv)
{
	const vec3 ghost_tint[4] = vec3[4](vec3(0.4, 0.7, 1.0), vec3(1.0, 0.6, 0.3), vec3(0.5, 1.0, 0.6), vec3(0.8, 0.5, 1.0));
	const float ghost_at[4] = float[4](0.6, 1.25, 1.6, 2.2);	/* 1 is the middle, 2 the sun's mirror image */
	const float ghost_size[4] = float[4](0.05, 0.11, 0.035, 0.16);
	float seen = texture(u_exposure, vec2(0.5)).b * u_flare * (1.0 - u_night) * sun_through_clouds(u_sun);
	float aspect = u_texel.y / u_texel.x;
	vec3 sun = normalize(u_sun), c;
	vec2 at, d;
	float r, angle;
	int i;

	if (seen < 0.002 || sun.z > -0.05)
		return vec3(0.0);
	at = vec2(sun.x * u_fx, sun.y * u_fy) / -sun.z * 0.5 + 0.5;
	d = (uv - at) * vec2(aspect, 1.0);
	r = length(d);
	angle = atan(d.y, d.x);

	c = vec3(exp(-r / 0.035) * 1.2);
	c += pow(abs(cos(angle * 3.0)), 60.0) * exp(-r / 0.12) * 0.5;
	c += exp(-abs(d.y) * 300.0) * exp(-abs(d.x) / 0.35) * 0.35;
	c += vec3(0.6, 0.8, 1.0) * exp(-abs(r - 0.32) / 0.012) * 0.03;
	for (i = 0; i < 4; i++) {
		vec2 ghost = (uv - mix(at, vec2(0.5), ghost_at[i])) * vec2(aspect, 1.0);

		c += ghost_tint[i] * (1.0 - smoothstep(ghost_size[i] * 0.7, ghost_size[i], length(ghost))) * 0.04;
	}
	return c * FLARE_COLOR * seen;
}

/* The inside of a goggle frame, dark and out of focus, with a cut-out for the nose. */
vec3 goggles(vec3 c, vec2 uv)
{
	vec2 p = (uv - 0.5) * vec2(u_texel.y / u_texel.x, 1.0);
	vec2 q = abs(uv - 0.5) * 2.0 / GOGGLE_OPENING;
	float shape = pow(pow(q.x, 4.0) + pow(q.y, 4.0), 0.25);	/* a rounded rectangle, 1 on its edge */
	float open = 1.0 - smoothstep(1.0 - GOGGLE_SOFT, 1.0 + GOGGLE_SOFT * 0.5, shape);

	open *= smoothstep(GOGGLE_NOSE_RADIUS - GOGGLE_SOFT * 0.4, GOGGLE_NOSE_RADIUS + GOGGLE_SOFT * 0.4, length(p - GOGGLE_NOSE));
	return mix(GOGGLE_FRAME, c, open);
}

float exposure_scale()
{
	float k = EXPOSURE_KEY / exp(texture(u_exposure, vec2(0.5)).r);

	/* A night scene is meant to be dark, so exposure never lifts it. */
	return clamp(k, EXPOSURE_MIN, mix(EXPOSURE_MAX, 1.0, u_night));
}

/* The finished, gamma encoded picture at a point on the screen. */
vec3 lens_finish(vec2 uv)
{
	vec3 c;

	g_uv = uv;
	c = grade(to_gamma(tonemap((lens(view_distance(uv)) + lens_flare(uv)) * exposure_scale())));
	return u_goggles != 0 ? goggles(c, uv) : c;
}
