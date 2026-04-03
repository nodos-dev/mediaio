// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "GammaFunctions.glsl"
#extension GL_EXT_shader_16bit_storage : enable

struct umat4
{
    uvec4 x, y, z, w;
};

layout(binding = 2) uniform UBO
{
    mat4 Colorspace;
    mat4 ColorspaceT;
    uint PixelFormat;
	uvec2 Resolution;
    // RGB2YCBCR
    uint InputFieldType;
    uint OutputFieldType;
    bool IsOutputInterlaced;
    // YCBCR2RGB
    uint IsInterlaced;
    uint GammaCurve;
    bool UseLUT;
} ubo;

// Defined in Conversion.fbs
#define YCBCR_PIXEL_FORMAT_YUV8 0
#define YCBCR_PIXEL_FORMAT_V210 1

uint InputFieldType = ubo.InputFieldType;
uint OutputFieldType = ubo.OutputFieldType; 
bool IsOutputInterlaced = ubo.IsOutputInterlaced;
uint IsInterlaced = ubo.IsInterlaced;

layout(binding = 3) readonly buffer SSBO
{
    uint16_t LUT[];
} GammaLUT;

#define N16 ((1<<16)-1)
#define N10 ((1<<10)-1)
#define N8  ((1<< 8)-1)

uvec3 IDX(uvec3 i)
{
    return uvec3(GammaLUT.LUT[i.x], GammaLUT.LUT[i.y], GammaLUT.LUT[i.z]);
}

uvec3 IDXuu(uvec3 c) { return IDX(clamp(c, 0, N10)); }

vec3 IDXff( vec3 c) 
{ 
    c *= N10;
    vec3 f = fract(c);
    vec3 s0 = IDXuu(uvec3(c));
    vec3 s1 = IDXuu(uvec3(c)+1);
    return mix(s0, s1, f) / float(N16);
}

/*
In:
    u8/10 -> Mul -> Idx -> f16
Out:
    f16 -> Idx -> Mul -> u8/10
*/


vec4  SDR_In (in vec3 c) 
{ 
    return  vec4(IDXff((ubo.Colorspace * vec4(c, 1)).xyz), 1); 
}

vec4  SDR_In_N (in uvec3 c, float N) 
{ 
    return  vec4(IDXff((ubo.Colorspace * vec4(c / N, 1)).xyz), 1); 
}

uvec3 SDR_Out_N(in vec3 c, float N)  
{ 
    return uvec3(round(clamp(ubo.Colorspace * vec4(IDXff(c), 1), 0.0, 1.0).xyz * N)); 
}

vec4  SDR_In_10 (in uvec3 c) { return SDR_In_N (c, N10); }
vec4  SDR_In_8  (in uvec3 c) { return SDR_In_N (c, N8); }
uvec3 SDR_Out_10(in vec3 c)  { return SDR_Out_N(c, N10); }
uvec3 SDR_Out_8 (in vec3 c)  { return SDR_Out_N(c, N8); }
