// LX SlideDeck — SPEC §4.1. Which textures to throw away, expressed without any GL so it
// can be unit tested. The caller does the glDeleteTextures.
#pragma once

#include <cstdint>
#include <vector>

namespace lxsd
{
struct LruEntry
{
	int key               = 0;
	uint64_t lastUsedFrame = 0;
};

/// Returns the keys to evict so that at most `capacity` entries remain.
/// Entries listed in `keep` are never evicted (current and next step, typically) — if that
/// alone already exceeds the capacity, nothing more can be freed and the list comes back
/// shorter than the overflow.
/// Oldest lastUsedFrame goes first; ties break on the lower key so the result is stable.
std::vector< int > SelectLruEvictions( const std::vector< LruEntry >& entries,
									   size_t capacity,
									   const std::vector< int >& keep );
}// namespace lxsd
