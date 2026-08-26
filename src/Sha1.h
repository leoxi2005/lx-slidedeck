// LX SlideDeck — tiny SHA-1, used only to name cache folders.
// Vendored instead of pulled in: SPEC §8 allows stb_image.h and nothing else.
// Not a security primitive — do not use it as one.
#pragma once
#include <cstdint>
#include <string>

namespace lxsd
{
/// Hex digest (40 chars, lowercase) of an arbitrary byte string.
std::string Sha1Hex( const std::string& data );

/// First `chars` characters of Sha1Hex — SPEC §3.6 wants 16.
std::string Sha1Short( const std::string& data, size_t chars = 16 );
}// namespace lxsd
