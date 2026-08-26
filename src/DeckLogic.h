// LX SlideDeck — SPEC §3.3 / §3.4, expressed as pure data so it can be unit tested
// without PowerPoint. The COM layer (PptxConverter) does nothing but fill these
// records in and then act on the answers computed here.
#pragma once

#include <climits>
#include <string>
#include <vector>

namespace lxsd
{
// MsoAnimTriggerType. Values cross-checked against Microsoft's documented enum (SPEC §3.3).
// NOTE: there is no such constant as msoAnimTriggerOnClick — the one that opens a new
// step is msoAnimTriggerOnPageClick == 1. Getting this wrong collapses every effect of a
// slide into a single step, silently.
enum MsoAnimTriggerType : int
{
	kTriggerMixed           = -1,
	kTriggerNone            = 0,
	kTriggerOnPageClick     = 1,
	kTriggerWithPrevious    = 2,
	kTriggerAfterPrevious   = 3,
	kTriggerOnShapeClick    = 4,
	kTriggerOnMediaBookmark = 5,
};

// MsoTriState. Effect.Exit and Shape.Visible are tri-states, not booleans (SPEC §3.4).
constexpr int kMsoTrue  = -1;
constexpr int kMsoFalse = 0;

struct ShapeRecord
{
	int shapeId            = 0;   //!< Shape.Id — stable within a slide, survives duplication.
	int shapeIndex         = 0;   //!< 1-based index into Slide.Shapes.
	bool hasTextFrame      = false;
	int paragraphCount     = 0;   //!< 0 when the shape has no text frame.
	bool paragraphFillSolid = true;//!< false => Font.Fill.Transparency cannot hide it (SPEC §3.4 warning).
};

struct EffectRecord
{
	int sequenceIndex = 0;                  //!< 1-based index in MainSequence.
	int shapeId       = 0;                  //!< Effect.Shape.Id
	int rawParagraph  = 0;                  //!< Effect.Paragraph, verbatim. Base is deck-dependent, see below.
	bool isExit       = false;              //!< ( Effect.Exit == msoTrue )
	int triggerType   = kTriggerOnPageClick;//!< Effect.Timing.TriggerType
};

struct SlideRecord
{
	int slideIndex = 0;//!< 1-based index in Presentation.Slides
	std::vector< ShapeRecord > shapes;
	std::vector< EffectRecord > effects;//!< in MainSequence order; empty when the slide has no animation
};

//--------------------------------------------------------------------------------------
// Step splitting (SPEC §3.3)
//--------------------------------------------------------------------------------------

/// Groups effect indices (into `effects`) per animation step.
/// Step 0 is always present and is the slide's base state: whatever is on screen before
/// the first click. Effects that are WithPrevious / AfterPrevious join the current step.
/// A slide with no effects yields exactly one (empty) step.
std::vector< std::vector< int > > SplitStepsByClick( const std::vector< EffectRecord >& effects );

//--------------------------------------------------------------------------------------
// Effect.Paragraph base detection (SPEC §3.4 / §10 — the one undocumented thing)
//--------------------------------------------------------------------------------------

enum class ParagraphBase : int
{
	Unknown   = 0,
	ZeroBased = 1,//!< Effect.Paragraph 0 is the first paragraph
	OneBased  = 2,//!< Effect.Paragraph 1 is the first paragraph, 0 means "whole shape"
};

struct ParagraphBaseVerdict
{
	ParagraphBase base = ParagraphBase::Unknown;
	int votesZero      = 0;
	int votesOne       = 0;
	std::string reason;

	ParagraphBase Resolved( ParagraphBase fallback ) const
	{
		return base == ParagraphBase::Unknown ? fallback : base;
	}
};

/// Infers the indexing base from the deck itself, by comparing the paragraph numbers a
/// shape's effects use against how many paragraphs that shape actually has.
///
/// Two decisive signals:
///   * an effect referencing paragraph N on a shape that has exactly N paragraphs
///     cannot be 0-based (0-based would be addressing paragraph N+1)  -> 1-based
///   * a contiguous run 0..k on a shape with more than one paragraph
///     cannot be 1-based (1-based reserves 0 for "whole shape")       -> 0-based
///
/// Returns Unknown when the deck carries no paragraph-level animation at all, which is
/// fine: with no paragraph targets the base never gets used.
ParagraphBaseVerdict DetectParagraphBase( const std::vector< SlideRecord >& slides );

//--------------------------------------------------------------------------------------
// Targets and visibility (SPEC §3.4)
//--------------------------------------------------------------------------------------

/// A thing an effect can make appear or disappear.
struct TargetKey
{
	int shapeId   = 0;
	int paragraph = 0;//!< 0 = the whole shape, otherwise 1-based paragraph number

	bool operator==( const TargetKey& o ) const
	{
		return shapeId == o.shapeId && paragraph == o.paragraph;
	}
	bool operator<( const TargetKey& o ) const
	{
		return shapeId != o.shapeId ? shapeId < o.shapeId : paragraph < o.paragraph;
	}
};

struct TargetVisibility
{
	TargetKey key;
	int visibleFrom  = 0;      //!< first step at which the target is on screen
	int visibleUntil = INT_MAX;//!< first step at which it is gone again
};

inline bool VisibleAt( const TargetVisibility& t, int step )
{
	return step >= t.visibleFrom && step < t.visibleUntil;
}

/// Turns one effect's raw Effect.Paragraph into our internal convention
/// (0 = whole shape, >=1 = 1-based paragraph), given the detected base.
///
/// Under a 0-based reading, a lone `0` is ambiguous: it is either "first paragraph" or
/// "the whole shape animated as one object", and PowerPoint reports both the same way.
/// We resolve it to the whole shape, because hiding a whole shape one step early is a
/// far less visible mistake than leaving a bullet on screen that should not be there.
int NormalizeParagraph( int rawParagraph,
						const ShapeRecord& shape,
						const std::vector< int >& rawValuesOnThisShape,
						ParagraphBase base );

/// Builds the visibility map for one slide.
///
/// HEURISTIC (SPEC §3.4) — deliberate trade-off, not a bug:
/// the FIRST non-exit effect on a target counts as its entrance; every later non-exit
/// effect on the same target is treated as emphasis and ignored. PowerPoint's EffectType
/// enum does distinguish entrance from emphasis, but the mapping is large, versioned and
/// full of special cases; this rule is right for the overwhelming majority of real decks
/// and costs one thing when it is wrong: an emphasis-only shape is treated as entering
/// at that step, i.e. it appears late instead of being on screen from step 0.
///
/// Targets never touched by any effect are static and are not returned — the caller
/// leaves them alone, so they show on every step.
std::vector< TargetVisibility > BuildVisibilityMap( const SlideRecord& slide,
												   const std::vector< std::vector< int > >& steps,
												   ParagraphBase base );

/// Total number of exported images for a whole deck.
int TotalStepCount( const std::vector< SlideRecord >& slides );
}// namespace lxsd
