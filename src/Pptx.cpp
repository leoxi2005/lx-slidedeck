#include "Pptx.h"

#include <algorithm>
#include <set>

#include "DeckLogic.h"
#include "Platform.h"
#include "XmlLite.h"

namespace lxsd
{
namespace
{
const char* const kSlideContentType = "application/vnd.openxmlformats-officedocument.presentationml.slide+xml";
const char* const kSlideRelType     = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
const char* const kNotesSlideRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide";

// Every element we inject carries its own namespace declaration.
//
// Most decks declare a: and r: once on the root element, but plenty of generators declare
// them per element instead — and in such a file an injected <p:sldId r:id="…"/> leaves r:
// unbound, which makes the whole package malformed. LibreOffice then refuses it with
// nothing but "source file could not be loaded". A redundant declaration is always legal,
// so declare it every time and stop caring what the deck does.
const char* const kNsDrawing = " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"";
const char* const kNsRels    = " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

/// A shape as it sits in one slide's shape tree.
struct ShapeInfo
{
	int id       = 0;
	size_t node  = XmlDocument::npos;
	size_t txBody = XmlDocument::npos;
	int paragraphCount = 0;
};

bool IsShapeElement( const std::string& localName )
{
	return localName == "sp" || localName == "pic" || localName == "graphicFrame" ||
		   localName == "grpSp" || localName == "cxnSp";
}

/// Every shape in the slide, groups included — animations happily target shapes nested
/// inside a group.
std::vector< ShapeInfo > CollectShapes( const XmlDocument& doc, size_t spTree )
{
	std::vector< ShapeInfo > out;
	if( spTree == XmlDocument::npos )
		return out;

	for( size_t node : doc.Descendants( spTree, "" ) )
	{
		if( !IsShapeElement( LocalName( doc.Node( node ).name ) ) )
			continue;

		ShapeInfo info;
		info.node = node;
		// The shape's own id lives in its non-visual properties: nvSpPr / nvPicPr / …
		for( size_t child : doc.Children( node, "" ) )
		{
			const std::string local = LocalName( doc.Node( child ).name );
			if( local.size() > 2 && local.compare( 0, 2, "nv" ) == 0 )
			{
				const size_t cNvPr = doc.FirstChild( child, "cNvPr" );
				if( cNvPr != XmlDocument::npos )
					info.id = doc.AttributeInt( cNvPr, "id", 0 );
				break;
			}
		}
		if( info.id == 0 )
			continue;

		info.txBody = doc.FirstChild( node, "txBody" );
		if( info.txBody != XmlDocument::npos )
			info.paragraphCount = static_cast< int >( doc.Children( info.txBody, "p" ).size() );

		out.push_back( info );
	}
	return out;
}

const ShapeInfo* FindShapeById( const std::vector< ShapeInfo >& shapes, int id )
{
	for( const ShapeInfo& s : shapes )
		if( s.id == id )
			return &s;
	return nullptr;
}

/// The main animation sequence, or npos when the slide has none — which is the common case
/// and the one SPEC §3.3 warns about most loudly.
size_t FindMainSequence( const XmlDocument& doc )
{
	for( size_t seq : doc.Descendants( doc.Root(), "seq" ) )
	{
		const size_t cTn = doc.FirstChild( seq, "cTn" );
		if( cTn != XmlDocument::npos && doc.AttributeOr( cTn, "nodeType", "" ) == "mainSeq" )
			return cTn;
	}
	return XmlDocument::npos;
}

int TriggerFromNodeType( const std::string& nodeType )
{
	if( nodeType == "clickEffect" || nodeType == "clickPar" )
		return kTriggerOnPageClick;
	if( nodeType == "afterEffect" || nodeType == "afterGroup" )
		return kTriggerAfterPrevious;
	return kTriggerWithPrevious;
}

/// Reads one slide's effects, already flattened into the record shape DeckLogic works on.
std::vector< EffectRecord > ReadEffects( const XmlDocument& doc, size_t mainSeq )
{
	std::vector< EffectRecord > effects;
	if( mainSeq == XmlDocument::npos )
		return effects;

	int sequenceIndex = 0;
	for( size_t cTn : doc.Descendants( mainSeq, "cTn" ) )
	{
		const std::string* presetClass = doc.Attribute( cTn, "presetClass" );
		if( presetClass == nullptr )
			continue;// a grouping node, not an effect

		const std::vector< size_t > targets = doc.Descendants( cTn, "spTgt" );
		if( targets.empty() )
			continue;// nothing we can address (a media or command effect)
		const size_t spTgt = targets.front();

		const int shapeId = doc.AttributeInt( spTgt, "spid", 0 );
		if( shapeId == 0 )
			continue;

		const int trigger = TriggerFromNodeType( doc.AttributeOr( cTn, "nodeType", "" ) );
		const bool isExit = ( *presetClass == "exit" );

		// <p:txEl><p:pRg st="0" end="2"/></p:txEl> means the effect covers paragraphs 0..2.
		// ECMA-376 defines these indices as zero-based, so +1 puts them in our own
		// convention where 0 is reserved for "the whole shape".
		int st = -1, en = -1;
		const size_t txEl = doc.FirstChild( spTgt, "txEl" );
		if( txEl != XmlDocument::npos )
		{
			const size_t pRg = doc.FirstChild( txEl, "pRg" );
			if( pRg != XmlDocument::npos )
			{
				st = doc.AttributeInt( pRg, "st", 0 );
				en = doc.AttributeInt( pRg, "end", st );
			}
		}

		if( st < 0 )
		{
			EffectRecord e;
			e.sequenceIndex = ++sequenceIndex;
			e.shapeId       = shapeId;
			e.rawParagraph  = 0;
			e.isExit        = isExit;
			e.triggerType   = trigger;
			effects.push_back( e );
			continue;
		}

		for( int p = st; p <= en; ++p )
		{
			EffectRecord e;
			e.sequenceIndex = ++sequenceIndex;
			e.shapeId       = shapeId;
			e.rawParagraph  = p + 1;
			e.isExit        = isExit;
			// One effect covering a range of paragraphs is still one click. Only the first
			// record may open a step, or a three-line build would become three steps.
			e.triggerType = ( p == st ) ? trigger : kTriggerWithPrevious;
			effects.push_back( e );
		}
	}
	return effects;
}

std::string ReadPart( const ZipPackage& package, const std::string& name )
{
	const std::string* data = package.Get( name );
	return data ? *data : std::string();
}
}// namespace

//--------------------------------------------------------------------------------------
bool ParseRelationships( const std::string& xml, std::vector< Relationship >& out, std::string& error )
{
	XmlDocument doc;
	if( xml.empty() )
		return true;// a part without rels is normal
	if( !doc.Parse( xml, error ) )
		return false;

	for( size_t node : doc.Children( doc.Root(), "Relationship" ) )
	{
		Relationship r;
		r.id     = doc.AttributeOr( node, "Id", "" );
		r.type   = doc.AttributeOr( node, "Type", "" );
		r.target = doc.AttributeOr( node, "Target", "" );
		r.mode   = doc.AttributeOr( node, "TargetMode", "" );
		out.push_back( std::move( r ) );
	}
	return true;
}

std::string ResolvePartPath( const std::string& sourcePart, const std::string& target )
{
	if( target.empty() )
		return {};
	if( target[ 0 ] == '/' )
		return target.substr( 1 );

	std::string dir = sourcePart;
	const size_t slash = dir.find_last_of( '/' );
	dir = ( slash == std::string::npos ) ? std::string() : dir.substr( 0, slash );

	std::string combined = dir.empty() ? target : dir + "/" + target;

	// Normalise . and .. by hand: these are package paths, not filesystem paths.
	std::vector< std::string > parts;
	std::string token;
	for( char c : combined + "/" )
	{
		if( c != '/' )
		{
			token.push_back( c );
			continue;
		}
		if( token.empty() || token == "." )
		{
		}
		else if( token == ".." )
		{
			if( !parts.empty() )
				parts.pop_back();
		}
		else
		{
			parts.push_back( token );
		}
		token.clear();
	}

	std::string out;
	for( size_t i = 0; i < parts.size(); ++i )
	{
		if( i )
			out.push_back( '/' );
		out += parts[ i ];
	}
	return out;
}

std::string RelsPathFor( const std::string& part )
{
	const size_t slash = part.find_last_of( '/' );
	if( slash == std::string::npos )
		return "_rels/" + part + ".rels";
	return part.substr( 0, slash ) + "/_rels/" + part.substr( slash + 1 ) + ".rels";
}

//--------------------------------------------------------------------------------------
bool AnalyzePptx( const ZipPackage& package, DeckPlan& plan, std::string& error )
{
	const std::string presentationXml = ReadPart( package, "ppt/presentation.xml" );
	if( presentationXml.empty() )
	{
		error = "not a PowerPoint file (ppt/presentation.xml is missing)";
		return false;
	}

	XmlDocument presentation;
	if( !presentation.Parse( presentationXml, error ) )
		return false;

	const size_t sldSz = presentation.FirstChild( presentation.Root(), "sldSz" );
	if( sldSz != XmlDocument::npos )
	{
		plan.slideWidthEmu  = presentation.AttributeInt( sldSz, "cx", 12192000 );
		plan.slideHeightEmu = presentation.AttributeInt( sldSz, "cy", 6858000 );
	}
	else
	{
		plan.slideWidthEmu  = 12192000;
		plan.slideHeightEmu = 6858000;
	}

	std::vector< Relationship > presRels;
	if( !ParseRelationships( ReadPart( package, "ppt/_rels/presentation.xml.rels" ), presRels, error ) )
		return false;

	std::map< std::string, std::string > relTargetById;
	for( const Relationship& r : presRels )
		if( r.mode != "External" )
			relTargetById[ r.id ] = ResolvePartPath( "ppt/presentation.xml", r.target );

	const size_t sldIdLst = presentation.FirstChild( presentation.Root(), "sldIdLst" );
	if( sldIdLst == XmlDocument::npos )
	{
		error = "empty deck";
		return false;
	}

	int slideNumber = 0;
	for( size_t sldId : presentation.Children( sldIdLst, "sldId" ) )
	{
		++slideNumber;
		const std::string relId = presentation.AttributeOr( sldId, "id", "" ).empty()
									  ? presentation.AttributeOr( sldId, "r:id", "" )
									  : presentation.AttributeOr( sldId, "r:id", "" );
		auto it = relTargetById.find( relId );
		if( it == relTargetById.end() )
		{
			plan.warnings.push_back( "slide " + std::to_string( slideNumber ) + " could not be located" );
			continue;
		}

		const std::string slidePart = it->second;
		const std::string slideXml  = ReadPart( package, slidePart );
		if( slideXml.empty() )
		{
			plan.warnings.push_back( "slide " + std::to_string( slideNumber ) + " is missing from the file" );
			continue;
		}

		XmlDocument slide;
		std::string slideError;
		if( !slide.Parse( slideXml, slideError ) )
		{
			plan.warnings.push_back( "slide " + std::to_string( slideNumber ) + ": " + slideError );
			continue;
		}

		// Slides hidden in PowerPoint are hidden here too.
		if( slide.AttributeOr( slide.Root(), "show", "1" ) == "0" )
			continue;

		const size_t cSld   = slide.FirstChild( slide.Root(), "cSld" );
		const size_t spTree = cSld == XmlDocument::npos ? XmlDocument::npos : slide.FirstChild( cSld, "spTree" );
		const std::vector< ShapeInfo > shapes = CollectShapes( slide, spTree );

		SlideRecord record;
		record.slideIndex = slideNumber;
		for( const ShapeInfo& s : shapes )
		{
			ShapeRecord sr;
			sr.shapeId        = s.id;
			sr.hasTextFrame   = s.txBody != XmlDocument::npos;
			sr.paragraphCount = s.paragraphCount;
			// Unlike the COM route, hiding a paragraph here means giving its runs a fully
			// transparent fill, which works whatever the original fill was — so the
			// gradient-text caveat in SPEC §3.4 does not apply.
			sr.paragraphFillSolid = true;
			record.shapes.push_back( sr );
		}
		record.effects = ReadEffects( slide, FindMainSequence( slide ) );

		// Effects pointing at shapes we cannot find would silently hide nothing; say so.
		for( const EffectRecord& e : record.effects )
		{
			if( FindShapeById( shapes, e.shapeId ) == nullptr )
			{
				plan.warnings.push_back( "slide " + std::to_string( slideNumber ) +
										 ": animation targets a shape that is not on the slide" );
				break;
			}
		}

		const std::vector< std::vector< int > > steps = SplitStepsByClick( record.effects );
		const std::vector< TargetVisibility > visibility =
			BuildVisibilityMap( record, steps, ParagraphBase::OneBased );

		for( int k = 0; k < static_cast< int >( steps.size() ); ++k )
		{
			StepPlan step;
			step.sourceSlide = slideNumber;
			step.stepInSlide = k;
			step.slidePart   = slidePart;
			for( const TargetVisibility& target : visibility )
			{
				if( VisibleAt( target, k ) )
					continue;
				if( target.key.paragraph == 0 )
					step.hiddenShapeIds.push_back( target.key.shapeId );
				else
					step.hiddenParagraphs.emplace_back( target.key.shapeId, target.key.paragraph );
			}
			plan.steps.push_back( std::move( step ) );
		}
	}

	if( plan.steps.empty() )
	{
		error = "empty deck";
		return false;
	}
	return true;
}

//--------------------------------------------------------------------------------------
namespace
{
std::string TransparentFill()
{
	return std::string( "<a:solidFill" ) + kNsDrawing +
		   "><a:srgbClr val=\"000000\"><a:alpha val=\"0\"/></a:srgbClr></a:solidFill>";
}

bool IsFillElement( const std::string& local )
{
	return local == "solidFill" || local == "noFill" || local == "gradFill" ||
		   local == "blipFill" || local == "pattFill" || local == "grpFill";
}

bool IsBulletElement( const std::string& local )
{
	return local == "buChar" || local == "buAutoNum" || local == "buNone" || local == "buBlip";
}

/// Gives one text run a fully transparent fill, creating its rPr when it has none.
void HideRun( const XmlDocument& doc, size_t run, TextSplicer& splicer )
{
	const size_t rPr = doc.FirstChild( run, "rPr" );
	if( rPr == XmlDocument::npos )
	{
		if( doc.Node( run ).selfClosing )
			return;// no text in it anyway
		splicer.Insert( doc.Node( run ).contentStart,
						std::string( "<a:rPr" ) + kNsDrawing + ">" + TransparentFill() + "</a:rPr>" );
		return;
	}

	// Whatever fill it had has to go, or two fills would fight.
	for( size_t child : doc.Children( rPr, "" ) )
		if( IsFillElement( LocalName( doc.Node( child ).name ) ) )
			splicer.Remove( doc.Node( child ).start, doc.Node( child ).end );

	if( doc.Node( rPr ).selfClosing )
	{
		// <a:rPr …/> has to grow a body: replace the closing "/>" with ">…</a:rPr>".
		const size_t end = doc.Node( rPr ).end;
		splicer.Replace( end - 2, end,
						 std::string( ">" ) + TransparentFill() + "</" + doc.Node( rPr ).name + ">" );
		return;
	}

	// DrawingML fixes the order of rPr's children: the fill goes right after a:ln.
	size_t insertAt   = doc.Node( rPr ).contentStart;
	const size_t line = doc.FirstChild( rPr, "ln" );
	if( line != XmlDocument::npos )
		insertAt = doc.Node( line ).end;
	splicer.Insert( insertAt, TransparentFill() );
}

/// Removes the bullet glyph of a hidden paragraph, which is drawn by the paragraph itself
/// and would otherwise stay on screen with nothing next to it.
void HideBullet( const XmlDocument& doc, size_t paragraph, TextSplicer& splicer )
{
	const size_t pPr = doc.FirstChild( paragraph, "pPr" );
	if( pPr == XmlDocument::npos )
	{
		if( doc.Node( paragraph ).selfClosing )
			return;
		splicer.Insert( doc.Node( paragraph ).contentStart,
						std::string( "<a:pPr" ) + kNsDrawing + "><a:buNone/></a:pPr>" );
		return;
	}

	for( size_t child : doc.Children( pPr, "" ) )
		if( IsBulletElement( LocalName( doc.Node( child ).name ) ) )
			splicer.Remove( doc.Node( child ).start, doc.Node( child ).end );

	if( doc.Node( pPr ).selfClosing )
	{
		const size_t end = doc.Node( pPr ).end;
		splicer.Replace( end - 2, end,
						 std::string( "><a:buNone" ) + kNsDrawing + "/></" + doc.Node( pPr ).name + ">" );
		return;
	}

	// buNone comes after the bullet font/size properties and before tabLst / defRPr.
	size_t insertAt = doc.Node( pPr ).contentEnd;
	for( size_t child : doc.Children( pPr, "" ) )
	{
		const std::string local = LocalName( doc.Node( child ).name );
		if( local == "tabLst" || local == "defRPr" || local == "extLst" )
		{
			insertAt = doc.Node( child ).start;
			break;
		}
	}
	splicer.Insert( insertAt, std::string( "<a:buNone" ) + kNsDrawing + "/>" );
}

/// Produces the slide xml for one step: animations stripped, hidden things hidden.
bool BuildStepSlideXml( const std::string& slideXml, const StepPlan& step,
						std::string& out, std::string& error )
{
	XmlDocument doc;
	if( !doc.Parse( slideXml, error ) )
		return false;

	TextSplicer splicer;

	// The exported image is a still, so the animation and the transition both go.
	for( size_t node : doc.Descendants( doc.Root(), "timing" ) )
		splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );
	for( size_t node : doc.Children( doc.Root(), "transition" ) )
		splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );
	// Alternate-content wrappers can hold a second copy of the transition.
	for( size_t node : doc.Descendants( doc.Root(), "transition" ) )
		splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );

	const size_t cSld   = doc.FirstChild( doc.Root(), "cSld" );
	const size_t spTree = cSld == XmlDocument::npos ? XmlDocument::npos : doc.FirstChild( cSld, "spTree" );
	const std::vector< ShapeInfo > shapes = CollectShapes( doc, spTree );

	std::set< size_t > removedShapeNodes;
	for( int shapeId : step.hiddenShapeIds )
	{
		const ShapeInfo* shape = FindShapeById( shapes, shapeId );
		if( shape == nullptr )
			continue;
		// Removing the element outright is safe here: DrawingML positions every shape
		// absolutely, so nothing else moves.
		splicer.Remove( doc.Node( shape->node ).start, doc.Node( shape->node ).end );
		removedShapeNodes.insert( shape->node );
	}

	for( const auto& entry : step.hiddenParagraphs )
	{
		const ShapeInfo* shape = FindShapeById( shapes, entry.first );
		if( shape == nullptr || shape->txBody == XmlDocument::npos )
			continue;
		if( removedShapeNodes.count( shape->node ) != 0 )
			continue;// the whole shape is already gone

		const std::vector< size_t > paragraphs = doc.Children( shape->txBody, "p" );
		const int index                        = entry.second - 1;// back to 0-based
		if( index < 0 || index >= static_cast< int >( paragraphs.size() ) )
			continue;

		const size_t paragraph = paragraphs[ index ];
		// Making the text transparent rather than deleting it is what keeps every other
		// line exactly where it was — that is what makes the crossfade look like a fade in
		// rather than a jump (SPEC §3.4).
		for( const char* runName : { "r", "fld", "br" } )
			for( size_t run : doc.Descendants( paragraph, runName ) )
				HideRun( doc, run, splicer );
		HideBullet( doc, paragraph, splicer );
	}

	if( !splicer.Apply( slideXml, out ) )
	{
		error = "conflicting edits while building a step";
		return false;
	}
	return true;
}

/// Everything reachable from `seeds` by following relationships.
void CollectReachable( const ZipPackage& package,
					   const std::vector< std::string >& seeds,
					   const std::set< std::string >& skipRelTypes,
					   std::set< std::string >& keep )
{
	std::vector< std::string > queue = seeds;
	while( !queue.empty() )
	{
		const std::string part = queue.back();
		queue.pop_back();
		if( part.empty() || keep.count( part ) != 0 )
			continue;
		if( !package.Has( part ) )
			continue;
		keep.insert( part );

		const std::string relsPart = RelsPathFor( part );
		const std::string* rels    = package.Get( relsPart );
		if( rels == nullptr )
			continue;
		keep.insert( relsPart );

		std::vector< Relationship > list;
		std::string error;
		if( !ParseRelationships( *rels, list, error ) )
			continue;
		for( const Relationship& r : list )
		{
			if( r.mode == "External" )
				continue;
			if( skipRelTypes.count( r.type ) != 0 )
				continue;
			queue.push_back( ResolvePartPath( part, r.target ) );
		}
	}
}
}// namespace

bool BuildStepPackage( const ZipPackage& source,
					   const DeckPlan& plan,
					   const std::vector< int >& stepIndices,
					   ZipPackage& out,
					   std::string& error )
{
	if( stepIndices.empty() )
	{
		error = "no steps requested";
		return false;
	}

	// ---- 1. build the new slide parts -------------------------------------------------
	struct NewSlide
	{
		std::string part;
		std::string rels;
		std::string xml;
		std::string relsXml;
	};
	std::vector< NewSlide > newSlides;

	for( size_t n = 0; n < stepIndices.size(); ++n )
	{
		const int index = stepIndices[ n ];
		if( index < 0 || index >= static_cast< int >( plan.steps.size() ) )
		{
			error = "step out of range";
			return false;
		}
		const StepPlan& step       = plan.steps[ index ];
		const std::string* slideXml = source.Get( step.slidePart );
		if( slideXml == nullptr )
		{
			error = "slide " + std::to_string( step.sourceSlide ) + " vanished from the package";
			return false;
		}

		NewSlide slide;
		slide.part = "ppt/slides/slide" + std::to_string( n + 1 ) + ".xml";
		slide.rels = RelsPathFor( slide.part );
		if( !BuildStepSlideXml( *slideXml, step, slide.xml, error ) )
			return false;

		// The step slide keeps its source's relationships — same layout, same images — but
		// not its notes, which we drop along with the notes parts.
		const std::string* sourceRels = source.Get( RelsPathFor( step.slidePart ) );
		if( sourceRels != nullptr )
		{
			XmlDocument doc;
			std::string parseError;
			if( doc.Parse( *sourceRels, parseError ) )
			{
				TextSplicer splicer;
				for( size_t node : doc.Children( doc.Root(), "Relationship" ) )
					if( doc.AttributeOr( node, "Type", "" ) == kNotesSlideRelType )
						splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );
				if( !splicer.Apply( *sourceRels, slide.relsXml ) )
					slide.relsXml = *sourceRels;
			}
			else
			{
				slide.relsXml = *sourceRels;
			}
		}
		newSlides.push_back( std::move( slide ) );
	}

	// ---- 2. work out which of the original parts are still needed ---------------------
	std::set< std::string > keep;
	std::set< std::string > skipFromPresentation{ kSlideRelType };

	// Seed from the package root, but stop the presentation from dragging in its slides:
	// ours replace them.
	keep.insert( "[Content_Types].xml" );
	keep.insert( "_rels/.rels" );
	{
		std::vector< Relationship > rootRels;
		std::string parseError;
		ParseRelationships( ReadPart( source, "_rels/.rels" ), rootRels, parseError );
		std::vector< std::string > seeds;
		for( const Relationship& r : rootRels )
			if( r.mode != "External" )
				seeds.push_back( ResolvePartPath( "", r.target ) );

		// Walk the presentation by hand first so we can skip its slide relationships.
		for( const std::string& seed : seeds )
		{
			if( seed != "ppt/presentation.xml" )
			{
				CollectReachable( source, { seed }, {}, keep );
				continue;
			}
			keep.insert( seed );
			keep.insert( RelsPathFor( seed ) );
			std::vector< Relationship > presRels;
			ParseRelationships( ReadPart( source, RelsPathFor( seed ) ), presRels, parseError );
			std::vector< std::string > next;
			for( const Relationship& r : presRels )
			{
				if( r.mode == "External" || skipFromPresentation.count( r.type ) != 0 )
					continue;
				next.push_back( ResolvePartPath( seed, r.target ) );
			}
			CollectReachable( source, next, {}, keep );
		}
	}
	// Then from our own slides, so their layouts and images survive.
	{
		std::vector< std::string > next;
		for( const NewSlide& slide : newSlides )
		{
			std::vector< Relationship > rels;
			std::string parseError;
			ParseRelationships( slide.relsXml, rels, parseError );
			for( const Relationship& r : rels )
				if( r.mode != "External" )
					next.push_back( ResolvePartPath( slide.part, r.target ) );
		}
		CollectReachable( source, next, {}, keep );
	}

	// The original slides and their notes are exactly what we are replacing.
	for( auto it = keep.begin(); it != keep.end(); )
	{
		const bool isOriginalSlide = it->compare( 0, 12, "ppt/slides/s" ) == 0 ||
									 it->compare( 0, 17, "ppt/notesSlides/n" ) == 0 ||
									 it->find( "ppt/slides/_rels/" ) == 0 ||
									 it->find( "ppt/notesSlides/_rels/" ) == 0;
		if( isOriginalSlide )
			it = keep.erase( it );
		else
			++it;
	}

	// ---- 3. presentation.xml.rels: drop the old slides, add the new ones --------------
	std::string presRelsXml = ReadPart( source, "ppt/_rels/presentation.xml.rels" );
	std::vector< std::string > slideRelIds;
	{
		XmlDocument doc;
		std::string parseError;
		if( !doc.Parse( presRelsXml, parseError ) )
		{
			error = "ppt/_rels/presentation.xml.rels: " + parseError;
			return false;
		}
		TextSplicer splicer;
		for( size_t node : doc.Children( doc.Root(), "Relationship" ) )
			if( doc.AttributeOr( node, "Type", "" ) == kSlideRelType )
				splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );

		std::string additions;
		for( size_t n = 0; n < newSlides.size(); ++n )
		{
			const std::string id = "rIdLXSD" + std::to_string( n + 1 );
			slideRelIds.push_back( id );
			additions += "<Relationship Id=\"" + id + "\" Type=\"" + kSlideRelType +
						 "\" Target=\"slides/slide" + std::to_string( n + 1 ) + ".xml\"/>";
		}
		splicer.Insert( doc.Node( doc.Root() ).contentEnd, additions );

		std::string rewritten;
		if( !splicer.Apply( presRelsXml, rewritten ) )
		{
			error = "could not rewrite the presentation relationships";
			return false;
		}
		presRelsXml = std::move( rewritten );
	}

	// ---- 4. presentation.xml: rebuild the slide list ----------------------------------
	std::string presentationXml = ReadPart( source, "ppt/presentation.xml" );
	{
		XmlDocument doc;
		std::string parseError;
		if( !doc.Parse( presentationXml, parseError ) )
		{
			error = "ppt/presentation.xml: " + parseError;
			return false;
		}
		const size_t sldIdLst = doc.FirstChild( doc.Root(), "sldIdLst" );
		if( sldIdLst == XmlDocument::npos )
		{
			error = "ppt/presentation.xml has no slide list";
			return false;
		}

		std::string body;
		int id = 256;// PowerPoint requires slide ids at or above 256
		for( const std::string& relId : slideRelIds )
			body += "<p:sldId id=\"" + std::to_string( id++ ) + "\" r:id=\"" + relId + "\"" + kNsRels + "/>";

		TextSplicer splicer;
		if( doc.Node( sldIdLst ).selfClosing )
			splicer.Replace( doc.Node( sldIdLst ).start, doc.Node( sldIdLst ).end,
							 "<p:sldIdLst>" + body + "</p:sldIdLst>" );
		else
			splicer.Replace( doc.Node( sldIdLst ).contentStart, doc.Node( sldIdLst ).contentEnd, body );

		// A custom show would still point at slides that no longer exist.
		for( size_t node : doc.Children( doc.Root(), "custShowLst" ) )
			splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );

		std::string rewritten;
		if( !splicer.Apply( presentationXml, rewritten ) )
		{
			error = "could not rewrite the slide list";
			return false;
		}
		presentationXml = std::move( rewritten );
	}

	// ---- 5. [Content_Types].xml -------------------------------------------------------
	std::string contentTypes = ReadPart( source, "[Content_Types].xml" );
	{
		XmlDocument doc;
		std::string parseError;
		if( !doc.Parse( contentTypes, parseError ) )
		{
			error = "[Content_Types].xml: " + parseError;
			return false;
		}
		TextSplicer splicer;
		for( size_t node : doc.Children( doc.Root(), "Override" ) )
		{
			std::string partName = doc.AttributeOr( node, "PartName", "" );
			if( !partName.empty() && partName[ 0 ] == '/' )
				partName.erase( 0, 1 );
			if( keep.count( partName ) == 0 )
				splicer.Remove( doc.Node( node ).start, doc.Node( node ).end );
		}
		std::string additions;
		for( const NewSlide& slide : newSlides )
			additions += "<Override PartName=\"/" + slide.part + "\" ContentType=\"" + kSlideContentType + "\"/>";
		splicer.Insert( doc.Node( doc.Root() ).contentEnd, additions );

		std::string rewritten;
		if( !splicer.Apply( contentTypes, rewritten ) )
		{
			error = "could not rewrite the content types";
			return false;
		}
		contentTypes = std::move( rewritten );
	}

	// ---- 6. assemble ------------------------------------------------------------------
	out = ZipPackage{};
	out.Set( "[Content_Types].xml", contentTypes );// must come first in the archive
	for( const auto& entry : source.Entries() )
	{
		if( entry.first == "[Content_Types].xml" )
			continue;
		if( entry.first == "ppt/presentation.xml" || entry.first == "ppt/_rels/presentation.xml.rels" )
			continue;
		if( keep.count( entry.first ) == 0 )
			continue;
		out.Set( entry.first, entry.second );
	}
	out.Set( "ppt/presentation.xml", presentationXml );
	out.Set( "ppt/_rels/presentation.xml.rels", presRelsXml );
	for( const NewSlide& slide : newSlides )
	{
		out.Set( slide.part, slide.xml );
		if( !slide.relsXml.empty() )
			out.Set( slide.rels, slide.relsXml );
	}
	return true;
}
}// namespace lxsd
