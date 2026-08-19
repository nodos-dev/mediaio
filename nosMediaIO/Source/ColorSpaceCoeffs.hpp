// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#pragma once

#include <array>

#include "Conversion_generated.h"

namespace nos::mediaio
{

// Luma weights for red and blue; green is 1 - red - blue.
// Moved here out of Conversions.cpp in 2.13.1 so Channel Viewer shares one table.
//
// These are fixed by the standards bodies, so they live in code. If colour spaces ever
// need to be addable without a rebuild, move this table together with the primaries in
// EncodeHDRMetadata.cpp into a data file, and turn the ColorSpace pins into named value
// pins the way Config/ResolutionNames.json already does for resolutions. A data file
// keyed by the ColorSpace enum would not be enough on its own: the enum is the pin type,
// so it still decides what the user can pick.
inline std::array<double, 2> LumaCoeffs(ColorSpace colorSpace)
{
	switch (colorSpace)
	{
	case ColorSpace::REC601:
		return { .299, .114 };
	case ColorSpace::REC2020:
		return { .2627, .0593 };
	// Sony S-Gamut3 / S-Gamut3.Cine luma (R, B) coefficients, derived from the
	// published primaries against D65 white. Blue is negative because the blue
	// primary lies outside the spectral locus.
	case ColorSpace::SGAMUT3:
		return { 0.2709805, -0.0575869 };
	case ColorSpace::SGAMUT3CINE:
		return { 0.2150825, -0.1001485 };
	case ColorSpace::REC709:
	default:
		return { .2126, .0722 };
	}
}

} // namespace nos::mediaio
