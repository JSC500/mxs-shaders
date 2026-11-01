/*
 Textures the way the console drew them: small, and with no filtering. A mip level
 near the given size stands in for a low resolution copy, read at texel centers.
*/
vec4 psx_texture(sampler2D tex, vec2 uv, float largest)
{
	ivec2 full = textureSize(tex, 0);
	int level = int(ceil(log2(max(float(max(full.x, full.y)) / largest, 1.0))));
	vec2 size = vec2(textureSize(tex, level));

	return textureLod(tex, (floor(uv * size) + 0.5) / size, float(level));
}
