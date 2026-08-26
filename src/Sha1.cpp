#include "Sha1.h"

#include <array>
#include <cstring>

namespace lxsd
{
namespace
{
inline uint32_t Rol( uint32_t v, int b )
{
	return ( v << b ) | ( v >> ( 32 - b ) );
}

void ProcessBlock( const uint8_t* p, uint32_t h[ 5 ] )
{
	uint32_t w[ 80 ];
	for( int i = 0; i < 16; ++i )
		w[ i ] = ( uint32_t( p[ i * 4 ] ) << 24 ) | ( uint32_t( p[ i * 4 + 1 ] ) << 16 ) |
				 ( uint32_t( p[ i * 4 + 2 ] ) << 8 ) | uint32_t( p[ i * 4 + 3 ] );
	for( int i = 16; i < 80; ++i )
		w[ i ] = Rol( w[ i - 3 ] ^ w[ i - 8 ] ^ w[ i - 14 ] ^ w[ i - 16 ], 1 );

	uint32_t a = h[ 0 ], b = h[ 1 ], c = h[ 2 ], d = h[ 3 ], e = h[ 4 ];
	for( int i = 0; i < 80; ++i )
	{
		uint32_t f, k;
		if( i < 20 )
		{
			f = ( b & c ) | ( ( ~b ) & d );
			k = 0x5A827999u;
		}
		else if( i < 40 )
		{
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		}
		else if( i < 60 )
		{
			f = ( b & c ) | ( b & d ) | ( c & d );
			k = 0x8F1BBCDCu;
		}
		else
		{
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}
		uint32_t tmp = Rol( a, 5 ) + f + e + k + w[ i ];
		e            = d;
		d            = c;
		c            = Rol( b, 30 );
		b            = a;
		a            = tmp;
	}
	h[ 0 ] += a;
	h[ 1 ] += b;
	h[ 2 ] += c;
	h[ 3 ] += d;
	h[ 4 ] += e;
}
}// namespace

std::string Sha1Hex( const std::string& data )
{
	uint32_t h[ 5 ] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

	const uint8_t* p  = reinterpret_cast< const uint8_t* >( data.data() );
	const size_t len  = data.size();
	size_t fullBlocks = len / 64;
	for( size_t i = 0; i < fullBlocks; ++i )
		ProcessBlock( p + i * 64, h );

	// Tail + padding. Worst case the 0x80 marker plus the 8 length bytes need a second block.
	uint8_t tail[ 128 ] = {};
	size_t rest         = len - fullBlocks * 64;
	std::memcpy( tail, p + fullBlocks * 64, rest );
	tail[ rest ]      = 0x80;
	size_t tailBlocks = ( rest + 1 + 8 > 64 ) ? 2 : 1;
	uint64_t bitLen   = static_cast< uint64_t >( len ) * 8;
	for( int i = 0; i < 8; ++i )
		tail[ tailBlocks * 64 - 1 - i ] = static_cast< uint8_t >( bitLen >> ( 8 * i ) );
	for( size_t i = 0; i < tailBlocks; ++i )
		ProcessBlock( tail + i * 64, h );

	static const char* hex = "0123456789abcdef";
	std::string out;
	out.reserve( 40 );
	for( int i = 0; i < 5; ++i )
		for( int b = 3; b >= 0; --b )
		{
			uint8_t byte = static_cast< uint8_t >( h[ i ] >> ( b * 8 ) );
			out.push_back( hex[ byte >> 4 ] );
			out.push_back( hex[ byte & 0x0F ] );
		}
	return out;
}

std::string Sha1Short( const std::string& data, size_t chars )
{
	std::string full = Sha1Hex( data );
	if( chars >= full.size() )
		return full;
	return full.substr( 0, chars );
}
}// namespace lxsd
