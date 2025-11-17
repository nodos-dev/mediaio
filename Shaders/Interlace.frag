// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#version 450

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform InterlaceParams
{
    uint ShouldOutputOdd;
} Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;


void main()
{
    const ivec2 TextureSize = textureSize(Input, 0);
    ivec2 Coord = ivec2((uv * TextureSize));
    if (Coord.y % 2 == 0 && Params.ShouldOutputOdd == 1)
        Coord.y += 1;
    else if (Coord.y % 2 == 1 && Params.ShouldOutputOdd == 0)
        Coord.y -= 1;
    vec4 Color = texelFetch(Input, Coord, 0);
    rt = Color;
}