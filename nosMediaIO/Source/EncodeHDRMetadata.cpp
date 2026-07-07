// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <cstdint>

#include "Conversion_generated.h"

namespace nos::mediaio
{

namespace
{
struct Primaries
{
	// CIE 1931 xy for red, green, blue and white point.
	float rx, ry, gx, gy, bx, by, wx, wy;
};

// Canonical mastering-display primaries + D65 white point per colour space.
// These are exact, standard values — the only part of HDR10 static metadata
// that can be derived from a colour-space enum. S-Gamut3 primaries include
// negative coordinates that ST 2086's unsigned encoding can't carry; they
// clamp to 0 downstream (camera gamuts aren't display-referred anyway).
Primaries PrimariesFor(ColorSpace cs)
{
	switch (cs)
	{
	case ColorSpace::REC709:      return {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f, 0.3127f, 0.3290f};
	case ColorSpace::REC601:      return {0.630f, 0.340f, 0.310f, 0.595f, 0.155f, 0.070f, 0.3127f, 0.3290f}; // SMPTE 170M
	case ColorSpace::REC2020:     return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
	case ColorSpace::SGAMUT3:     return {0.730f, 0.280f, 0.140f, 0.855f, 0.100f, -0.050f, 0.3127f, 0.3290f};
	case ColorSpace::SGAMUT3CINE: return {0.766f, 0.275f, 0.225f, 0.800f, 0.089f, -0.087f, 0.3127f, 0.3290f};
	default:                      return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f}; // Rec2020
	}
}

bool IsSDR(GammaCurve g)
{
	return g == GammaCurve::REC709 || g == GammaCurve::SRGB;
}

// ST 2086 stores each chromaticity as a uint16 in 0.00002 units, so only
// coordinates in [0, 1.3107] survive. Camera gamuts like S-Gamut3 use virtual
// primaries with negative coordinates — not representable, and not a
// mastering-display gamut in the first place.
constexpr float ST2086_CHROMA_MAX = 0xFFFF * 0.00002f; // 1.3107
bool Encodable(float c) { return c >= 0.0f && c <= ST2086_CHROMA_MAX; }
bool Encodable(const Primaries& p)
{
	return Encodable(p.rx) && Encodable(p.ry) && Encodable(p.gx) && Encodable(p.gy)
	    && Encodable(p.bx) && Encodable(p.by) && Encodable(p.wx) && Encodable(p.wy);
}
} // namespace

struct EncodeHDRMetadataNode : NodeContext
{
	EncodeHDRMetadataNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);

		ColorSpace colorSpace = ColorSpace::REC2020;
		if (auto* p = execParams.GetPinData<ColorSpace>(NOS_NAME_STATIC("ColorSpace")))
			colorSpace = *p;
		GammaCurve gammaCurve = GammaCurve::ST2084;
		if (auto* p = execParams.GetPinData<GammaCurve>(NOS_NAME_STATIC("GammaCurve")))
			gammaCurve = *p;

		float maxLuminance = 0.0f, minLuminance = 0.0f;
		if (auto* p = execParams.GetPinData<float>(NOS_NAME_STATIC("MaxLuminance")))
			maxLuminance = *p;
		if (auto* p = execParams.GetPinData<float>(NOS_NAME_STATIC("MinLuminance")))
			minLuminance = *p;

		uint32_t maxCLL = 0, maxFALL = 0;
		if (auto* p = execParams.GetPinData<uint32_t>(NOS_NAME_STATIC("MaxCLL")))
			maxCLL = *p;
		if (auto* p = execParams.GetPinData<uint32_t>(NOS_NAME_STATIC("MaxFALL")))
			maxFALL = *p;

		// Luminance: 0 means "use the GammaCurve default" (mastering luminance
		// can't be inferred from the curve exactly, but SDR ~100-nit vs HDR
		// ~1000-nit is the sensible convention). Any positive value overrides.
		// MaxCLL/MaxFALL are NOT derived — 0 is the honest CTA-861.3 "unknown".
		const bool sdr = IsSDR(gammaCurve);
		if (maxLuminance <= 0.0f)
			maxLuminance = sdr ? 100.0f : 1000.0f;
		if (minLuminance <= 0.0f)
			minLuminance = sdr ? 0.1f : 0.0001f;

		Primaries pr = PrimariesFor(colorSpace);
		if (!Encodable(pr))
		{
			// Camera gamuts (S-Gamut3 / S-Gamut3.Cine) have virtual primaries
			// that ST 2086 can't carry. Flag it loudly and fall back to a valid
			// Rec.2020 container rather than emit clamped, meaningless primaries.
			SetNodeStatusMessage(
				"Selected Color Space is not a valid mastering-display gamut: its primaries fall "
				"outside the SMPTE ST 2086 range (0..1.3107). This is a scene-referred camera gamut "
				"(e.g. S-Gamut3), not a display. Falling back to Rec.2020 — pick REC709 or REC2020.",
				fb::NodeStatusMessageType::FAILURE);
			pr = PrimariesFor(ColorSpace::REC2020);
		}
		else
			ClearNodeStatusMessages();

		HDRMetadata out(pr.rx, pr.ry, pr.gx, pr.gy, pr.bx, pr.by, pr.wx, pr.wy,
		                maxLuminance, minLuminance, maxCLL, maxFALL);

		nosBuffer buf{.Data = &out, .Size = sizeof(out)};
		SetPinValue(NOS_NAME_STATIC("Out"), buf);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterEncodeHDRMetadata(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("EncodeHDRMetadata"), EncodeHDRMetadataNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
