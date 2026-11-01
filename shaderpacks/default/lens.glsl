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
uniform float u_dof, u_saturation, u_contrast, u_split_tone;	/* the settings, 1 at 100 */

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

vec3 grade(vec3 c)
{
	vec3 g = clamp(c, 0.0, 1.0);
	float l = dot(g, vec3(0.2126, 0.7152, 0.0722));

	g = mix(vec3(l), g, u_saturation);
	g = clamp(g, 0.0, 1.0);
	g = mix(g, g * g * (3.0 - 2.0 * g), u_contrast);
	return g * mix(vec3(1.0), mix(GRADE_SHADOWS, GRADE_HIGHLIGHTS, l), u_split_tone);
}

/* The finished, gamma encoded picture at a point on the screen. */
vec3 lens_finish(vec2 uv)
{
	vec3 c;
	float k = EXPOSURE_KEY / exp(texture(u_exposure, vec2(0.5)).r);

	g_uv = uv;
	c = lens(view_distance(uv));

	/* A night scene is meant to be dark, so exposure never lifts it. */
	k = clamp(k, EXPOSURE_MIN, mix(EXPOSURE_MAX, 1.0, u_night));
	return grade(to_gamma(tonemap(c * k)));
}
