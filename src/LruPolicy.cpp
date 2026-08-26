#include "LruPolicy.h"

#include <algorithm>

namespace lxsd
{
std::vector< int > SelectLruEvictions( const std::vector< LruEntry >& entries,
									   size_t capacity,
									   const std::vector< int >& keep )
{
	std::vector< int > out;
	if( entries.size() <= capacity )
		return out;

	std::vector< LruEntry > candidates;
	candidates.reserve( entries.size() );
	for( const LruEntry& e : entries )
		if( std::find( keep.begin(), keep.end(), e.key ) == keep.end() )
			candidates.push_back( e );

	std::sort( candidates.begin(), candidates.end(), []( const LruEntry& a, const LruEntry& b ) {
		if( a.lastUsedFrame != b.lastUsedFrame )
			return a.lastUsedFrame < b.lastUsedFrame;
		return a.key < b.key;
	} );

	size_t overflow = entries.size() - capacity;
	for( size_t i = 0; i < candidates.size() && out.size() < overflow; ++i )
		out.push_back( candidates[ i ].key );
	return out;
}
}// namespace lxsd
