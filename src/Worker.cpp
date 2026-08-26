#include "Worker.h"

#include <algorithm>

#include "Converter.h"
#include "Platform.h"
#include "Sha1.h"

#if defined( _WIN32 )
#	define WIN32_LEAN_AND_MEAN
#	include <objbase.h>
#endif

namespace lxsd
{
namespace
{
std::string CacheDirFor( const std::string& absPath, int exportWidth )
{
	const std::string key = absPath + "|" + std::to_string( FileMtime( absPath ) ) + "|" + std::to_string( exportWidth );
	return JoinPath( CacheRoot(), Sha1Short( key, 16 ) );
}

std::string HumanSize( uint64_t bytes )
{
	char buf[ 64 ];
	if( bytes >= 1024ull * 1024 * 1024 )
		snprintf( buf, sizeof( buf ), "%.1f GB", bytes / ( 1024.0 * 1024.0 * 1024.0 ) );
	else
		snprintf( buf, sizeof( buf ), "%.0f MB", bytes / ( 1024.0 * 1024.0 ) );
	return buf;
}
}// namespace

Worker::~Worker()
{
	Stop();
}

void Worker::Start()
{
	if( running.exchange( true ) )
		return;
	thread = std::thread( [ this ] { Run(); } );
}

void Worker::Stop()
{
	if( !running.exchange( false ) )
		return;
	{
		std::lock_guard< std::mutex > lock( cmdMutex );
		commands.push_back( Command{ Command::Type::Shutdown, {}, 0, false, 0, 0 } );
	}
	cmdCv.notify_all();
	resultCv.notify_all();// unblocks PushResult if it is waiting for room
	if( thread.joinable() )
		thread.join();
}

void Worker::Post( Command cmd )
{
	{
		std::lock_guard< std::mutex > lock( cmdMutex );
		if( cmd.type == Command::Type::LoadDeck )
		{
			// A new deck makes every queued step request pointless.
			commands.erase( std::remove_if( commands.begin(), commands.end(), []( const Command& c ) {
								return c.type == Command::Type::RequestStep;
							} ),
							commands.end() );
		}
		commands.push_back( std::move( cmd ) );
	}
	cmdCv.notify_one();
}

bool Worker::TryPopResult( DecodedImage& out )
{
	std::unique_lock< std::mutex > lock( resultMutex, std::try_to_lock );
	if( !lock.owns_lock() || results.empty() )
		return false;// never block the render thread, not even on a mutex
	out = std::move( results.front() );
	results.pop_front();
	lock.unlock();
	resultCv.notify_one();
	return true;
}

void Worker::PushResult( DecodedImage img )
{
	std::unique_lock< std::mutex > lock( resultMutex );
	resultCv.wait( lock, [ this ] { return results.size() < kResultCapacity || !running.load(); } );
	if( !running.load() )
		return;
	results.push_back( std::move( img ) );
}

bool Worker::PopCommand( Command& out )
{
	std::unique_lock< std::mutex > lock( cmdMutex );
	cmdCv.wait( lock, [ this ] { return !commands.empty(); } );
	out = std::move( commands.front() );
	commands.pop_front();
	return true;
}

void Worker::Run()
{
#if defined( _WIN32 )
	// PowerPoint automation is apartment-threaded (SPEC §2.1).
	CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
#endif

	PruneOldCaches( CacheRoot(), 30 );// SPEC §10 — do not let old decks fill the disk

	// Say up front what this machine can render with. Finding out that no renderer is
	// installed should not wait until someone picks a deck.
	{
		const RendererStatus& renderer = ProbeRenderer();
		if( shared.GetState() == DeckState::Idle )
			shared.SetStatus( renderer.usable ? "Idle — renderer: " + renderer.name
											  : "Idle — " + renderer.problem );
	}

	for( ;; )
	{
		Command cmd;
		if( !PopCommand( cmd ) )
			break;
		if( cmd.type == Command::Type::Shutdown || !running.load() )
			break;

		try
		{
			switch( cmd.type )
			{
			case Command::Type::LoadDeck: HandleLoadDeck( cmd ); break;
			case Command::Type::RequestStep: HandleRequestStep( cmd ); break;
			case Command::Type::ExportDeck: HandleExportDeck( cmd ); break;
			default: break;
			}
		}
		catch( const std::exception& e )
		{
			shared.state.store( static_cast< int >( DeckState::Error ) );
			shared.SetStatus( std::string( "Error: " ) + e.what() );
		}
		catch( ... )
		{
			shared.state.store( static_cast< int >( DeckState::Error ) );
			shared.SetStatus( "Error: unknown failure" );
		}
	}

#if defined( _WIN32 )
	CoUninitialize();
#endif
}

void Worker::HandleLoadDeck( const Command& cmd )
{
	if( cmd.generation != shared.generation.load() )
		return;// a newer deck was picked while this sat in the queue

	{
		std::lock_guard< std::mutex > lock( resultMutex );
		results.clear();
	}

	shared.state.store( static_cast< int >( DeckState::Converting ) );
	shared.progressDone.store( 0 );
	shared.progressTotal.store( 0 );
	shared.stepCount.store( 0 );
	shared.SetStatus( "Loading…" );

	if( cmd.path.empty() )
	{
		assets = DeckAssets{};
		shared.state.store( static_cast< int >( DeckState::Idle ) );
		const RendererStatus& renderer = ProbeRenderer();
		shared.SetStatus( renderer.usable ? "Idle — renderer: " + renderer.name
										  : "Idle — " + renderer.problem );
		return;
	}

	// Reload is also how someone tells us they have just installed LibreOffice.
	if( cmd.forceReload )
		ReprobeRenderer();

	DeckAssets loaded;
	std::string error;
	if( !ResolveDeck( cmd.path, cmd.exportWidth, cmd.forceReload, loaded, error ) )
	{
		// SPEC §7: a failed load keeps whatever deck was already playing.
		shared.state.store( static_cast< int >( DeckState::Error ) );
		shared.SetStatus( "Error: " + error );
		return;
	}

	assets = std::move( loaded );
	shared.deckWidth.store( assets.manifest.width );
	shared.deckHeight.store( assets.manifest.height );
	shared.stepCount.store( static_cast< int >( assets.stepFiles.size() ) );
	shared.state.store( static_cast< int >( DeckState::Ready ) );

	std::string status = "Ready — " + std::to_string( assets.stepFiles.size() ) + " steps";
	if( !assets.manifest.warning.empty() )
		status += " (" + assets.manifest.warning + ")";
	shared.SetStatus( status );
}

void Worker::HandleRequestStep( const Command& cmd )
{
	if( cmd.generation != shared.generation.load() )
		return;
	if( cmd.stepIndex < 0 || cmd.stepIndex >= static_cast< int >( assets.stepFiles.size() ) )
		return;

	DecodedImage out;
	out.generation = cmd.generation;
	out.stepIndex  = cmd.stepIndex;

	std::string error;
	if( !DecodeImageFile( assets.stepFiles[ cmd.stepIndex ], out.image, error ) )
	{
		shared.SetStatus( "Error: step " + std::to_string( cmd.stepIndex + 1 ) + " — " + error );
		return;
	}
	PushResult( std::move( out ) );
}

void Worker::HandleExportDeck( const Command& cmd )
{
	if( cmd.generation != shared.generation.load() )
		return;
	if( assets.stepFiles.empty() )
	{
		shared.SetStatus( "Export: nothing loaded yet" );
		return;
	}
	if( assets.manifest.renderer == "folder" )
	{
		shared.SetStatus( "Export: this deck is already a folder — just copy it" );
		return;
	}

	const std::string source = assets.manifest.sourcePath;
	const std::string target = JoinPath( ParentDirectory( source ), FileStem( source ) + "_LXSD" );
	if( !EnsureDirectory( target ) )
	{
		shared.SetStatus( "Export: cannot create " + FileName( target ) );
		return;
	}

	const int total = static_cast< int >( assets.stepFiles.size() );
	for( int i = 0; i < total; ++i )
	{
		std::string bytes;
		if( !ReadWholeFile( assets.stepFiles[ i ], bytes ) ||
			!WriteWholeFile( JoinPath( target, FileName( assets.stepFiles[ i ] ) ), bytes ) )
		{
			shared.SetStatus( "Export failed at step " + std::to_string( i + 1 ) );
			return;
		}
		if( ( i % 4 ) == 3 || i + 1 == total )
			shared.SetStatus( "Exporting " + std::to_string( i + 1 ) + "/" + std::to_string( total ) );
	}

	std::string manifestJson;
	if( ReadWholeFile( JoinPath( assets.cacheDir, "manifest.json" ), manifestJson ) )
		WriteWholeFile( JoinPath( target, "manifest.json" ), manifestJson );

	shared.SetStatus( "Exported " + std::to_string( total ) + " steps to " + FileName( target ) );
}

bool Worker::ResolveDeck( const std::string& path, int exportWidth, bool forceReload,
						  DeckAssets& out, std::string& error )
{
	const std::string abs = AbsolutePath( path );

	if( IsDirectory( abs ) )
		return LoadFromFolder( abs, out, error );

	if( !PathExists( abs ) )
	{
		error = "file not found";
		return false;
	}

	const std::string ext = ExtensionLower( abs );

	// A picked image means "play the folder it sits in" — this is the pre-rendered backup
	// deck, and it is also the only mode that needs neither PowerPoint nor LibreOffice.
	if( ext == "png" || ext == "jpg" || ext == "jpeg" )
		return LoadFromFolder( ParentDirectory( abs ), out, error );

	// The converter reads the OOXML inside the file, which the pre-2007 binary .ppt format
	// does not have. Say that plainly instead of failing later with a confusing zip error.
	if( ext == "ppt" )
	{
		error = "old .ppt format — open it in PowerPoint and Save As .pptx";
		return false;
	}
	if( ext != "pptx" && ext != "pptm" )
	{
		error = "unsupported file";
		return false;
	}

	const std::string cacheDir = CacheDirFor( abs, exportWidth );
	if( forceReload )
		RemoveDirectoryTree( cacheDir );
	else if( LoadFromCache( cacheDir, out ) )
		return true;// SPEC §3.6 — opening the same project twice must not reconvert

	return ConvertPresentation( abs, exportWidth, cacheDir, out, error );
}

bool Worker::LoadFromFolder( const std::string& folder, DeckAssets& out, std::string& error )
{
	// A cache folder made by the converter is just a folder with a manifest.
	if( LoadFromCache( folder, out ) )
		return true;

	std::vector< std::string > files = ListFilesByExtension( folder, "png" );
	if( files.empty() )
		files = ListFilesByExtension( folder, "jpg" );
	if( files.empty() )
	{
		error = "no images in " + FileName( folder );
		return false;
	}

	int w = 0, h = 0;
	if( !ReadImageSize( files.front(), w, h ) )
	{
		error = "cannot read " + FileName( files.front() );
		return false;
	}

	out.cacheDir              = folder;
	out.stepFiles             = std::move( files );
	out.manifest              = DeckManifest{};
	out.manifest.stepCount    = static_cast< int >( out.stepFiles.size() );
	out.manifest.width        = w;
	out.manifest.height       = h;
	out.manifest.sourcePath   = folder;
	out.manifest.renderer     = "folder";
	return true;
}

bool Worker::LoadFromCache( const std::string& cacheDir, DeckAssets& out )
{
	const std::string manifestPath = JoinPath( cacheDir, "manifest.json" );
	std::string json;
	if( !PathExists( manifestPath ) || !ReadWholeFile( manifestPath, json ) )
		return false;

	DeckManifest m;
	if( !ParseManifest( json, m ) )
		return false;

	std::vector< std::string > files = ListFilesByExtension( cacheDir, "png" );
	if( static_cast< int >( files.size() ) != m.stepCount || files.empty() )
		return false;// half-written cache, treat as a miss and convert again

	out.cacheDir  = cacheDir;
	out.stepFiles = std::move( files );
	out.manifest  = m;
	return true;
}

bool Worker::ConvertPresentation( const std::string& pptxPath, int exportWidth,
								  const std::string& cacheDir, DeckAssets& out, std::string& error )
{
	ConvertRequest request;
	request.sourcePath  = pptxPath;
	request.cacheDir    = cacheDir;
	request.exportWidth = exportWidth;

	const uint64_t myGeneration = shared.generation.load();

	ConvertCallbacks callbacks;
	callbacks.progress = [ this ]( int done, int total, const std::string& note ) {
		shared.progressDone.store( done );
		shared.progressTotal.store( total );
		std::string s = "Converting " + std::to_string( done ) + "/" + std::to_string( total );
		if( !note.empty() )
			s += " — " + note;
		shared.SetStatus( s );
	};
	callbacks.cancelled = [ this, myGeneration ] {
		return !running.load() || shared.generation.load() != myGeneration;
	};

	DeckManifest manifest;
	if( !ConvertDeck( request, callbacks, manifest, error ) )
	{
		RemoveDirectoryTree( cacheDir );// never leave a half deck behind to be cache-hit later
		return false;
	}

	if( !LoadFromCache( cacheDir, out ) )
	{
		error = "converted deck could not be read back";
		return false;
	}

	const uint64_t bytes = DirectorySize( cacheDir );
	if( bytes > 0 && out.manifest.warning.empty() )
		out.manifest.warning = "cache " + HumanSize( bytes );
	return true;
}
}// namespace lxsd
