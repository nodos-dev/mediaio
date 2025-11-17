// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#version 450

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform DeinterlaceParams
{
    uint IsOdd;
} Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;


void main()
{
    const ivec2 TextureSize = textureSize(Input, 0);
    ivec2 Coord = ivec2((uv * TextureSize));
    if (Coord.y % 2 == 0 && Params.IsOdd == 1)
        discard;
    else if (Coord.y % 2 == 1 && Params.IsOdd == 0)
        discard;
    vec4 Color = texelFetch(Input, Coord, 0);
    rt = Color;
}
