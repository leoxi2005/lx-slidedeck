#include "ImageDecode.h"

#include "Platform.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO// we read the bytes ourselves so non-ASCII paths work the same on both platforms
#include "stb_image.h"

namespace lxsd
{
bool DecodeImageFile( const std::string& path, RgbaImage& out, std::string& error )
{
	std::string bytes;
	if( !ReadWholeFile( path, bytes ) )
	{
		error = "cannot read " + FileName( path );
		return false;
	}

	int w = 0, h = 0, channels = 0;
	stbi_uc* data = stbi_load_from_memory( reinterpret_cast< const stbi_uc* >( bytes.data() ),
										   static_cast< int >( bytes.size() ),
										   &w, &h, &channels, 4 );
	if( data == nullptr )
	{
		const char* reason = stbi_failure_reason();
		error              = std::string( "decode failed: " ) + ( reason ? reason : "unknown" );
		return false;
	}

	out.width  = w;
	out.height = h;
	out.pixels.assign( data, data + static_cast< size_t >( w ) * h * 4 );
	stbi_image_free( data );
	return true;
}

bool ReadImageSize( const std::string& path, int& width, int& height )
{
	std::string bytes;
	if( !ReadWholeFile( path, bytes ) )
		return false;
	int comp = 0;
	return stbi_info_from_memory( reinterpret_cast< const stbi_uc* >( bytes.data() ),
								  static_cast< int >( bytes.size() ),
								  &width, &height, &comp ) != 0;
}
}// namespace lxsd
