// LX SlideDeck — SPEC §3.6. The little JSON file that says what a cache folder contains.
// Deliberately hand-rolled: it only ever has to read back what we wrote.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lxsd
{
struct DeckManifest
{
	int version         = 1;
	int stepCount       = 0;
	int width           = 0;//!< pixel size of the exported images
	int height          = 0;
	int exportWidth     = 0;//!< the Export Width parameter this cache was built with
	int64_t sourceMtime = 0;
	std::string sourcePath;
	std::string renderer;//!< "powerpoint" | "libreoffice" | "folder"
	std::string warning; //!< surfaced in Status when non-empty
	std::vector< int > slideOfStep;//!< 1-based source slide per step; may be empty
};

std::string SerializeManifest( const DeckManifest& m );
bool ParseManifest( const std::string& json, DeckManifest& out );

std::string JsonEscape( const std::string& s );
}// namespace lxsd
