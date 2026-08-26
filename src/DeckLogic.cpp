#include "DeckLogic.h"

#include <algorithm>
#include <map>
#include <set>

namespace lxsd
{
std::vector< std::vector< int > > SplitStepsByClick( const std::vector< EffectRecord >& effects )
{
	std::vector< std::vector< int > > steps;
	steps.emplace_back();// step 0 = the slide's base state, always exists

	for( int i = 0; i < static_cast< int >( effects.size() ); ++i )
	{
		// Every click opens a new step, the first one included.
		//
		// SPEC contradicts itself here: §3.1 says four bullets give five states, which is
		// what PowerPoint actually does — you arrive on the slide with nothing shown and the
		// first click brings up line one. The pseudo-code in §3.3 says `i > 1`, which would
		// fold that first click into the base state and lose the empty state entirely.
		// §3.1 wins: it describes the behaviour a presenter sees.
		//
		// Effects that are WithPrevious / AfterPrevious before any click play by themselves
		// on slide entry, so their end state IS the base state and they stay in step 0.
		if( effects[ i ].triggerType == kTriggerOnPageClick )
			steps.emplace_back();
		steps.back().push_back( i );
	}
	return steps;
}

namespace
{
/// raw Effect.Paragraph values seen per shape, in first-seen order, deduplicated.
std::map< int, std::vector< int > > RawValuesPerShape( const SlideRecord& slide )
{
	std::map< int, std::vector< int > > byShape;
	for( const EffectRecord& e : slide.effects )
	{
		std::vector< int >& v = byShape[ e.shapeId ];
		if( std::find( v.begin(), v.end(), e.rawParagraph ) == v.end() )
			v.push_back( e.rawParagraph );
	}
	return byShape;
}

const ShapeRecord* FindShape( const SlideRecord& slide, int shapeId )
{
	for( const ShapeRecord& s : slide.shapes )
		if( s.shapeId == shapeId )
			return &s;
	return nullptr;
}
}// namespace

ParagraphBaseVerdict DetectParagraphBase( const std::vector< SlideRecord >& slides )
{
	ParagraphBaseVerdict verdict;

	for( const SlideRecord& slide : slides )
	{
		for( const auto& entry : RawValuesPerShape( slide ) )
		{
			const ShapeRecord* shape = FindShape( slide, entry.first );
			if( shape == nullptr || !shape->hasTextFrame || shape->paragraphCount < 2 )
				continue;// nothing to compare against

			std::vector< int > vals = entry.second;
			std::sort( vals.begin(), vals.end() );
			const int lo = vals.front();
			const int hi = vals.back();
			const int n  = static_cast< int >( vals.size() );

			// Decisive: paragraph number equal to the paragraph count. Under a 0-based
			// reading that would address one paragraph past the end.
			if( hi == shape->paragraphCount )
			{
				++verdict.votesOne;
				continue;
			}
			// Decisive the other way: a contiguous run starting at 0 covering more than one
			// value. Under a 1-based reading, 0 means "whole shape", and a shape would not be
			// animated both as a whole and per paragraph in one contiguous run.
			if( lo == 0 && n >= 2 && hi == n - 1 )
			{
				++verdict.votesZero;
				continue;
			}
			// Weaker: a run 1..n on a shape with at least n paragraphs.
			if( lo == 1 && n >= 2 && hi == n && shape->paragraphCount >= n )
				++verdict.votesOne;
		}
	}

	if( verdict.votesOne > verdict.votesZero )
	{
		verdict.base   = ParagraphBase::OneBased;
		verdict.reason = "paragraph numbers reach the shape's paragraph count";
	}
	else if( verdict.votesZero > verdict.votesOne )
	{
		verdict.base   = ParagraphBase::ZeroBased;
		verdict.reason = "paragraph numbers form a contiguous run starting at 0";
	}
	else if( verdict.votesOne > 0 )
	{
		// A tie with evidence on both sides means the deck is inconsistent; say so rather
		// than picking a winner silently.
		verdict.reason = "conflicting evidence";
	}
	else
	{
		verdict.reason = "no paragraph-level animation found";
	}
	return verdict;
}

int NormalizeParagraph( int rawParagraph,
						const ShapeRecord& shape,
						const std::vector< int >& rawValuesOnThisShape,
						ParagraphBase base )
{
	// No text, or a single paragraph: nothing to address but the shape itself.
	if( !shape.hasTextFrame || shape.paragraphCount < 2 )
		return 0;
	// A shape whose text cannot be hidden by transparency has to be hidden whole
	// (SPEC §3.4: gradient / pattern / texture text fill).
	if( !shape.paragraphFillSolid )
		return 0;

	if( base == ParagraphBase::ZeroBased )
	{
		int distinct = 0;
		int hi       = 0;
		for( int v : rawValuesOnThisShape )
		{
			++distinct;
			hi = std::max( hi, v );
		}
		// Lone 0 on this shape: ambiguous between "first paragraph" and "whole shape".
		// Resolve to whole shape — see NormalizeParagraph's contract in the header.
		if( distinct < 2 && hi == 0 )
			return 0;
		if( rawParagraph < 0 )
			return 0;
		return rawParagraph + 1;
	}

	// OneBased (and the Unknown fallback the caller passes in): 0 means the whole shape.
	if( rawParagraph <= 0 )
		return 0;
	return rawParagraph;
}

std::vector< TargetVisibility > BuildVisibilityMap( const SlideRecord& slide,
												   const std::vector< std::vector< int > >& steps,
												   ParagraphBase base )
{
	const std::map< int, std::vector< int > > rawPerShape = RawValuesPerShape( slide );

	std::map< TargetKey, TargetVisibility > map;
	std::set< TargetKey > hasEntrance;

	for( int stepIdx = 0; stepIdx < static_cast< int >( steps.size() ); ++stepIdx )
	{
		for( int effIdx : steps[ stepIdx ] )
		{
			if( effIdx < 0 || effIdx >= static_cast< int >( slide.effects.size() ) )
				continue;
			const EffectRecord& eff  = slide.effects[ effIdx ];
			const ShapeRecord* shape = FindShape( slide, eff.shapeId );
			if( shape == nullptr )
				continue;// effect on something we cannot address; leave it visible

			auto it = rawPerShape.find( eff.shapeId );
			const std::vector< int > empty;
			const int paragraph = NormalizeParagraph( eff.rawParagraph,
													  *shape,
													  it != rawPerShape.end() ? it->second : empty,
													  base );

			TargetKey key{ eff.shapeId, paragraph };
			auto& vis = map[ key ];
			vis.key   = key;

			if( eff.isExit )
			{
				// First exit wins: once it is gone it stays gone.
				if( vis.visibleUntil == INT_MAX )
					vis.visibleUntil = stepIdx;
				// An exit on a target that never had an entrance means it was on screen
				// from the start.
				if( hasEntrance.find( key ) == hasEntrance.end() )
					vis.visibleFrom = 0;
			}
			else
			{
				// HEURISTIC (SPEC §3.4): first non-exit effect = entrance, later ones =
				// emphasis and ignored. See BuildVisibilityMap's contract in the header.
				if( hasEntrance.find( key ) == hasEntrance.end() )
				{
					vis.visibleFrom = stepIdx;
					hasEntrance.insert( key );
				}
			}
		}
	}

	std::vector< TargetVisibility > out;
	out.reserve( map.size() );
	for( const auto& kv : map )
		out.push_back( kv.second );
	return out;
}

int TotalStepCount( const std::vector< SlideRecord >& slides )
{
	int total = 0;
	for( const SlideRecord& s : slides )
		total += static_cast< int >( SplitStepsByClick( s.effects ).size() );
	return total;
}
}// namespace lxsd
