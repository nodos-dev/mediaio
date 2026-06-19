// must match Conversion.fbs
#define GAMMA_CURVE_REC709  0
#define GAMMA_CURVE_HLG  1
#define GAMMA_CURVE_ST2084 2
#define GAMMA_CURVE_SRGB 3
#define GAMMA_CURVE_IDENTITY 4
#define GAMMA_CURVE_SLOG3 5

vec3 Rec709ToLinear(vec3 c)
{
    return mix(c / 4.5, pow((c + 0.099) / 1.099, vec3(1.0 / 0.45)), lessThanEqual(vec3(0.081), c));
}
vec3 LinearToRec709(vec3 c)
{
    return mix(c * 4.5, pow(c, vec3(0.45)) * 1.099 - 0.099, lessThanEqual(vec3(0.018), c));
}
vec3 HLGToLinear(vec3 c)
{
    return mix(c * c / 3.0, exp(c / 0.17883277 - 5.61582460179) + 0.02372241, lessThanEqual(vec3(0.5), c));
}
vec3 LinearToHLG(vec3 c)
{
    return mix(sqrt(c * 3.0), log(c - 0.02372241) * 0.17883277 + 1.00429346, lessThanEqual(vec3(1.0 / 12.0), c));
}

vec3 ST2084ToLinear(vec3 c)
{
    vec3 p = pow(c, vec3(0.01268331));
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(6.27739463));
}

vec3 LinearToST2084(vec3 c)
{
    vec3 p = pow(c, vec3(0.15930175));
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));
}

vec3 SRGBToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), lessThanEqual(vec3(0.04045), c));
}
vec3 LinearToSRGB(vec3 c)
{
    return mix(c * 12.92, pow(c, vec3(1.0 / 2.4)) * 1.055 - 0.055, lessThanEqual(vec3(0.0031308), c));
}
vec3 IdentityGamma(vec3 c)
{
    return c;
}

// Sony S-Log3 (full-range, normalized 0..1 code value).
// Linear breakpoint 0.01125 → code 171.2102946929/1023 ≈ 0.16739.
vec3 SLog3ToLinear(vec3 c)
{
    vec3 logBranch = pow(vec3(10.0), (c * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
    vec3 linBranch = (c * 1023.0 - 95.0) * (0.01125 / (171.2102946929 - 95.0));
    return mix(linBranch, logBranch, lessThanEqual(vec3(171.2102946929 / 1023.0), c));
}
vec3 LinearToSLog3(vec3 c)
{
    vec3 logBranch = (420.0 + log((c + 0.01) / 0.19) * (261.5 / log(10.0))) / 1023.0;
    vec3 linBranch = (c * ((171.2102946929 - 95.0) / 0.01125) + 95.0) / 1023.0;
    return mix(linBranch, logBranch, lessThanEqual(vec3(0.01125), c));
}

vec3 ToLinear(vec3 c, uint curve) {
    switch (curve) {
        case GAMMA_CURVE_REC709: return Rec709ToLinear(c);
        case GAMMA_CURVE_HLG: return HLGToLinear(c);
        case GAMMA_CURVE_ST2084: return ST2084ToLinear(c);
        case GAMMA_CURVE_SRGB: return SRGBToLinear(c);
        case GAMMA_CURVE_SLOG3: return SLog3ToLinear(c);
        default: return c;
    }
}

vec3 FromLinear(vec3 c, uint curve) {
    switch (curve) {
        case GAMMA_CURVE_REC709: return LinearToRec709(c);
        case GAMMA_CURVE_HLG: return LinearToHLG(c);
        case GAMMA_CURVE_ST2084: return LinearToST2084(c);
        case GAMMA_CURVE_SRGB: return LinearToSRGB(c);
        case GAMMA_CURVE_SLOG3: return LinearToSLog3(c);
        default: return c;
    }
}