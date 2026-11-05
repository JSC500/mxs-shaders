/*
 The atmosphere, after Hillaire's "A Scalable and Production Ready Sky and Atmosphere
 Rendering Technique" (2020): Earth's air and haze in kilometres, single scattering
 marched along each ray, and multiple scattering as an even glow on top. Needs
 common.glsl before it. post_sky.fsh draws it into a small texture each frame, which
 post_final.fsh reads where the skybox shows open sky.
*/

const float GROUND_RADIUS = 6360.0;	/* km */
const float TOP_RADIUS = 6460.0;
const float CAMERA_ALTITUDE = 0.2;	/* km, a track is near enough the ground */

const vec3 RAYLEIGH_SCATTER = vec3(5.802, 13.558, 33.1) * 0.001;	/* per km at sea level */
const float RAYLEIGH_HEIGHT = 8.0;
/* Twice Hillaire's clear-day haze: a summer haze, which turns dusk orange well above the horizon. */
const float MIE_SCATTER = 0.007992, MIE_EXTINCT = 0.0088;
const float MIE_HEIGHT = 1.2;
const float MIE_G = 0.8;
const vec3 OZONE_ABSORB = vec3(0.650, 1.881, 0.085) * 0.001;	/* in a layer around 25 km */

/*
 Scales the light so a clear sky matches the game's painted ones in the scene: their blue
 reads about 0.2 there and their hazy horizons 1 to 2, this gives 0.2 at the zenith and
 1.2 at the horizon with the sun at 60 degrees.
*/
const float SUN_ILLUMINANCE = 15.0;
const float MULTI_SCATTER = 0.5;	/* of the single scattered light, spread evenly */

const float SUN_ANGULAR_RADIUS = 0.0047;	/* radians */
const float SUN_DISC_BRIGHT = 100.0;	/* limited, the real one would swamp the bloom */

/* The LUT's rows crowd toward the horizon, where the colour changes fastest. */
const vec2 SKY_LUT_SIZE = vec2(256.0, 128.0);

vec3 sky_camera()
{
	return vec3(0.0, GROUND_RADIUS + CAMERA_ALTITUDE, 0.0);
}

/* Distance along d from o to where it leaves a sphere of radius r at the origin, -1 if it misses. */
float sphere_exit(vec3 o, vec3 d, float r)
{
	float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;

	return disc < 0.0 ? -1.0 : -b + sqrt(disc);
}

/* Distance to the ground, -1 if the ray doesn't reach it. */
float ground_hit(vec3 o, vec3 d)
{
	float b = dot(o, d), c = dot(o, o) - GROUND_RADIUS * GROUND_RADIUS, disc = b * b - c;

	if (disc < 0.0 || b > 0.0)
		return -1.0;
	return -b - sqrt(disc);
}

/* Extinction per km at a height in km. Rayleigh and Mie scattering come back for the in-scattering. */
vec3 air_extinction(float h, out vec3 rayleigh, out float mie)
{
	float r = exp(-h / RAYLEIGH_HEIGHT), m = exp(-h / MIE_HEIGHT), ozone = max(0.0, 1.0 - abs(h - 25.0) / 15.0);

	rayleigh = RAYLEIGH_SCATTER * r;
	mie = MIE_SCATTER * m;
	return rayleigh + MIE_EXTINCT * m + OZONE_ABSORB * ozone;
}

/* What's left of the sunlight arriving at p, 0 behind the planet. */
vec3 sun_transmittance(vec3 p, vec3 sun)
{
	const int STEPS = 8;
	vec3 optical = vec3(0.0), rayleigh;
	float len, dt, mie;
	int i;

	if (ground_hit(p, sun) > 0.0)
		return vec3(0.0);
	len = sphere_exit(p, sun, TOP_RADIUS);
	dt = len / float(STEPS);
	for (i = 0; i < STEPS; i++)
		optical += air_extinction(length(p + sun * (float(i) + 0.5) * dt) - GROUND_RADIUS, rayleigh, mie) * dt;
	return exp(-optical);
}

float rayleigh_phase(float mu)
{
	return 3.0 / (16.0 * PI) * (1.0 + mu * mu);
}

/* Cornette-Shanks, the haze's strong forward glow around the sun. */
float mie_phase(float mu)
{
	float g2 = MIE_G * MIE_G;

	return 3.0 / (8.0 * PI) * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * MIE_G * mu, 1.5));
}

/* The light reaching the camera along world direction d, with the sun toward sun. */
vec3 sky_radiance(vec3 d, vec3 sun)
{
	const int STEPS = 32;
	vec3 o = sky_camera(), light = vec3(0.0), through = vec3(1.0), rayleigh;
	float mu = dot(d, sun), pr = rayleigh_phase(mu), pm = mie_phase(mu), len, ground, last = 0.0, mie;
	int i;

	len = sphere_exit(o, d, TOP_RADIUS);
	ground = ground_hit(o, d);
	if (ground > 0.0)
		len = ground;
	for (i = 0; i < STEPS; i++) {
		/* Steps grow with distance, the air near the camera is the thickest. */
		float t = len * pow((float(i) + 0.5) / float(STEPS), 2.0), next = len * pow((float(i) + 1.0) / float(STEPS), 2.0);
		float dt = next - last;
		vec3 p = o + d * t, ext = air_extinction(length(p) - GROUND_RADIUS, rayleigh, mie);
		vec3 sunlight = sun_transmittance(p, sun), step_through = exp(-ext * dt);
		vec3 scatter = sunlight * (rayleigh * pr + mie * pm) + sunlight * (rayleigh + mie) * MULTI_SCATTER / (4.0 * PI);

		/* The light scattered inside the step, dimmed by the rest of the step. */
		light += through * scatter * (1.0 - step_through) / max(ext, vec3(1e-6));
		through *= step_through;
		last = next;
	}
	return light * SUN_ILLUMINANCE;
}

/* Where a world direction sits in the LUT: across is the turn around the horizon, as sky_open() reads the mask. */
vec2 sky_lut_uv(vec3 d)
{
	float elevation = asin(clamp(d.y, -1.0, 1.0)), v = sqrt(abs(elevation) / (0.5 * PI));

	return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, 0.5 + 0.5 * sign(elevation) * v);
}

vec3 sky_lut_dir(vec2 uv)
{
	float turn = (uv.x - 0.5) * 2.0 * PI, s = uv.y * 2.0 - 1.0, elevation = sign(s) * s * s * 0.5 * PI;

	return vec3(cos(elevation) * cos(turn), sin(elevation), cos(elevation) * sin(turn));
}

uniform sampler2D u_sky;	/* the clear sky in every direction, from post_sky.fsh */
uniform sampler2D u_sky_mask;	/* the sine of the skybox's skyline, all the way around */
uniform vec3 u_sky_sun;		/* world space, where the sun really is: below the horizon too */

const vec3 NIGHT_SKY = vec3(0.004, 0.006, 0.012);
const float DAY_ZENITH = 0.2;	/* a clear sky straight up with a high sun, see SUN_ILLUMINANCE */

/* How much of a direction is open sky, not scenery painted into the skybox. The edge is a pixel wide. */
float skyline_open(vec3 dir)
{
	float line = texture(u_sky_mask, vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5, 0.5)).r;

	return smoothstep(line, line + max(fwidth(dir.y), 0.0001), dir.y);
}

/* The sunlight left by the clouds between the camera and a world direction. */
float clouds_toward(vec3 d)
{
	vec3 p;

	if (cloud_cover() <= 0.0)
		return 1.0;
	p = to_world(vec3(0.0));
	return 1.0 - cloud_density(p.xz + d.xz / max(d.y, 0.1) * (CLOUD_HEIGHT - p.y)) * cloud_opacity();
}

/*
 Eyes adapt as the light goes. Twilight's sky is a hundred times dimmer than by day,
 far darker than the track it lights looks, so with the time of day set the sky is
 raised until its zenith matches the light the setting leaves.
*/
float twilight_gain()
{
	const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
	float zenith;

	if (dot(u_ambient_loss, vec3(1.0)) < 0.001)
		return 1.0;
	zenith = dot(texture(u_sky, sky_lut_uv(vec3(0.0, 1.0, 0.0))).rgb, luma);
	return clamp(DAY_ZENITH * dot(1.0 - u_ambient_loss, luma) / max(zenith, 1e-6), 1.0, 300.0);
}

/* Scenery painted into the skybox is lit for the day: it takes the light the time of day leaves. */
vec3 painted_light()
{
	return mix(1.0 - u_ambient_loss, light_tint(), 0.5);
}

vec3 sun_disc(vec3 d, vec3 sun);

/* The atmosphere in a world direction. It goes through twilight by itself as the sun sets. */
vec3 clear_sky(vec3 dir)
{
	vec3 sun = normalize(u_sky_sun);

	/*
	 The disc is a hundred times the sky's brightness, so the few percent a cloud lets
	 through would still show it: the cloud field dims it too, under volumetric clouds as well.
	*/
	return texture(u_sky, sky_lut_uv(dir)).rgb * twilight_gain() + sun_disc(dir, sun) * clouds_toward(sun) +
	       NIGHT_SKY * u_night;
}

/*
 post_clouds.fsh's last frame, found by direction: the light the clouds add, and in
 alpha how much of what's behind them shows. u_cloud_scale's z is 0 without clouds.
*/
uniform sampler2D u_clouds;
uniform mat4 u_cloud_view;	/* world to the view they were drawn from */
uniform vec3 u_cloud_scale;	/* and that view's x and y scale */

vec4 clouds_at(vec3 dir)
{
	vec3 v = mat3(u_cloud_view) * dir;

	if (u_cloud_scale.z == 0.0 || v.z > -0.01)
		return vec4(0.0, 0.0, 0.0, 1.0);
	return texture(u_clouds, clamp(v.xy * u_cloud_scale.xy / -v.z * 0.5 + 0.5, 0.0, 1.0));
}

/* The sun's disc, darkened toward its rim like the real one. */
vec3 sun_disc(vec3 d, vec3 sun)
{
	float angle = acos(clamp(dot(d, sun), -1.0, 1.0)), rim;

	if (angle > SUN_ANGULAR_RADIUS * 1.5)
		return vec3(0.0);
	rim = sqrt(max(1.0 - pow(angle / SUN_ANGULAR_RADIUS, 2.0), 0.0));
	return sun_transmittance(sky_camera(), sun) * SUN_DISC_BRIGHT * (0.4 + 0.6 * rim) *
	       (1.0 - smoothstep(SUN_ANGULAR_RADIUS * 0.9, SUN_ANGULAR_RADIUS * 1.1, angle));
}
