// LX SlideDeck — just enough XML for OOXML.
//
// The parser keeps byte offsets into the original document instead of rebuilding a tree of
// strings, and edits are applied as splices over that original text. That way a slide we
// modify comes out byte-identical everywhere we did not touch it, which matters: PowerPoint
// and LibreOffice are both happy to reject a file over a detail a re-serialiser got wrong.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lxsd
{
struct XmlAttribute
{
	std::string name;
	std::string value;
};

struct XmlNode
{
	std::string name;//!< as written, prefix included, e.g. "p:sp"
	std::vector< XmlAttribute > attributes;
	std::vector< size_t > children;//!< indices into XmlDocument::nodes
	size_t parent       = static_cast< size_t >( -1 );
	size_t start        = 0;//!< offset of '<'
	size_t end          = 0;//!< offset one past '>' of the closing tag
	size_t contentStart = 0;//!< offset one past '>' of the opening tag
	size_t contentEnd   = 0;//!< offset of '<' of the closing tag
	bool selfClosing    = false;
};

/// "p:sp" -> "sp"
std::string LocalName( const std::string& name );

class XmlDocument
{
public:
	bool Parse( const std::string& text, std::string& error );

	const std::string& Text() const
	{
		return source;
	}
	bool Empty() const
	{
		return nodes.empty();
	}
	size_t Root() const
	{
		return rootIndex;
	}
	const XmlNode& Node( size_t index ) const
	{
		return nodes[ index ];
	}
	size_t NodeCount() const
	{
		return nodes.size();
	}

	/// Direct children whose local name matches. An empty name matches every child.
	std::vector< size_t > Children( size_t node, const std::string& localName ) const;
	/// First matching direct child, or npos.
	size_t FirstChild( size_t node, const std::string& localName ) const;
	/// Every descendant with this local name, in document order.
	std::vector< size_t > Descendants( size_t node, const std::string& localName ) const;

	const std::string* Attribute( size_t node, const std::string& name ) const;
	std::string AttributeOr( size_t node, const std::string& name, const std::string& fallback ) const;
	int AttributeInt( size_t node, const std::string& name, int fallback ) const;

	static constexpr size_t npos = static_cast< size_t >( -1 );

private:
	std::string source;
	std::vector< XmlNode > nodes;
	size_t rootIndex = npos;
};

/// Collects edits as byte ranges and applies them in one pass.
/// Overlapping edits are a programming error and are rejected by Apply().
class TextSplicer
{
public:
	void Replace( size_t start, size_t end, std::string replacement );
	void Remove( size_t start, size_t end )
	{
		Replace( start, end, std::string() );
	}
	void Insert( size_t at, std::string text )
	{
		Replace( at, at, std::move( text ) );
	}
	bool Empty() const
	{
		return edits.empty();
	}

	/// Returns the edited text, or false when two edits overlap.
	bool Apply( const std::string& original, std::string& out ) const;

private:
	struct Edit
	{
		size_t start;
		size_t end;
		std::string text;
	};
	std::vector< Edit > edits;
};

/// Escapes a value for use inside an XML attribute.
std::string XmlEscapeAttribute( const std::string& value );
}// namespace lxsd
