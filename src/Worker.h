// LX SlideDeck — SPEC §2. The worker thread and the two queues that connect it to the
// render thread.
//
// Two hard rules, both from SPEC §2.1:
//   * no file I/O, no image decoding, no converting anywhere in ProcessOpenGL
//   * no OpenGL call anywhere on this thread — the host does not share its GL context,
//     so the worker only ever produces CPU pixel buffers
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ImageDecode.h"
#include "Manifest.h"

namespace lxsd
{
enum class DeckState : int
{
	Idle       = 0,
	Converting = 1,
	Ready      = 2,
	Error      = 3,
};

struct Command
{
	enum class Type
	{
		LoadDeck,
		RequestStep,
		ExportDeck,
		Shutdown,
	};

	Type type            = Type::LoadDeck;
	std::string path;    //!< LoadDeck: the .pptx / .png / folder the user picked
	int exportWidth      = 3840;
	bool forceReload     = false;//!< Reload button: throw the cache away first
	int stepIndex        = 0;    //!< RequestStep: 0-based step
	uint64_t generation  = 0;    //!< which deck this command belongs to
};

/// A decoded step, on its way to becoming a texture.
struct DecodedImage
{
	uint64_t generation = 0;
	int stepIndex       = 0;
	RgbaImage image;
};

/// Everything the render thread is allowed to read while the worker is running.
struct SharedState
{
	std::atomic< int > state{ static_cast< int >( DeckState::Idle ) };
	std::atomic< int > progressDone{ 0 };
	std::atomic< int > progressTotal{ 0 };
	std::atomic< int > stepCount{ 0 };
	std::atomic< int > deckWidth{ 0 };
	std::atomic< int > deckHeight{ 0 };
	std::atomic< uint64_t > generation{ 0 };//!< bumped on every deck change
	std::atomic< uint64_t > statusSerial{ 0 };//!< bumped whenever statusMessage changes

	mutable std::mutex msgMutex;
	std::string statusMessage = "Idle";

	void SetStatus( const std::string& s )
	{
		{
			std::lock_guard< std::mutex > lock( msgMutex );
			if( statusMessage == s )
				return;
			statusMessage = s;
		}
		statusSerial.fetch_add( 1 );
	}
	std::string GetStatus() const
	{
		std::lock_guard< std::mutex > lock( msgMutex );
		return statusMessage;
	}
	DeckState GetState() const
	{
		return static_cast< DeckState >( state.load() );
	}
};

/// What a loaded deck actually is, from the worker's point of view: an ordered list of
/// PNG files plus the manifest that describes them.
struct DeckAssets
{
	std::vector< std::string > stepFiles;
	DeckManifest manifest;
	std::string cacheDir;
};

class Worker
{
public:
	Worker() = default;
	~Worker();

	Worker( const Worker& )            = delete;
	Worker& operator=( const Worker& ) = delete;

	void Start();
	void Stop();

	/// Queues a command. A LoadDeck also bumps the generation and drops everything that
	/// was queued for the previous deck.
	void Post( Command cmd );

	/// Render thread: takes one decoded image, or returns false when none is waiting.
	bool TryPopResult( DecodedImage& out );

	uint64_t BumpGeneration()
	{
		return shared.generation.fetch_add( 1 ) + 1;
	}

	SharedState shared;

private:
	void Run();
	void HandleLoadDeck( const Command& cmd );
	void HandleRequestStep( const Command& cmd );
	/// Copies the rendered deck next to the .pptx, so it can be carried to a machine that
	/// has neither PowerPoint nor LibreOffice.
	void HandleExportDeck( const Command& cmd );
	void PushResult( DecodedImage img );
	bool PopCommand( Command& out );

	/// Resolves a user-picked path into a list of step images.
	/// Folders and .png files are taken as-is (SPEC's "pre-rendered deck" fallback);
	/// .pptx / .ppt go through the cache and, when it misses, the converter.
	bool ResolveDeck( const std::string& path, int exportWidth, bool forceReload,
					  DeckAssets& out, std::string& error );

	bool LoadFromFolder( const std::string& folder, DeckAssets& out, std::string& error );
	bool LoadFromCache( const std::string& cacheDir, DeckAssets& out );
	bool ConvertPresentation( const std::string& pptxPath, int exportWidth,
							  const std::string& cacheDir, DeckAssets& out, std::string& error );

	std::thread thread;
	std::atomic< bool > running{ false };

	std::mutex cmdMutex;
	std::condition_variable cmdCv;
	std::deque< Command > commands;

	static constexpr size_t kResultCapacity = 4;//!< SPEC §2.2 — keeps preloading from eating RAM
	std::mutex resultMutex;
	std::condition_variable resultCv;
	std::deque< DecodedImage > results;

	DeckAssets assets;
};
}// namespace lxsd
