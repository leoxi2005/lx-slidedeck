#include "Converter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "Platform.h"
#include "Pptx.h"
#include "Zip.h"

namespace lxsd
{
/// Implemented on Windows in PowerPointRenderer.cpp; the stub elsewhere just says no.
bool RenderWithPowerPoint( const std::string& packagePath,
						   int stepCount,
						   int width,
						   int height,
						   const std::string& outputDir,
						   const ConvertCallbacks& callbacks,
						   int timeoutSeconds,
						   std::string& error );
bool IsPowerPointAvailable();

namespace
{
std::string StepImageName( int index1Based )
{
	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "step_%04d.png", index1Based );
	return buf;
}

std::string StepPackageName( int index1Based )
{
	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "step_%04d.pptx", index1Based );
	return buf;
}

bool Cancelled( const ConvertCallbacks& callbacks )
{
	return callbacks.cancelled && callbacks.cancelled();
}

void Report( const ConvertCallbacks& callbacks, int done, int total, const std::string& note )
{
	if( callbacks.progress )
		callbacks.progress( done, total, note );
}

/// LibreOffice writes one png per input file, named after the input.
/// Converting in chunks keeps progress moving and stops one bad slide from taking the
/// whole deck down with it.
bool RenderWithLibreOffice( const std::string& soffice,
							const std::vector< std::string >& packagePaths,
							int width,
							int height,
							const std::string& workDir,
							const std::string& outputDir,
							const ConvertCallbacks& callbacks,
							int timeoutSeconds,
							std::string& error )
{
	// A private profile so this never fights with a LibreOffice the user has open —
	// without it soffice hands the job to the running instance and returns immediately.
	const std::string profileDir = JoinPath( workDir, "loprofile" );
	EnsureDirectory( profileDir );
	EnsureDirectory( outputDir );

	char filter[ 256 ];
	snprintf( filter, sizeof( filter ),
			  "png:impress_png_Export:{\"PixelWidth\":{\"type\":\"long\",\"value\":%d},"
			  "\"PixelHeight\":{\"type\":\"long\",\"value\":%d}}",
			  width, height );

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( timeoutSeconds );
	const size_t chunkSize = 8;
	const int total        = static_cast< int >( packagePaths.size() );

	for( size_t start = 0; start < packagePaths.size(); start += chunkSize )
	{
		if( Cancelled( callbacks ) )
		{
			error = "cancelled";
			return false;
		}

		const auto remaining = std::chrono::duration_cast< std::chrono::seconds >(
								   deadline - std::chrono::steady_clock::now() )
								   .count();
		if( remaining <= 0 )
		{
			error = "timed out while rendering";
			return false;
		}

		std::vector< std::string > args{
			"--headless",
			"--norestore",
			"--invisible",
			"--nolockcheck",
			"-env:UserInstallation=file://" + profileDir,
			"--convert-to",
			filter,
			"--outdir",
			outputDir,
		};
		const size_t stop = std::min( packagePaths.size(), start + chunkSize );
		for( size_t i = start; i < stop; ++i )
			args.push_back( packagePaths[ i ] );

		int exitCode = -1;
		if( !RunProcess( soffice, args, static_cast< int >( remaining ), exitCode ) )
		{
			error = "LibreOffice did not finish (timed out or could not start)";
			return false;
		}
		if( exitCode != 0 )
		{
			error = "LibreOffice failed with exit code " + std::to_string( exitCode );
			return false;
		}

		Report( callbacks, static_cast< int >( stop ), total, "rendering" );
	}
	return true;
}
}// namespace

namespace
{
RendererStatus g_renderer;
std::mutex g_rendererMutex;
bool g_rendererProbed = false;

RendererStatus RunProbe()
{
	RendererStatus status;

	if( IsPowerPointAvailable() )
	{
		status.backend = RenderBackend::PowerPoint;
		status.usable  = true;
		status.name    = "PowerPoint";
		return status;
	}

	const std::string soffice = FindLibreOffice();
	if( soffice.empty() )
	{
		status.problem = "LibreOffice not installed — install it, or point Deck File at a "
						 "folder of PNGs instead";
		return status;
	}

	status.backend = RenderBackend::LibreOffice;
	status.path    = soffice;

	// Actually start it. `--version` prints one line and exits, so it is cheap (well under
	// a second here) and it proves the binary runs on this machine.
	EnsureDirectory( TempRoot() );
	const std::string outPath = JoinPath( TempRoot(), "renderer_probe.txt" );
	int exitCode              = -1;
	if( !RunProcess( soffice, { "--version" }, 60, exitCode, &outPath ) )
	{
		status.problem = "LibreOffice found but would not start (" + soffice + ")";
		return status;
	}
	if( exitCode != 0 )
	{
		status.problem = "LibreOffice failed to run, exit code " + std::to_string( exitCode );
		return status;
	}

	std::string output;
	ReadWholeFile( outPath, output );
	const size_t newline = output.find_first_of( "\r\n" );
	std::string firstLine = newline == std::string::npos ? output : output.substr( 0, newline );
	// The version line ends with a build hash nobody needs to read.
	const size_t space = firstLine.find( ' ', firstLine.find( ' ' ) + 1 );
	if( space != std::string::npos )
		firstLine = firstLine.substr( 0, space );

	status.usable = true;
	status.name   = firstLine.empty() ? "LibreOffice" : firstLine;
	return status;
}
}// namespace

const RendererStatus& ProbeRenderer()
{
	std::lock_guard< std::mutex > lock( g_rendererMutex );
	if( !g_rendererProbed )
	{
		g_renderer       = RunProbe();
		g_rendererProbed = true;
	}
	return g_renderer;
}

const RendererStatus& ReprobeRenderer()
{
	std::lock_guard< std::mutex > lock( g_rendererMutex );
	g_renderer       = RunProbe();
	g_rendererProbed = true;
	return g_renderer;
}

RenderBackend SelectRenderBackend( RenderBackend requested, std::string& why )
{
	const RendererStatus& probe = ProbeRenderer();
	const bool havePowerPoint   = probe.backend == RenderBackend::PowerPoint && probe.usable;
	const bool haveLibreOffice  = probe.backend == RenderBackend::LibreOffice && probe.usable;

	if( requested == RenderBackend::PowerPoint && havePowerPoint )
		return RenderBackend::PowerPoint;
	if( requested == RenderBackend::LibreOffice && haveLibreOffice )
		return RenderBackend::LibreOffice;

	// Auto, or the asked-for one is missing: take whatever this machine actually has.
	// PowerPoint first, because it drew the deck in the first place.
	if( havePowerPoint )
		return RenderBackend::PowerPoint;
	if( haveLibreOffice )
		return RenderBackend::LibreOffice;

	why = probe.problem.empty()
			  ? "no renderer found — install LibreOffice, or run this on a machine with PowerPoint"
			  : probe.problem;
	return RenderBackend::Auto;
}

bool ConvertDeck( const ConvertRequest& request,
				  const ConvertCallbacks& callbacks,
				  DeckManifest& outManifest,
				  std::string& error )
{
	if( !PathExists( request.sourcePath ) )
	{
		error = "file not found";
		return false;
	}

	Report( callbacks, 0, 1, "reading" );

	ZipPackage source;
	if( !ReadZip( request.sourcePath, source, error ) )
		return false;

	DeckPlan plan;
	if( !AnalyzePptx( source, plan, error ) )
		return false;

	const int total = static_cast< int >( plan.steps.size() );
	Report( callbacks, 0, total, "planning" );

	// SPEC §3.5 — height follows the deck's own aspect ratio, and stays even so that any
	// downstream encoder stays happy.
	int width  = std::max( 320, request.exportWidth );
	int height = static_cast< int >( width / plan.AspectRatio() + 0.5 );
	height += height & 1;

	std::string why;
	const RenderBackend backend = SelectRenderBackend( request.backend, why );
	if( backend == RenderBackend::Auto )
	{
		error = why;
		return false;
	}

	const std::string workDir = JoinPath( TempRoot(), FileStem( request.cacheDir ) + "_work" );
	RemoveDirectoryTree( workDir );
	if( !EnsureDirectory( workDir ) || !EnsureDirectory( request.cacheDir ) )
	{
		error = "cannot create the cache folder";
		return false;
	}

	bool ok = false;
	if( backend == RenderBackend::LibreOffice )
	{
		// One single-slide package per step: LibreOffice's png export only ever writes the
		// first slide of a file, so each step has to be its own file.
		std::vector< std::string > packages;
		packages.reserve( plan.steps.size() );
		for( int i = 0; i < total; ++i )
		{
			if( Cancelled( callbacks ) )
			{
				error = "cancelled";
				RemoveDirectoryTree( workDir );
				return false;
			}

			ZipPackage stepPackage;
			if( !BuildStepPackage( source, plan, { i }, stepPackage, error ) )
			{
				RemoveDirectoryTree( workDir );
				return false;
			}
			const std::string path = JoinPath( workDir, StepPackageName( i + 1 ) );
			if( !WriteZip( path, stepPackage, error ) )
			{
				RemoveDirectoryTree( workDir );
				return false;
			}
			packages.push_back( path );
			if( ( i % 8 ) == 7 || i + 1 == total )
				Report( callbacks, i + 1, total, "preparing" );
		}

		const std::string pngDir = JoinPath( workDir, "png" );
		ok = RenderWithLibreOffice( FindLibreOffice(), packages, width, height,
									workDir, pngDir, callbacks, request.timeoutSeconds, error );
		if( ok )
		{
			for( int i = 0; i < total; ++i )
			{
				const std::string produced = JoinPath( pngDir, StepImageName( i + 1 ) );
				std::string bytes;
				if( !ReadWholeFile( produced, bytes ) )
				{
					error = "LibreOffice did not produce step " + std::to_string( i + 1 );
					ok    = false;
					break;
				}
				if( !WriteWholeFile( JoinPath( request.cacheDir, StepImageName( i + 1 ) ), bytes ) )
				{
					error = "cannot write the cache";
					ok    = false;
					break;
				}
			}
		}
	}
	else
	{
		// PowerPoint opens one file and exports its slides, so all the steps go into a
		// single package.
		std::vector< int > all( total );
		for( int i = 0; i < total; ++i )
			all[ i ] = i;

		ZipPackage flattened;
		if( !BuildStepPackage( source, plan, all, flattened, error ) )
		{
			RemoveDirectoryTree( workDir );
			return false;
		}
		const std::string path = JoinPath( workDir, "deck_steps.pptx" );
		if( !WriteZip( path, flattened, error ) )
		{
			RemoveDirectoryTree( workDir );
			return false;
		}
		ok = RenderWithPowerPoint( path, total, width, height, request.cacheDir,
								   callbacks, request.timeoutSeconds, error );
	}

	RemoveDirectoryTree( workDir );
	if( !ok )
		return false;

	outManifest             = DeckManifest{};
	outManifest.stepCount   = total;
	outManifest.width       = width;
	outManifest.height      = height;
	outManifest.exportWidth = request.exportWidth;
	outManifest.sourcePath  = request.sourcePath;
	outManifest.sourceMtime = FileMtime( request.sourcePath );
	outManifest.renderer    = backend == RenderBackend::PowerPoint ? "powerpoint" : "libreoffice";
	for( const StepPlan& step : plan.steps )
		outManifest.slideOfStep.push_back( step.sourceSlide );
	if( !plan.warnings.empty() )
		outManifest.warning = plan.warnings.front();

	if( !WriteWholeFile( JoinPath( request.cacheDir, "manifest.json" ), SerializeManifest( outManifest ) ) )
	{
		error = "cannot write the manifest";
		return false;
	}

	Report( callbacks, total, total, "done" );
	return true;
}
}// namespace lxsd
