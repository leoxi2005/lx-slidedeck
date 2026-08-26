#include "XmlLite.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace lxsd
{
std::string LocalName( const std::string& name )
{
	const size_t colon = name.find( ':' );
	return colon == std::string::npos ? name : name.substr( colon + 1 );
}

namespace
{
bool IsSpace( char c )
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void SkipSpace( const std::string& s, size_t& i )
{
	while( i < s.size() && IsSpace( s[ i ] ) )
		++i;
}

bool IsNameChar( char c )
{
	return !IsSpace( c ) && c != '>' && c != '/' && c != '=' && c != '<';
}

std::string Unescape( const std::string& s )
{
	if( s.find( '&' ) == std::string::npos )
		return s;
	std::string out;
	out.reserve( s.size() );
	for( size_t i = 0; i < s.size(); ++i )
	{
		if( s[ i ] != '&' )
		{
			out.push_back( s[ i ] );
			continue;
		}
		const size_t semi = s.find( ';', i );
		if( semi == std::string::npos )
		{
			out.push_back( s[ i ] );
			continue;
		}
		const std::string entity = s.substr( i + 1, semi - i - 1 );
		if( entity == "lt" )
			out.push_back( '<' );
		else if( entity == "gt" )
			out.push_back( '>' );
		else if( entity == "amp" )
			out.push_back( '&' );
		else if( entity == "quot" )
			out.push_back( '"' );
		else if( entity == "apos" )
			out.push_back( '\'' );
		else if( !entity.empty() && entity[ 0 ] == '#' )
		{
			const long code = entity[ 1 ] == 'x' ? std::strtol( entity.c_str() + 2, nullptr, 16 )
												 : std::strtol( entity.c_str() + 1, nullptr, 10 );
			// UTF-8 encode; OOXML text is UTF-8 throughout.
			if( code < 0x80 )
			{
				out.push_back( static_cast< char >( code ) );
			}
			else if( code < 0x800 )
			{
				out.push_back( static_cast< char >( 0xC0 | ( code >> 6 ) ) );
				out.push_back( static_cast< char >( 0x80 | ( code & 0x3F ) ) );
			}
			else
			{
				out.push_back( static_cast< char >( 0xE0 | ( code >> 12 ) ) );
				out.push_back( static_cast< char >( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
				out.push_back( static_cast< char >( 0x80 | ( code & 0x3F ) ) );
			}
		}
		else
		{
			out += "&" + entity + ";";
		}
		i = semi;
	}
	return out;
}
}// namespace

bool XmlDocument::Parse( const std::string& text, std::string& error )
{
	source = text;
	nodes.clear();
	rootIndex = npos;

	std::vector< size_t > stack;
	size_t i = 0;

	while( i < source.size() )
	{
		const size_t lt = source.find( '<', i );
		if( lt == std::string::npos )
			break;
		i = lt;

		// Declarations, comments, doctypes and CDATA carry no structure we need.
		if( source.compare( i, 4, "<!--" ) == 0 )
		{
			const size_t close = source.find( "-->", i );
			if( close == std::string::npos )
			{
				error = "unterminated comment";
				return false;
			}
			i = close + 3;
			continue;
		}
		if( source.compare( i, 9, "<![CDATA[" ) == 0 )
		{
			const size_t close = source.find( "]]>", i );
			if( close == std::string::npos )
			{
				error = "unterminated CDATA";
				return false;
			}
			i = close + 3;
			continue;
		}
		if( source.compare( i, 2, "<?" ) == 0 )
		{
			const size_t close = source.find( "?>", i );
			if( close == std::string::npos )
			{
				error = "unterminated processing instruction";
				return false;
			}
			i = close + 2;
			continue;
		}
		if( source.compare( i, 2, "<!" ) == 0 )
		{
			const size_t close = source.find( '>', i );
			if( close == std::string::npos )
			{
				error = "unterminated declaration";
				return false;
			}
			i = close + 1;
			continue;
		}

		// Closing tag
		if( source.compare( i, 2, "</" ) == 0 )
		{
			const size_t close = source.find( '>', i );
			if( close == std::string::npos )
			{
				error = "unterminated closing tag";
				return false;
			}
			if( stack.empty() )
			{
				error = "closing tag without a matching open tag";
				return false;
			}
			XmlNode& node   = nodes[ stack.back() ];
			node.contentEnd = i;
			node.end        = close + 1;
			stack.pop_back();
			i = close + 1;
			continue;
		}

		// Opening tag
		size_t p = i + 1;
		const size_t nameStart = p;
		while( p < source.size() && IsNameChar( source[ p ] ) )
			++p;
		if( p == nameStart )
		{
			error = "empty element name";
			return false;
		}

		XmlNode node;
		node.name  = source.substr( nameStart, p - nameStart );
		node.start = i;

		for( ;; )
		{
			SkipSpace( source, p );
			if( p >= source.size() )
			{
				error = "unterminated tag";
				return false;
			}
			if( source[ p ] == '>' )
			{
				node.selfClosing  = false;
				node.contentStart = p + 1;
				++p;
				break;
			}
			if( source.compare( p, 2, "/>" ) == 0 )
			{
				node.selfClosing  = true;
				node.contentStart = p;
				node.contentEnd   = p;
				node.end          = p + 2;
				p += 2;
				break;
			}

			const size_t attrNameStart = p;
			while( p < source.size() && IsNameChar( source[ p ] ) )
				++p;
			if( p == attrNameStart )
			{
				error = "malformed attribute";
				return false;
			}
			XmlAttribute attr;
			attr.name = source.substr( attrNameStart, p - attrNameStart );
			SkipSpace( source, p );
			if( p < source.size() && source[ p ] == '=' )
			{
				++p;
				SkipSpace( source, p );
				if( p < source.size() && ( source[ p ] == '"' || source[ p ] == '\'' ) )
				{
					const char quote      = source[ p++ ];
					const size_t valueStart = p;
					while( p < source.size() && source[ p ] != quote )
						++p;
					attr.value = Unescape( source.substr( valueStart, p - valueStart ) );
					if( p < source.size() )
						++p;
				}
			}
			node.attributes.push_back( std::move( attr ) );
		}

		const size_t index = nodes.size();
		node.parent        = stack.empty() ? npos : stack.back();
		nodes.push_back( std::move( node ) );
		if( !stack.empty() )
			nodes[ stack.back() ].children.push_back( index );
		else if( rootIndex == npos )
			rootIndex = index;
		if( !nodes[ index ].selfClosing )
			stack.push_back( index );

		i = p;
	}

	if( !stack.empty() )
	{
		error = "unclosed element <" + nodes[ stack.back() ].name + ">";
		return false;
	}
	if( rootIndex == npos )
	{
		error = "no root element";
		return false;
	}
	return true;
}

std::vector< size_t > XmlDocument::Children( size_t node, const std::string& localName ) const
{
	std::vector< size_t > out;
	if( node >= nodes.size() )
		return out;
	for( size_t child : nodes[ node ].children )
		if( localName.empty() || LocalName( nodes[ child ].name ) == localName )
			out.push_back( child );
	return out;
}

size_t XmlDocument::FirstChild( size_t node, const std::string& localName ) const
{
	if( node >= nodes.size() )
		return npos;
	for( size_t child : nodes[ node ].children )
		if( localName.empty() || LocalName( nodes[ child ].name ) == localName )
			return child;
	return npos;
}

std::vector< size_t > XmlDocument::Descendants( size_t node, const std::string& localName ) const
{
	std::vector< size_t > out;
	if( node >= nodes.size() )
		return out;
	std::vector< size_t > stack{ node };
	while( !stack.empty() )
	{
		const size_t current = stack.back();
		stack.pop_back();
		// push in reverse so the traversal comes out in document order
		const auto& kids = nodes[ current ].children;
		for( auto it = kids.rbegin(); it != kids.rend(); ++it )
			stack.push_back( *it );
		if( current != node && ( localName.empty() || LocalName( nodes[ current ].name ) == localName ) )
			out.push_back( current );
	}
	std::sort( out.begin(), out.end() );
	return out;
}

const std::string* XmlDocument::Attribute( size_t node, const std::string& name ) const
{
	if( node >= nodes.size() )
		return nullptr;
	for( const XmlAttribute& a : nodes[ node ].attributes )
		if( a.name == name || LocalName( a.name ) == name )
			return &a.value;
	return nullptr;
}

std::string XmlDocument::AttributeOr( size_t node, const std::string& name, const std::string& fallback ) const
{
	const std::string* v = Attribute( node, name );
	return v ? *v : fallback;
}

int XmlDocument::AttributeInt( size_t node, const std::string& name, int fallback ) const
{
	const std::string* v = Attribute( node, name );
	if( v == nullptr || v->empty() )
		return fallback;
	char* end   = nullptr;
	const long n = std::strtol( v->c_str(), &end, 10 );
	if( end == v->c_str() )
		return fallback;
	return static_cast< int >( n );
}

void TextSplicer::Replace( size_t start, size_t end, std::string replacement )
{
	if( end < start )
		std::swap( start, end );
	edits.push_back( Edit{ start, end, std::move( replacement ) } );
}

bool TextSplicer::Apply( const std::string& original, std::string& out ) const
{
	std::vector< Edit > sorted = edits;
	std::stable_sort( sorted.begin(), sorted.end(), []( const Edit& a, const Edit& b ) {
		return a.start < b.start;
	} );

	out.clear();
	out.reserve( original.size() );
	size_t cursor = 0;
	for( const Edit& e : sorted )
	{
		if( e.start < cursor )
			return false;// overlapping edits mean the caller built a contradictory plan
		if( e.end > original.size() )
			return false;
		out.append( original, cursor, e.start - cursor );
		out.append( e.text );
		cursor = e.end;
	}
	out.append( original, cursor, original.size() - cursor );
	return true;
}

std::string XmlEscapeAttribute( const std::string& value )
{
	std::string out;
	out.reserve( value.size() );
	for( char c : value )
	{
		switch( c )
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		default: out.push_back( c );
		}
	}
	return out;
}
}// namespace lxsd
