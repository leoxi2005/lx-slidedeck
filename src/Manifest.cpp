#include "Manifest.h"

#include <cstdlib>
#include <sstream>

namespace lxsd
{
std::string JsonEscape( const std::string& s )
{
	std::string out;
	out.reserve( s.size() + 8 );
	for( char c : s )
	{
		switch( c )
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if( static_cast< unsigned char >( c ) < 0x20 )
			{
				char buf[ 8 ];
				snprintf( buf, sizeof( buf ), "\\u%04x", c );
				out += buf;
			}
			else
			{
				out.push_back( c );
			}
		}
	}
	return out;
}

std::string SerializeManifest( const DeckManifest& m )
{
	std::ostringstream o;
	o << "{\n";
	o << "  \"version\": " << m.version << ",\n";
	o << "  \"stepCount\": " << m.stepCount << ",\n";
	o << "  \"width\": " << m.width << ",\n";
	o << "  \"height\": " << m.height << ",\n";
	o << "  \"exportWidth\": " << m.exportWidth << ",\n";
	o << "  \"sourceMtime\": " << m.sourceMtime << ",\n";
	o << "  \"sourcePath\": \"" << JsonEscape( m.sourcePath ) << "\",\n";
	o << "  \"renderer\": \"" << JsonEscape( m.renderer ) << "\",\n";
	o << "  \"warning\": \"" << JsonEscape( m.warning ) << "\",\n";
	o << "  \"slideOfStep\": [";
	for( size_t i = 0; i < m.slideOfStep.size(); ++i )
	{
		if( i )
			o << ", ";
		o << m.slideOfStep[ i ];
	}
	o << "]\n}\n";
	return o.str();
}

namespace
{
size_t FindKey( const std::string& json, const std::string& key )
{
	const std::string needle = "\"" + key + "\"";
	size_t pos               = json.find( needle );
	if( pos == std::string::npos )
		return std::string::npos;
	pos = json.find( ':', pos + needle.size() );
	if( pos == std::string::npos )
		return std::string::npos;
	++pos;
	while( pos < json.size() && ( json[ pos ] == ' ' || json[ pos ] == '\t' || json[ pos ] == '\n' || json[ pos ] == '\r' ) )
		++pos;
	return pos;
}

bool ReadInt( const std::string& json, const std::string& key, int64_t& out )
{
	size_t pos = FindKey( json, key );
	if( pos == std::string::npos )
		return false;
	char* end   = nullptr;
	long long v = std::strtoll( json.c_str() + pos, &end, 10 );
	if( end == json.c_str() + pos )
		return false;
	out = v;
	return true;
}

bool ReadString( const std::string& json, const std::string& key, std::string& out )
{
	size_t pos = FindKey( json, key );
	if( pos == std::string::npos || pos >= json.size() || json[ pos ] != '"' )
		return false;
	++pos;
	std::string v;
	while( pos < json.size() && json[ pos ] != '"' )
	{
		if( json[ pos ] == '\\' && pos + 1 < json.size() )
		{
			++pos;
			switch( json[ pos ] )
			{
			case 'n': v.push_back( '\n' ); break;
			case 'r': v.push_back( '\r' ); break;
			case 't': v.push_back( '\t' ); break;
			case 'u':
			{
				// We only ever emit \u for control characters, so decoding to a byte is enough.
				if( pos + 4 < json.size() )
				{
					std::string hex = json.substr( pos + 1, 4 );
					v.push_back( static_cast< char >( std::strtol( hex.c_str(), nullptr, 16 ) ) );
					pos += 4;
				}
				break;
			}
			default: v.push_back( json[ pos ] ); break;
			}
			++pos;
			continue;
		}
		v.push_back( json[ pos ] );
		++pos;
	}
	out = v;
	return true;
}
}// namespace

bool ParseManifest( const std::string& json, DeckManifest& out )
{
	int64_t v = 0;
	if( !ReadInt( json, "stepCount", v ) )
		return false;
	out.stepCount = static_cast< int >( v );

	if( ReadInt( json, "version", v ) )
		out.version = static_cast< int >( v );
	if( ReadInt( json, "width", v ) )
		out.width = static_cast< int >( v );
	if( ReadInt( json, "height", v ) )
		out.height = static_cast< int >( v );
	if( ReadInt( json, "exportWidth", v ) )
		out.exportWidth = static_cast< int >( v );
	if( ReadInt( json, "sourceMtime", v ) )
		out.sourceMtime = v;

	ReadString( json, "sourcePath", out.sourcePath );
	ReadString( json, "renderer", out.renderer );
	ReadString( json, "warning", out.warning );

	out.slideOfStep.clear();
	size_t pos = FindKey( json, "slideOfStep" );
	if( pos != std::string::npos && pos < json.size() && json[ pos ] == '[' )
	{
		size_t end = json.find( ']', pos );
		std::string body = json.substr( pos + 1, end == std::string::npos ? 0 : end - pos - 1 );
		std::istringstream in( body );
		std::string token;
		while( std::getline( in, token, ',' ) )
		{
			try
			{
				out.slideOfStep.push_back( std::stoi( token ) );
			}
			catch( ... )
			{
			}
		}
	}
	return out.stepCount > 0;
}
}// namespace lxsd
