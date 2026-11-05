#version 150

/*
 Volumetric clouds: a cumulus layer marched through the same cloud field the cloud
 shadows on the ground come from, carved into puffs by 3D noise, and thin cirrus far
 above. Drawn at a reduced size for a view a little wider than the screen, so the
 next frame's sky in sprite.fsh finds them by direction even as the camera turns.
 Each frame is blended into the last, which smooths out the noise of few steps.

 Out: the light the clouds add, and in alpha how much of the sky behind shows.
*/

in vec2 v_uv;
out vec4 f_color;

uniform float u_fx, u_fy;	/* the camera's projection scale */
uniform float u_margin;		/* how much wider than the screen this view is */
uniform int u_steps, u_light_steps;	/* from Cloud Quality */
uniform int u_history;		/* u_clouds holds last frame's */
uniform vec3 u_cloud_light;	/* world space, toward the sun, or the moon at night */
uniform float u_cloud_moon;	/* 1 when that's the moon */
uniform mat4 u_prev_view;	/* world to last frame's cloud view */
uniform float u_frame;		/* changes the steps' offsets each frame */
uniform sampler3D u_cloud_noise;	/* tiling Worley noise, high between round cells */

#include "common.glsl"
#include "sky.glsl"

const float CLOUD_EXTINCTION = 0.006;	/* per game unit, in the thick of a cloud */
const float EDGE_THIN = 0.65;		/* how much thinner than linear a cloud gets toward its edge */
const float SHAPE_SIZE = 4300.0;	/* game units across the noise texture, for the billows */
const float SHAPE_ERODE = 0.6;
const float DETAIL_SIZE = 950.0;	/* and again for the ragged edges */
const float DETAIL_ERODE = 0.45;
const float MAX_SPAN = 12000.0;		/* the most of a slanting ray marched through the layer */
const float FAR_START = 20000.0, FAR_END = 70000.0;	/* clouds fade into the haze between these */
const float LIGHT_STEP = 150.0;		/* the first step toward the light, each next is longer */

const float CIRRUS_HEIGHT = 14000.0, CIRRUS_SIZE = 12000.0;
const float CIRRUS_OPACITY = 0.4;

/* Sunlight on clouds against the sky's own brightness, and the sky's light on them. */
const float CLOUD_SUN = 1.0;
const float CLOUD_AMBIENT = 2.0;
const vec3 MOONLIGHT = vec3(0.07, 0.085, 0.12);	/* match g_moonlight in hooks.c */
const float CLOUD_ALTITUDE = 1.0;	/* km, where the clouds' sunlight is worked out */

const float BLEND = 0.12;	/* of this frame mixed into the last */

/*
 0 to 1 through the cumulus layer, after Schneider's clouds for Horizon Zero Dawn. The
 field says where clouds are and how tall: a flat base, and a top that rounds off higher
 where the field is thick. Large noise carves that into billows, small noise frays their
 edges. The light march leaves the small noise out.
*/
float cumulus(vec3 p, bool detail)
{
	float h = (p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE), field, top, d, erode;
	vec3 drift = vec3(WIND.x, 0.0, WIND.y) * u_time;

	if (h <= 0.0 || h >= 1.0)
		return 0.0;
	field = cloud_density(p.xz);
	if (field <= 0.0)
		return 0.0;
	top = mix(0.3, 1.0, field);
	d = field * smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(top * 0.4, top, h));
	if (d <= 0.0)
		return 0.0;
	/* Billows: thin where the noise is high, between its round cells. An overcast is one closed sheet. */
	erode = SHAPE_ERODE * (1.0 - 0.7 * smoothstep(0.7, 1.0, cloud_cover()));
	d = clamp((d - texture(u_cloud_noise, (p + drift) / SHAPE_SIZE).r * erode) / (1.0 - erode), 0.0, 1.0);
	if (detail && d > 0.0 && d < 1.0) {
		/* Only the edges fray, the inside of a cloud stays solid. */
		float n = texture(u_cloud_noise, (p + drift * 1.5) / DETAIL_SIZE).r;

		d = clamp((d - n * DETAIL_ERODE * (1.0 - d)) / (1.0 - DETAIL_ERODE * (1.0 - d)), 0.0, 1.0);
	}
	return d;
}

/* How much cloud lies between p and the light, as optical depth. */
float light_depth(vec3 p, vec3 light)
{
	float depth = 0.0, along = 0.0, len = LIGHT_STEP;
	int i;

	for (i = 0; i < u_light_steps; i++) {
		float d = cumulus(p + light * (along + len * 0.5), false);

		depth += d * mix(1.0 - EDGE_THIN, 1.0, d) * len;
		along += len;
		len *= 2.0;
	}
	return depth * CLOUD_EXTINCTION;
}

float henyey_greenstein(float mu, float g)
{
	float g2 = g * g;

	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

/*
 Light scattered toward the camera after passing depth of cloud. Light that scattered
 many times inside is softer, wider and gets further: three weaker, broader octaves
 stand in for it (Wrenninge's approximation), with a bright silver lining toward the sun.
*/
float cloud_scatter(float depth, float mu)
{
	float sum = 0.0, a = 1.0, b = 1.0, c = 1.0;
	int i;

	for (i = 0; i < 3; i++) {
		sum += a * exp(-depth * b) * mix(henyey_greenstein(mu, 0.8 * c), henyey_greenstein(mu, -0.3 * c), 0.5);
		a *= 0.6;
		b *= 0.4;
		c *= 0.5;
	}
	return sum;
}

/* The sun's light at the clouds, coloured by the air it came through, or the moon's. */
vec3 cloud_sunlight(vec3 light)
{
	if (u_cloud_moon > 0.5)
		return MOONLIGHT * SUN_ILLUMINANCE * CLOUD_SUN;
	return sun_transmittance(sky_camera() + vec3(0.0, CLOUD_ALTITUDE, 0.0), light) * SUN_ILLUMINANCE * CLOUD_SUN;
}

void main()
{
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 dir = normalize(to_world_dir(vec3(ndc.x * u_margin / u_fx, ndc.y * u_margin / u_fy, -1.0)));
	vec3 camera = to_world(vec3(0.0)), light = normalize(u_cloud_light);
	vec3 sunlight = cloud_sunlight(light), sum = vec3(0.0);
	vec3 skylight = (texture(u_sky, sky_lut_uv(vec3(0.0, 1.0, 0.0))).rgb * twilight_gain() + NIGHT_SKY * u_night) * CLOUD_AMBIENT;
	float mu = dot(dir, light), through = 1.0, cover = cloud_cover();
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + u_frame * vec2(5.588238, 3.1233), vec2(0.06711056, 0.00583715))));
	vec4 result;
	int i;

	if (cover > 0.0 && dir.y > 0.01 && camera.y < CLOUD_BASE) {
		float t0 = (CLOUD_BASE - camera.y) / dir.y, t1 = min((CLOUD_TOP - camera.y) / dir.y, t0 + MAX_SPAN);
		float dt = (t1 - t0) / float(u_steps), fade = 1.0 - smoothstep(FAR_START, FAR_END, t0);

		for (i = 0; i < u_steps && fade > 0.0; i++) {
			vec3 p = camera + dir * (t0 + (float(i) + noise) * dt);
			float d = cumulus(p, true), h, step_through;
			vec3 lit;

			if (d <= 0.0)
				continue;
			h = (p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE);
			/* Bottoms see less of the sky than tops. */
			lit = sunlight * cloud_scatter(light_depth(p, light), mu) + skylight * mix(0.5, 1.0, h);
			/* Thinner than linear toward the edge, so clouds fade out instead of ending. */
			step_through = exp(-d * mix(1.0 - EDGE_THIN, 1.0, d) * CLOUD_EXTINCTION * dt);
			sum += through * lit * (1.0 - step_through);
			through *= step_through;
			if (through < 0.01)
				break;
		}
		sum *= fade;
		through = mix(1.0, through, fade);
	}

	/* Cirrus: thin streaks of ice far above, more of them as the sky clouds over, hidden by an overcast. */
	if (dir.y > 0.01) {
		vec3 p = camera + dir * ((CIRRUS_HEIGHT - camera.y) / dir.y);
		vec2 q = (p.xz + WIND * 3.0 * u_time) / CIRRUS_SIZE;
		float streak = weather_fbm(vec2(q.x * 0.6 + q.y * 0.3, q.y * 2.5 - q.x * 0.8) * NOISE_CELLS * 0.5);
		/* In patches across the sky, not one even sheet of strokes. */
		float patches = smoothstep(0.45, 0.7, weather_fbm(q * 0.71 + vec2(5.1, 2.9)));
		/* None in a clear sky, the most around half cover, hidden again by an overcast. */
		float amount = smoothstep(0.0, 0.4, cover) * (1.0 - smoothstep(0.8, 1.0, cover)) * patches;
		float alpha = smoothstep(0.5, 0.85, streak) * CIRRUS_OPACITY * amount * smoothstep(0.01, 0.12, dir.y);
		vec3 lit = sunlight * (henyey_greenstein(mu, 0.6) + 0.05) + skylight * 0.8;

		sum += through * lit * alpha;
		through *= 1.0 - alpha;
	}

	result = vec4(sum, through);

	/* Last frame's clouds in the same direction, where they were in view. */
	if (u_history != 0) {
		vec3 v = mat3(u_prev_view) * dir;

		if (v.z < -0.01) {
			vec2 uv = v.xy * vec2(u_fx, u_fy) / u_margin / -v.z * 0.5 + 0.5;

			if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
				result = mix(texture(u_clouds, uv), result, BLEND);
		}
	}
	f_color = result;
}
