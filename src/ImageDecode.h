// LX SlideDeck — PNG decoding. Worker thread only: never call this from ProcessOpenGL.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lxsd
{
struct RgbaImage
{
	int width  = 0;
	int height = 0;
	std::vector< uint8_t > pixels;//!< width * height * 4, row 0 is the top of the image

	bool Valid() const
	{
		return width > 0 && height > 0 && pixels.size() == static_cast< size_t >( width ) * height * 4;
	}
};

/// Decodes a PNG (or anything else stb_image understands) to 8-bit RGBA.
/// Returns false and fills `error` on failure.
bool DecodeImageFile( const std::string& path, RgbaImage& out, std::string& error );

/// Reads just the pixel size without decoding the whole image.
bool ReadImageSize( const std::string& path, int& width, int& height );
}// namespace lxsd
