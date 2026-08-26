// LX SlideDeck — unit tests for the parts that need neither PowerPoint nor a GL context.
// Deliberately dependency-free: one file, run it, non-zero exit means something broke.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "DeckLogic.h"
#include "LruPolicy.h"
#include "Manifest.h"
#include "ScaleMode.h"
#include "Sha1.h"

using namespace lxsd;

namespace
{
int g_failures = 0;
int g_checks   = 0;
std::string g_group;

void Group( const char* name )
{
	g_group = name;
	printf( "\n[%s]\n", name );
}

void Check( bool ok, const std::string& what )
{
	++g_checks;
	if( ok )
	{
		printf( "  ok   %s\n", what.c_str() );
		return;
	}
	++g_failures;
	printf( "  FAIL %s\n", what.c_str() );
}

void CheckNear( float got, float expected, const std::string& what, float eps = 1e-4f )
{
	const bool ok = std::fabs( got - expected ) <= eps;
	Check( ok, what + " (got " + std::to_string( got ) + ", expected " + std::to_string( expected ) + ")" );
}

//--------------------------------------------------------------------------------------
void TestSha1()
{
	Group( "sha1" );
	Check( Sha1Hex( "abc" ) == "a9993e364706816aba3e25717850c26c9cd0d89d", "abc" );
	Check( Sha1Hex( "" ) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "empty string" );
	Check( Sha1Hex( "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" ) ==
			   "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
		   "56 byte message (two block padding boundary)" );
	// 64 bytes exactly: the tail block must still be emitted.
	Check( Sha1Hex( std::string( 64, 'a' ) ) == "0098ba824b5c16427bd7a1122a5a442a25ec644d", "64 byte message" );
	Check( Sha1Short( "abc", 16 ) == "a9993e364706816a", "short digest is 16 chars" );
}

//--------------------------------------------------------------------------------------
void TestScaleModes()
{
	Group( "scale modes" );

	// 16:9 slide on a 16:9 viewport — every mode should be a no-op.
	for( ScaleMode m : { ScaleMode::Native, ScaleMode::Fit, ScaleMode::Fill, ScaleMode::Stretch } )
	{
		UvTransform t = ComputeUvTransform( m, 1920, 1080, 1920, 1080 );
		CheckNear( t.scaleX, 1.0f, "matching aspect: scaleX" );
		CheckNear( t.scaleY, 1.0f, "matching aspect: scaleY" );
		CheckNear( t.offX, 0.0f, "matching aspect: offX" );
		CheckNear( t.offY, 0.0f, "matching aspect: offY" );
	}

	// 16:9 slide on an ultrawide 32:9 comp.
	{
		UvTransform fit = ComputeUvTransform( ScaleMode::Fit, 1920, 1080, 3840, 1080 );
		CheckNear( fit.scaleX, 2.0f, "fit ultrawide: image covers half the width" );
		CheckNear( fit.offX, -0.5f, "fit ultrawide: centred horizontally" );
		CheckNear( fit.scaleY, 1.0f, "fit ultrawide: full height" );
		// Screen centre must land on the image centre.
		CheckNear( 0.5f * fit.scaleX + fit.offX, 0.5f, "fit ultrawide: centre maps to centre" );
		// Left edge of the screen must fall outside the image, i.e. become letterbox.
		Check( 0.0f * fit.scaleX + fit.offX < 0.0f, "fit ultrawide: left edge is letterbox" );

		UvTransform fill = ComputeUvTransform( ScaleMode::Fill, 1920, 1080, 3840, 1080 );
		CheckNear( fill.scaleX, 1.0f, "fill ultrawide: full width used" );
		CheckNear( fill.scaleY, 0.5f, "fill ultrawide: vertical crop to half" );
		CheckNear( fill.offY, 0.25f, "fill ultrawide: crop centred" );
		Check( 0.0f * fill.scaleY + fill.offY >= 0.0f, "fill ultrawide: never samples outside" );
		Check( 1.0f * fill.scaleY + fill.offY <= 1.0f, "fill ultrawide: never samples outside" );
	}

	// Native on a viewport half the size of the image: image overflows and gets cropped.
	{
		UvTransform t = ComputeUvTransform( ScaleMode::Native, 3840, 2160, 1920, 1080 );
		CheckNear( t.scaleX, 0.5f, "native: 1:1 pixels crop horizontally" );
		CheckNear( t.offX, 0.25f, "native: crop is centred" );
	}

	// Degenerate inputs must not produce NaNs.
	{
		UvTransform t = ComputeUvTransform( ScaleMode::Fit, 0, 0, 1920, 1080 );
		CheckNear( t.scaleX, 1.0f, "zero-sized image falls back to identity" );
	}
}

//--------------------------------------------------------------------------------------
EffectRecord Eff( int seq, int shapeId, int paragraph, bool exitEffect, int trigger )
{
	EffectRecord e;
	e.sequenceIndex = seq;
	e.shapeId       = shapeId;
	e.rawParagraph  = paragraph;
	e.isExit        = exitEffect;
	e.triggerType   = trigger;
	return e;
}

ShapeRecord Shape( int id, int index, int paragraphs )
{
	ShapeRecord s;
	s.shapeId        = id;
	s.shapeIndex     = index;
	s.hasTextFrame   = paragraphs > 0;
	s.paragraphCount = paragraphs;
	return s;
}

void TestStepSplitting()
{
	Group( "step splitting" );

	Check( SplitStepsByClick( {} ).size() == 1, "slide with no animation yields exactly one step" );

	// Four bullets, one click each: 4 effects -> 5 steps (SPEC §3.1).
	{
		std::vector< EffectRecord > effects;
		for( int i = 0; i < 4; ++i )
			effects.push_back( Eff( i + 1, 10, i + 1, false, kTriggerOnPageClick ) );
		auto steps = SplitStepsByClick( effects );
		Check( steps.size() == 5, "four bullets on click give five states (SPEC 3.1)" );
		Check( steps[ 0 ].empty(), "the deck arrives with nothing shown yet" );
		Check( steps[ 1 ].size() == 1 && steps[ 1 ][ 0 ] == 0, "the first click brings up line one" );
	}

	// WithPrevious / AfterPrevious join the step that is already open.
	{
		std::vector< EffectRecord > effects{
			Eff( 1, 10, 0, false, kTriggerOnPageClick ),
			Eff( 2, 11, 0, false, kTriggerWithPrevious ),
			Eff( 3, 12, 0, false, kTriggerAfterPrevious ),
			Eff( 4, 13, 0, false, kTriggerOnPageClick ),
		};
		auto steps = SplitStepsByClick( effects );
		Check( steps.size() == 3, "two clicks plus the base state" );
		Check( steps[ 0 ].empty(), "base state is empty" );
		Check( steps[ 1 ].size() == 3, "with/after previous join the click that opened the step" );
		Check( steps[ 2 ].size() == 1, "the second click opens its own step" );
	}
}

void TestParagraphBase()
{
	Group( "paragraph base detection" );

	// A shape with 4 paragraphs animated 1,2,3,4 cannot be 0-based.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 10, 1, 4 ) );
		for( int i = 1; i <= 4; ++i )
			s.effects.push_back( Eff( i, 10, i, false, kTriggerOnPageClick ) );
		auto v = DetectParagraphBase( { s } );
		Check( v.base == ParagraphBase::OneBased, "1..4 on a 4 paragraph shape reads as 1-based" );
	}

	// The same shape animated 0,1,2,3 cannot be 1-based.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 10, 1, 4 ) );
		for( int i = 0; i < 4; ++i )
			s.effects.push_back( Eff( i + 1, 10, i, false, kTriggerOnPageClick ) );
		auto v = DetectParagraphBase( { s } );
		Check( v.base == ParagraphBase::ZeroBased, "0..3 on a 4 paragraph shape reads as 0-based" );
	}

	// Nothing to go on: say so rather than guessing.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 10, 1, 0 ) );
		s.effects.push_back( Eff( 1, 10, 0, false, kTriggerOnPageClick ) );
		auto v = DetectParagraphBase( { s } );
		Check( v.base == ParagraphBase::Unknown, "a deck with no paragraph animation stays Unknown" );
		Check( v.Resolved( ParagraphBase::OneBased ) == ParagraphBase::OneBased, "Unknown falls back" );
	}
}

void TestVisibilityMap()
{
	Group( "visibility map" );

	// Four bullets appearing one per click.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 10, 1, 4 ) );
		for( int i = 1; i <= 4; ++i )
			s.effects.push_back( Eff( i, 10, i, false, kTriggerOnPageClick ) );

		auto steps = SplitStepsByClick( s.effects );
		auto map   = BuildVisibilityMap( s, steps, ParagraphBase::OneBased );
		Check( map.size() == 4, "one target per bullet" );

		// At step 0 only the first bullet is up; by the last step all four are.
		int visibleAtFirst = 0, visibleAtLast = 0;
		for( const auto& t : map )
		{
			if( VisibleAt( t, 0 ) )
				++visibleAtFirst;
			if( VisibleAt( t, static_cast< int >( steps.size() ) - 1 ) )
				++visibleAtLast;
		}
		Check( visibleAtFirst == 0, "no bullet visible before the first click" );
		Check( visibleAtLast == 4, "all four visible at the last step" );
	}

	// An exit effect takes something away again.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 20, 1, 0 ) );
		s.effects.push_back( Eff( 1, 20, 0, false, kTriggerOnPageClick ) );// enters on click 1
		s.effects.push_back( Eff( 2, 20, 0, true, kTriggerOnPageClick ) ); // leaves on click 2
		auto steps = SplitStepsByClick( s.effects );
		auto map   = BuildVisibilityMap( s, steps, ParagraphBase::OneBased );
		Check( map.size() == 1, "one target" );
		Check( !VisibleAt( map[ 0 ], 0 ), "not there before its entrance" );
		Check( VisibleAt( map[ 0 ], 1 ), "on screen after the first click" );
		Check( !VisibleAt( map[ 0 ], 2 ), "gone after the second click" );
	}

	// An emphasis effect after the entrance must not move the entrance (SPEC §3.4).
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 30, 1, 0 ) );
		s.effects.push_back( Eff( 1, 30, 0, false, kTriggerOnPageClick ) );
		s.effects.push_back( Eff( 2, 30, 0, false, kTriggerOnPageClick ) );
		s.effects.push_back( Eff( 3, 30, 0, false, kTriggerOnPageClick ) );
		auto steps = SplitStepsByClick( s.effects );
		auto map   = BuildVisibilityMap( s, steps, ParagraphBase::OneBased );
		Check( map.size() == 1, "all three effects hit one target" );
		Check( map[ 0 ].visibleFrom == 1, "the first effect is the entrance, later ones are ignored" );
	}

	// A shape that only ever exits was on screen from the start.
	{
		SlideRecord s;
		s.slideIndex = 1;
		s.shapes.push_back( Shape( 40, 1, 0 ) );
		s.effects.push_back( Eff( 1, 40, 0, false, kTriggerOnPageClick ) );
		s.effects.push_back( Eff( 2, 41, 0, true, kTriggerOnPageClick ) );
		s.shapes.push_back( Shape( 41, 2, 0 ) );
		auto steps = SplitStepsByClick( s.effects );
		auto map   = BuildVisibilityMap( s, steps, ParagraphBase::OneBased );
		for( const auto& t : map )
			if( t.key.shapeId == 41 )
			{
				Check( t.visibleFrom == 0, "exit-only target starts visible" );
				Check( VisibleAt( t, 1 ), "exit-only target is still there before its exit" );
				Check( !VisibleAt( t, 2 ), "exit-only target is gone from its exit step on" );
			}
	}

	// Text whose fill is a gradient cannot be hidden per paragraph, so the whole shape goes.
	{
		SlideRecord s;
		s.slideIndex     = 1;
		ShapeRecord sh   = Shape( 50, 1, 3 );
		sh.paragraphFillSolid = false;
		s.shapes.push_back( sh );
		s.effects.push_back( Eff( 1, 50, 2, false, kTriggerOnPageClick ) );
		auto steps = SplitStepsByClick( s.effects );
		auto map   = BuildVisibilityMap( s, steps, ParagraphBase::OneBased );
		Check( map.size() == 1 && map[ 0 ].key.paragraph == 0,
			   "non-solid text fill falls back to hiding the whole shape" );
	}
}

void TestTotalStepCount()
{
	Group( "total step count" );
	SlideRecord a;
	a.effects.push_back( Eff( 1, 1, 0, false, kTriggerOnPageClick ) );
	a.effects.push_back( Eff( 2, 2, 0, false, kTriggerOnPageClick ) );
	SlideRecord b;// no animation
	Check( TotalStepCount( { a, b } ) == 4, "base plus two clicks, plus one static slide" );
}

//--------------------------------------------------------------------------------------
void TestLru()
{
	Group( "lru eviction" );

	std::vector< LruEntry > entries{
		{ 1, 100 }, { 2, 200 }, { 3, 50 }, { 4, 400 }, { 5, 10 },
	};
	{
		auto evict = SelectLruEvictions( entries, 5, {} );
		Check( evict.empty(), "nothing to evict when under capacity" );
	}
	{
		auto evict = SelectLruEvictions( entries, 3, {} );
		Check( evict.size() == 2, "two evictions to get from five to three" );
		Check( evict[ 0 ] == 5 && evict[ 1 ] == 3, "oldest first" );
	}
	{
		// The step being shown and the one being faded from must survive.
		auto evict = SelectLruEvictions( entries, 3, { 5, 3 } );
		Check( evict.size() == 2, "still two evictions" );
		Check( evict[ 0 ] == 1 && evict[ 1 ] == 2, "protected keys are skipped" );
	}
	{
		std::vector< LruEntry > tie{ { 7, 5 }, { 2, 5 }, { 9, 5 }, { 1, 9 } };
		auto evict = SelectLruEvictions( tie, 2, {} );
		Check( evict.size() == 2 && evict[ 0 ] == 2 && evict[ 1 ] == 7, "ties break on key, stably" );
	}
}

//--------------------------------------------------------------------------------------
void TestManifest()
{
	Group( "manifest" );

	DeckManifest m;
	m.stepCount   = 48;
	m.width       = 3840;
	m.height      = 2160;
	m.exportWidth = 3840;
	m.sourceMtime = 1724668800;
	m.sourcePath  = "C:\\decks\\show \"final\".pptx";
	m.renderer    = "libreoffice";
	m.warning     = "line 1\nline 2";
	m.slideOfStep = { 1, 1, 2, 3, 3, 3 };

	DeckManifest back;
	const std::string json = SerializeManifest( m );
	Check( ParseManifest( json, back ), "round trip parses" );
	Check( back.stepCount == m.stepCount, "stepCount" );
	Check( back.width == m.width && back.height == m.height, "pixel size" );
	Check( back.exportWidth == m.exportWidth, "exportWidth" );
	Check( back.sourceMtime == m.sourceMtime, "mtime" );
	Check( back.sourcePath == m.sourcePath, "path with backslashes and quotes survives" );
	Check( back.renderer == m.renderer, "renderer" );
	Check( back.warning == m.warning, "newline in warning survives" );
    Check( back.slideOfStep == m.slideOfStep, "slide-of-step array" );

	DeckManifest broken;
	Check( !ParseManifest( "{ not json at all", broken ), "garbage is rejected" );
	Check( !ParseManifest( "{ \"stepCount\": 0 }", broken ), "an empty deck is rejected" );
}
}// namespace

int main()
{
	TestSha1();
	TestScaleModes();
	TestStepSplitting();
	TestParagraphBase();
	TestVisibilityMap();
	TestTotalStepCount();
	TestLru();
	TestManifest();

	printf( "\n%d checks, %d failures\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}
