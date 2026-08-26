#include "ScaleMode.h"

#include <algorithm>

namespace lxsd
{
UvTransform UvFromCoverage( float coverX, float coverY )
{
	UvTransform t;
	if( coverX <= 0.0f || coverY <= 0.0f )
		return t;
	// screenUV -> texUV. The image occupies [ (1-cover)/2, (1+cover)/2 ] of the viewport,
	// so invert that mapping: tex = ( screen - (1-cover)/2 ) / cover.
	t.scaleX = 1.0f / coverX;
	t.scaleY = 1.0f / coverY;
	t.offX   = -( 1.0f - coverX ) / ( 2.0f * coverX );
	t.offY   = -( 1.0f - coverY ) / ( 2.0f * coverY );
	return t;
}

UvTransform ComputeUvTransform( ScaleMode mode, int imgW, int imgH, int vpW, int vpH )
{
	if( imgW <= 0 || imgH <= 0 || vpW <= 0 || vpH <= 0 )
		return UvTransform{};

	const float iw = static_cast< float >( imgW );
	const float ih = static_cast< float >( imgH );
	const float vw = static_cast< float >( vpW );
	const float vh = static_cast< float >( vpH );

	switch( mode )
	{
	case ScaleMode::Stretch:
		return UvTransform{};// cover 1:1 on both axes

	case ScaleMode::Native:
		return UvFromCoverage( iw / vw, ih / vh );

	case ScaleMode::Fit:
	{
		const float k = std::min( vw / iw, vh / ih );
		return UvFromCoverage( iw * k / vw, ih * k / vh );
	}

	case ScaleMode::Fill:
	default:
	{
		const float k = std::max( vw / iw, vh / ih );
		return UvFromCoverage( iw * k / vw, ih * k / vh );
	}
	}
}
}// namespace lxsd
