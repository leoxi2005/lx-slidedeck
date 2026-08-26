// LX SlideDeck — SPEC §5.3. Pure math, no GL, so it can be unit tested on any platform.
#pragma once

namespace lxsd
{
enum class ScaleMode : int
{
	Native  = 0,
	Fit     = 1,
	Fill    = 2,
	Stretch = 3,
};

/// Maps screen uv [0,1] to texture uv: texUV = screenUV * scale + offset.
/// Sampling outside [0,1] is what the fragment shader turns into transparent letterbox.
struct UvTransform
{
	float scaleX = 1.0f;
	float scaleY = 1.0f;
	float offX   = 0.0f;
	float offY   = 0.0f;
};

/// `coverX/coverY` = fraction of the viewport the image covers on each axis.
/// >1 means the image overflows and gets cropped, <1 means bars.
UvTransform UvFromCoverage( float coverX, float coverY );

UvTransform ComputeUvTransform( ScaleMode mode, int imgW, int imgH, int vpW, int vpH );
}// namespace lxsd
