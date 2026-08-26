// LX SlideDeck — the non-Windows half of the PowerPoint backend: there isn't one.
// macOS renders through LibreOffice instead (see Converter.cpp).
#if !defined( _WIN32 )

#	include <string>

#	include "Converter.h"

namespace lxsd
{
bool IsPowerPointAvailable()
{
	return false;
}

bool RenderWithPowerPoint( const std::string&, int, int, int, const std::string&,
						   const ConvertCallbacks&, int, std::string& error )
{
	error = "PowerPoint automation is Windows only";
	return false;
}
}// namespace lxsd

#endif
