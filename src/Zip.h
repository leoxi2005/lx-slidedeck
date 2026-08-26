// LX SlideDeck — a .pptx is a zip of xml parts. This is the whole of our zip handling:
// read every part into memory, edit some of them, write a new zip out.
//
// Decks are tens of megabytes at most, so keeping the package in memory keeps the rest of
// the converter simple and lets a step be built without touching the user's file again.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lxsd
{
class ZipPackage
{
public:
	/// Part name as stored in the archive, e.g. "ppt/slides/slide1.xml".
	using Entry = std::pair< std::string, std::string >;

	bool Has( const std::string& name ) const;
	const std::string* Get( const std::string& name ) const;
	/// Adds or replaces a part, keeping insertion order for anything new.
	void Set( const std::string& name, std::string data );
	void Remove( const std::string& name );

	const std::vector< Entry >& Entries() const
	{
		return entries;
	}
	std::vector< Entry >& Entries()
	{
		return entries;
	}
	size_t Size() const
	{
		return entries.size();
	}

private:
	std::vector< Entry > entries;
};

bool ReadZip( const std::string& path, ZipPackage& out, std::string& error );
bool WriteZip( const std::string& path, const ZipPackage& package, std::string& error );
}// namespace lxsd
