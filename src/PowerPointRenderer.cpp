// LX SlideDeck — the Windows renderer: PowerPoint exports the slides.
//
// This is a much smaller job than SPEC §3 assumed. The spec had COM read the whole
// animation model — MainSequence, Effect.Paragraph, Effect.Exit, TriggerType — and that is
// where all four of its documented COM traps lived. Here the animation has already been
// worked out by reading the file's own XML (see Pptx.cpp), and PowerPoint is handed a
// finished deck in which every step is simply one static slide. All that is left is:
// open, export each slide, close.
//
// What that removes: the msoAnimTriggerOnPageClick naming trap, the MainSequence-throws-
// when-empty trap, the MsoTriState-is-not-a-bool trap, and the paragraph indexing question
// Microsoft never documented. None of them can be got wrong here because none of them are
// used.
#if defined( _WIN32 )

#	include <algorithm>
#	include <chrono>
#	include <cstdio>
#	include <string>

#	include "ComDispatch.h"
#	include "Converter.h"
#	include "Platform.h"

namespace lxsd
{
using com::Dispatch;
using com::Variant;

namespace
{
std::string StepImageName( int index1Based )
{
	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "step_%04d.png", index1Based );
	return buf;
}

/// PowerPoint is happier with native separators.
std::wstring WindowsPath( const std::string& path )
{
	std::wstring wide = com::Widen( path );
	std::replace( wide.begin(), wide.end(), L'/', L'\\' );
	return wide;
}

/// Hides the PowerPoint window if the one we opened is the only presentation there is.
///
/// SPEC §3.7: PowerPoint has no Application.HWND — asking for one is error 438, unlike
/// Excel and Word. The window has to be found through Win32 by its class name. And if the
/// user already had PowerPoint open, that same call would find THEIR window, so this only
/// runs when ours is the only presentation open.
void HideWindowIfOurs( long presentationCount )
{
	if( presentationCount != 1 )
		return;
	HWND window = FindWindowW( L"PPTFrameClass", nullptr );
	if( window != nullptr )
		ShowWindow( window, SW_HIDE );
}
}// namespace

bool IsPowerPointAvailable()
{
	return com::Dispatch::IsRegistered( L"PowerPoint.Application" );
}

bool RenderWithPowerPoint( const std::string& packagePath,
						   int stepCount,
						   int& width,
						   int& height,
						   const std::string& outputDir,
						   const ConvertCallbacks& callbacks,
						   int timeoutSeconds,
						   std::string& error )
{
	if( !EnsureDirectory( outputDir ) )
	{
		error = "cannot create the cache folder";
		return false;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( timeoutSeconds );

	Dispatch app;
	Dispatch presentations;
	Dispatch presentation;
	bool opened = false;

	try
	{
		app = Dispatch::CreateObject( L"PowerPoint.Application" );

		// SPEC §3.7: setting Visible to msoFalse makes a lot of later calls fail, so leave
		// the application visible and open the presentation without a window instead.
		app.Put( L"Visible", Variant::Long( com::kMsoTrue ) );

		presentations = app.GetObject( L"Presentations" );

		// The file being opened is one we generated, not the user's deck, but open it
		// read-only and untitled anyway: it costs nothing and it means no code path here
		// can ever write to a .pptx.
		presentation = Dispatch( presentations
									 .Call( L"Open",
											{ Variant::Text( WindowsPath( packagePath ) ),
											  Variant::Long( com::kMsoTrue ), // ReadOnly
											  Variant::Long( com::kMsoTrue ), // Untitled
											  Variant::Long( com::kMsoFalse ) } )// WithWindow
									 .AsDispatch(),
								 /*addReference*/ true );
		if( !presentation.Valid() )
		{
			error = "PowerPoint could not open the generated deck";
			return false;
		}
		opened = true;

		HideWindowIfOurs( presentations.GetLong( L"Count" ) );

		Dispatch slides = presentation.GetObject( L"Slides" );
		const long slideCount = slides.GetLong( L"Count" );
		if( slideCount < stepCount )
			stepCount = static_cast< int >( slideCount );

		// SPEC §3.5 warns that very large exports fail. Rather than discovering that
		// halfway through and ending up with a deck of mixed sizes, settle the resolution
		// on the first slide and use it for all of them.
		int chosenWidth  = width;
		int chosenHeight = height;
		{
			const int ladder[] = { width, width / 2, 1920 };
			bool exported      = false;
			std::string lastFailure;
			for( int candidate : ladder )
			{
				if( candidate < 320 )
					continue;
				const int candidateHeight = static_cast< int >(
					static_cast< double >( candidate ) * height / std::max( 1, width ) + 0.5 );
				try
				{
					Dispatch first = slides.GetObject( L"Item", { Variant::Long( 1 ) } );
					first.Call( L"Export", { Variant::Text( WindowsPath( JoinPath( outputDir, StepImageName( 1 ) ) ) ),
											 Variant::Text( L"PNG" ),
											 Variant::Long( candidate ),
											 Variant::Long( candidateHeight ) } );
					chosenWidth  = candidate;
					chosenHeight = candidateHeight;
					exported     = true;
					break;
				}
				catch( const com::ComError& e )
				{
					lastFailure = e.what();
				}
			}
			if( !exported )
			{
				error = "PowerPoint could not export a slide (" + lastFailure + ")";
				presentation.Call( L"Close" );
				return false;
			}
		}
		width  = chosenWidth;
		height = chosenHeight;

		if( callbacks.progress )
			callbacks.progress( 1, stepCount, "exporting" );

		for( int index = 2; index <= stepCount; ++index )
		{
			if( callbacks.cancelled && callbacks.cancelled() )
			{
				error = "cancelled";
				presentation.Call( L"Close" );
				return false;
			}
			if( std::chrono::steady_clock::now() > deadline )
			{
				error = "PowerPoint timed out";
				presentation.Call( L"Close" );
				return false;
			}

			Dispatch slide = slides.GetObject( L"Item", { Variant::Long( index ) } );
			slide.Call( L"Export", { Variant::Text( WindowsPath( JoinPath( outputDir, StepImageName( index ) ) ) ),
									 Variant::Text( L"PNG" ),
									 Variant::Long( chosenWidth ),
									 Variant::Long( chosenHeight ) } );

			if( callbacks.progress && ( index % 4 == 0 || index == stepCount ) )
				callbacks.progress( index, stepCount, "exporting" );
		}

		// Never save, and never close a presentation we did not open.
		presentation.Call( L"Close" );
		opened = false;

		// SPEC §3.7: if the user had PowerPoint open with their own work, quitting it would
		// take their windows down with ours.
		if( presentations.GetLong( L"Count" ) == 0 )
			app.Call( L"Quit" );

		return true;
	}
	catch( const com::ComError& e )
	{
		error = std::string( "PowerPoint: " ) + e.what();
	}
	catch( const std::exception& e )
	{
		error = std::string( "PowerPoint: " ) + e.what();
	}
	catch( ... )
	{
		error = "PowerPoint: unknown failure";
	}

	// Best effort cleanup — a failure must not leave a hidden presentation open forever.
	try
	{
		if( opened && presentation.Valid() )
			presentation.Call( L"Close" );
		if( presentations.Valid() && presentations.GetLong( L"Count" ) == 0 && app.Valid() )
			app.Call( L"Quit" );
	}
	catch( ... )
	{
	}
	return false;
}
}// namespace lxsd

#endif// _WIN32
