#version 150

/* Ambient occlusion from the depth buffer, at half resolution. */

in vec2 v_uv;
out vec4 f_color;

uniform sampler2D u_depth;
uniform vec2 u_texel;
uniform float u_proj_a, u_proj_b, u_fx, u_fy;
uniform vec3 u_sun;
uniform float u_night;

const int SAMPLES = 12;
const float RADIUS = 0.7;	/* meters */
const float FADE_START = 30.0, FADE_END = 80.0;	/* meters */
const float INTENSITY = 1.4;
const float CONTACT_END = 40.0;

vec3 view_pos(vec2 uv)
{
	float z = -u_proj_b / (texture(u_depth, uv).r * 2.0 - 1.0 + u_proj_a);
	vec2 ndc = uv * 2.0 - 1.0;

	return vec3(-ndc.x * z / u_fx, -ndc.y * z / u_fy, z);
}

/*
 Takes the smaller step on each axis so normals stay clean across object edges.
 The step spans several texels: over a single one, depth buffer precision shows
 up as stairs on flat ground, which reads as lines of false occlusion.
*/
vec3 view_normal(vec3 p)
{
	vec2 step = u_texel * 3.0;
	vec3 l = p - view_pos(v_uv - vec2(step.x, 0.0));
	vec3 r = view_pos(v_uv + vec2(step.x, 0.0)) - p;
	vec3 d = p - view_pos(v_uv - vec2(0.0, step.y));
	vec3 u = view_pos(v_uv + vec2(0.0, step.y)) - p;

	return normalize(cross(abs(l.z) < abs(r.z) ? l : r, abs(d.z) < abs(u.z) ? d : u));
}

/*
 A short march toward the sun through the depth buffer. It picks up detail too
 small for the shadow maps, like tyres meeting the ground.
*/
float contact_shadow(vec3 p, vec3 n)
{
	const int STEPS = 16;
	float dist = -p.z, reach = 0.8 + dist * 0.01;
	float thickness = 0.5 + dist * 0.02;
	float bias = 0.025 + dist * 0.005;	/* above the depth buffer's stairs on flat ground */
	float occlusion = 0.0;
	vec3 sun = normalize(u_sun);
	vec3 start = p + n * bias;
	int i;

	if (dist > CONTACT_END || u_night >= 1.0)
		return 1.0;

	for (i = 1; i <= STEPS; i++) {
		float t = float(i) / float(STEPS);
		vec3 q = start + sun * t * reach;
		vec2 uv = vec2(-q.x * u_fx, -q.y * u_fy) / q.z * 0.5 + 0.5;
		float d = view_pos(uv).z - q.z;

		/* The further along the hit, the softer: no hard edge for the blur to fight. */
		if (d > bias && d < thickness)
			occlusion = max(occlusion, 1.0 - t * t);
	}
	return 1.0 - occlusion * (1.0 - smoothstep(CONTACT_END * 0.6, CONTACT_END, dist)) * (1.0 - u_night);
}

void main()
{
	const float golden = 2.39996323;
	vec3 p, n;
	vec2 radius;
	float dist, noise, sum = 0.0;
	int i;

	f_color = vec4(1.0);
	if (texture(u_depth, v_uv).r >= 1.0)
		return;

	p = view_pos(v_uv);
	dist = -p.z;
	if (dist > FADE_END)
		return;
	n = view_normal(p);

	radius = vec2(u_fx, u_fy) * 0.5 * RADIUS / dist;
	noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

	for (i = 0; i < SAMPLES; i++) {
		float t = (float(i) + 0.5) / float(SAMPLES);
		float angle = float(i) * golden + noise * 6.2831853;
		vec3 v = view_pos(v_uv + vec2(cos(angle), sin(angle)) * radius * sqrt(t)) - p;
		float vv = dot(v, v);

		/* Ignore samples far beyond the radius, they are different objects. */
		sum += max(dot(v, n) - (0.01 + 0.008 * dist), 0.0) / (vv + 0.01) *
		       (1.0 - smoothstep(RADIUS * RADIUS, RADIUS * RADIUS * 4.0, vv));
	}

	sum = clamp(1.0 - sum * 2.0 * RADIUS * INTENSITY / float(SAMPLES), 0.0, 1.0);
	f_color = vec4(mix(sum, 1.0, smoothstep(FADE_START, FADE_END, dist)), contact_shadow(p, n), 0.0, 1.0);
}
