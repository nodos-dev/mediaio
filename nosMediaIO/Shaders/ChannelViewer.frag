// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#version 450

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform ChannelViewerParams
{
	vec4 Channel;
	vec4 Format;
} Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;

void main()
{
    vec4 Color = texture(Input,  uv);

	vec4 RGBA =
		(Params.Channel.r * Color.rrrr) +
		(Params.Channel.g * Color.gggg) +
		(Params.Channel.b * Color.bbbb) +
		(Params.Channel.a * Color.aaaa);

    float Y = Params.Format.r * Color.r + Params.Format.g * Color.g + Params.Format.b * Color.b;

    float Cb = 0.5 + 0.5 * (Color.b - Y) / (1 - Params.Format.b);
    float Cr = 0.5 + 0.5 * (Color.r - Y) / (1 - Params.Format.r);

    vec4 YCbCr =
		(Params.Channel.r * vec4(Y, Y, Y, 1.0)) +
		(Params.Channel.g * vec4(Cb, Cb, Cb, 1.0)) +
		(Params.Channel.b * vec4(Cr, Cr, Cr, 1.0));

    rt = (1 - Params.Format.a) * RGBA + Params.Format.a * YCbCr;
}
