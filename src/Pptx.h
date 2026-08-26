// LX SlideDeck — reading a .pptx well enough to know what its animation steps are, and
// writing out a stripped-down .pptx that shows exactly one of those steps.
//
// This replaces what SPEC §3 did through PowerPoint COM. Doing it on the file itself is
// both portable and better defined: where the COM object model left Effect.Paragraph
// undocumented, OOXML says plainly that <p:pRg st="0"/> is a zero-based paragraph index,
// and where COM made us guess entrance from emphasis, <p:cTn presetClass="entr"> says so.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "Zip.h"

namespace lxsd
{
/// One relationship out of a .rels part.
struct Relationship
{
	std::string id;
	std::string type;  //!< the full schema URL
	std::string target;//!< as written, usually relative
	std::string mode;  //!< "External" for links we must not follow
};

/// Parses a .rels part. `error` is only set on malformed XML.
bool ParseRelationships( const std::string& xml, std::vector< Relationship >& out, std::string& error );

/// Resolves a relationship target against the part that declared it,
/// e.g. ("ppt/presentation.xml", "slides/slide1.xml") -> "ppt/slides/slide1.xml".
std::string ResolvePartPath( const std::string& sourcePart, const std::string& target );

/// The .rels part name that belongs to a part: "ppt/presentation.xml" -> "ppt/_rels/presentation.xml.rels"
std::string RelsPathFor( const std::string& part );

/// What has to be hidden to render one animation step.
struct StepPlan
{
	int sourceSlide     = 0;//!< 1-based position in the original deck
	int stepInSlide     = 0;//!< 0-based
	std::string slidePart;  //!< e.g. "ppt/slides/slide3.xml"
	std::vector< int > hiddenShapeIds;
	std::vector< std::pair< int, int > > hiddenParagraphs;//!< (shape id, 1-based paragraph)
};

struct DeckPlan
{
	std::vector< StepPlan > steps;
	int slideWidthEmu  = 0;
	int slideHeightEmu = 0;
	std::vector< std::string > warnings;

	double AspectRatio() const
	{
		return slideHeightEmu > 0 ? static_cast< double >( slideWidthEmu ) / slideHeightEmu : 16.0 / 9.0;
	}
};

/// Works out every animation step in the deck.
bool AnalyzePptx( const ZipPackage& package, DeckPlan& plan, std::string& error );

/// Builds a .pptx that contains only the given steps, one slide each, with animations and
/// transitions stripped and the hidden parts of each step made invisible.
/// Parts nothing references any more are dropped, which is what keeps a 200-step deck from
/// writing the source deck's media out 200 times.
bool BuildStepPackage( const ZipPackage& source,
					   const DeckPlan& plan,
					   const std::vector< int >& stepIndices,
					   ZipPackage& out,
					   std::string& error );
}// namespace lxsd
