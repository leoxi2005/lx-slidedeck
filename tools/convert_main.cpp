// LX SlideDeck — command line front end to the converter.
//
// The plugin never runs this; it exists so the conversion can be exercised, timed and
// diffed without Resolume in the way.
//
//   lxsd-convert --analyze deck.pptx
//   lxsd-convert deck.pptx /tmp/out 1920
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Converter.h"
#include "Manifest.h"
#include "Platform.h"
#include "Pptx.h"
#include "Zip.h"

using namespace lxsd;

namespace
{
int Analyze( const std::string& path )
{
	ZipPackage package;
	std::string error;
	if( !ReadZip( path, package, error ) )
	{
		printf( "error: %s\n", error.c_str() );
		return 1;
	}
	printf( "package: %zu parts\n", package.Size() );

	DeckPlan plan;
	if( !AnalyzePptx( package, plan, error ) )
	{
		printf( "error: %s\n", error.c_str() );
		return 1;
	}

	printf( "slide size: %d x %d emu (aspect %.4f)\n",
			plan.slideWidthEmu, plan.slideHeightEmu, plan.AspectRatio() );
	printf( "steps: %zu\n", plan.steps.size() );
	for( size_t i = 0; i < plan.steps.size(); ++i )
	{
		const StepPlan& s = plan.steps[ i ];
		printf( "  step %2zu  slide %d.%d  hide shapes[", i + 1, s.sourceSlide, s.stepInSlide );
		for( size_t n = 0; n < s.hiddenShapeIds.size(); ++n )
			printf( "%s%d", n ? "," : "", s.hiddenShapeIds[ n ] );
		printf( "] paragraphs[" );
		for( size_t n = 0; n < s.hiddenParagraphs.size(); ++n )
			printf( "%s%d:%d", n ? "," : "", s.hiddenParagraphs[ n ].first, s.hiddenParagraphs[ n ].second );
		printf( "]  part=%s\n", s.slidePart.c_str() );
	}
	for( const std::string& w : plan.warnings )
		printf( "warning: %s\n", w.c_str() );
	return 0;
}
}// namespace

int main( int argc, char** argv )
{
	std::vector< std::string > args( argv + 1, argv + argc );
	if( args.empty() )
	{
		printf( "usage: lxsd-convert [--analyze] <deck.pptx> [outdir] [width]\n" );
		return 2;
	}
	if( args[ 0 ] == "--build-step" )
	{
		// Writes one step's .pptx so it can be unzipped and inspected by hand.
		if( args.size() < 4 )
		{
			printf( "usage: lxsd-convert --build-step <deck.pptx> <step 1-based> <out.pptx>\n" );
			return 2;
		}
		ZipPackage package;
		std::string error;
		if( !ReadZip( args[ 1 ], package, error ) )
		{
			printf( "error: %s\n", error.c_str() );
			return 1;
		}
		DeckPlan plan;
		if( !AnalyzePptx( package, plan, error ) )
		{
			printf( "error: %s\n", error.c_str() );
			return 1;
		}
		ZipPackage out;
		const int index = std::atoi( args[ 2 ].c_str() ) - 1;
		if( !BuildStepPackage( package, plan, { index }, out, error ) )
		{
			printf( "error: %s\n", error.c_str() );
			return 1;
		}
		if( !WriteZip( args[ 3 ], out, error ) )
		{
			printf( "error: %s\n", error.c_str() );
			return 1;
		}
		printf( "wrote %s (%zu parts)\n", args[ 3 ].c_str(), out.Size() );
		for( const auto& e : out.Entries() )
			printf( "   %8zu  %s\n", e.second.size(), e.first.c_str() );
		return 0;
	}
	if( args[ 0 ] == "--analyze" )
	{
		if( args.size() < 2 )
		{
			printf( "usage: lxsd-convert --analyze <deck.pptx>\n" );
			return 2;
		}
		return Analyze( args[ 1 ] );
	}

	const std::string input  = AbsolutePath( args[ 0 ] );
	const std::string outDir = args.size() > 1 ? AbsolutePath( args[ 1 ] ) : JoinPath( CacheRoot(), "cli" );
	const int width          = args.size() > 2 ? std::atoi( args[ 2 ].c_str() ) : 1920;

	std::string why;
	const RenderBackend backend = SelectRenderBackend( RenderBackend::Auto, why );
	printf( "renderer: %s%s\n",
			backend == RenderBackend::PowerPoint ? "powerpoint" : backend == RenderBackend::LibreOffice ? "libreoffice" : "none",
			why.empty() ? "" : ( " (" + why + ")" ).c_str() );

	ConvertRequest request;
	request.sourcePath  = input;
	request.cacheDir    = outDir;
	request.exportWidth = width;

	ConvertCallbacks callbacks;
	callbacks.progress = []( int done, int total, const std::string& note ) {
		printf( "  %d/%d %s\n", done, total, note.c_str() );
		fflush( stdout );
	};

	DeckManifest manifest;
	std::string error;
	if( !ConvertDeck( request, callbacks, manifest, error ) )
	{
		printf( "error: %s\n", error.c_str() );
		return 1;
	}

	printf( "done: %d steps at %dx%d in %s\n",
			manifest.stepCount, manifest.width, manifest.height, outDir.c_str() );
	return 0;
}
