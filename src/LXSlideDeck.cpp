#include "LXSlideDeck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "LruPolicy.h"
#include "Platform.h"
#include "Shaders.inc"

using namespace ffglex;

namespace lxsd
{
static CFFGLPluginInfo PluginInfo(
	PluginFactory< LXSlideDeck >,// Create method
	"LXSD",                      // Plugin unique ID
	"LX SlideDeck",              // Plugin name
	2,                           // API major version
	1,                           // API minor version
	1,                           // Plugin major version
	0,                           // Plugin minor version
	FF_SOURCE,                   // Plugin type
	"Plays a PowerPoint deck step by step, crossfading between animation steps",
	"LX / leoxi" );

namespace
{
constexpr const char* kBuildTag = "b8";

/// Lets two instances of the plugin in the same Resolume process move together.
///
/// The intended use is a second layer set to Preview = Next Only, kept off the output and
/// watched in Arena's own Preview Monitor: it follows the deck on the main layer and shows
/// the step that is coming, without the operator having to map one key onto two Next
/// buttons and hope they stay in step.
///
/// Whoever moves last wins; a follower adopting a step never rebroadcasts it, so there is
/// no ping-pong between the two.
struct SyncGroupState
{
	std::mutex mutex;
	int step        = 0;
	uint64_t serial = 0;
};

constexpr int kSyncGroupCount = 3;

SyncGroupState& SyncGroup( int index )
{
	static SyncGroupState groups[ kSyncGroupCount ];
	return groups[ ( index - 1 ) % kSyncGroupCount ];
}

float ApplyCurve( float t, int curve )
{
	t = std::min( 1.0f, std::max( 0.0f, t ) );
	switch( curve )
	{
	case 0: return t;                       // Linear
	case 2: return 1.0f - ( 1.0f - t ) * ( 1.0f - t );// Ease Out
	case 1:
	default: return t * t * ( 3.0f - 2.0f * t );      // Smooth
	}
}
}// namespace

LXSlideDeck::LXSlideDeck()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	// SPEC §1.1 said FFGL has no file parameter type. That is no longer true on the master
	// branch of the SDK: FF_TYPE_FILE exists and Resolume shows its own file picker for it,
	// which is why there is no separate Browse event parameter here.
	SetFileParamInfo( PID_DECK_FILE, "Deck File", { "pptx", "pptm", "ppt", "png" }, "" );

	SetParamInfo( PID_RELOAD, "Reload", FF_TYPE_EVENT, false );

	SetParamInfo( PID_STEP, "Step", FF_TYPE_INTEGER, 1.0f );
	// Resolume reads a parameter's range once, when the plugin is instantiated, and a later
	// SetParamRange does not reach it — there is no FF_EVENT_FLAG for a range change in the
	// SDK. A range of 1..1 here meant the host clamped every typed value back to 1, so the
	// box could only be driven with Next / Prev. Declare a range wide enough for any deck
	// and clamp to the real step count ourselves.
	SetParamRange( PID_STEP, 1.0f, static_cast< float >( kMaxAddressableStep ) );

	SetParamInfo( PID_NEXT, "Next", FF_TYPE_EVENT, false );
	SetParamInfo( PID_PREV, "Prev", FF_TYPE_EVENT, false );

	SetParamInfo( PID_FADE_TIME, "Fade Time", FF_TYPE_STANDARD, 0.4f );
	SetParamRange( PID_FADE_TIME, 0.0f, 5.0f );

	SetOptionParamInfo( PID_FADE_CURVE, "Fade Curve", 3, 1.0f );
	SetParamElementInfo( PID_FADE_CURVE, 0, "Linear", 0.0f );
	SetParamElementInfo( PID_FADE_CURVE, 1, "Smooth", 1.0f );
	SetParamElementInfo( PID_FADE_CURVE, 2, "Ease Out", 2.0f );

	SetParamInfo( PID_AUTOPILOT, "Autopilot", FF_TYPE_BOOLEAN, false );

	SetParamInfo( PID_INTERVAL, "Interval", FF_TYPE_STANDARD, 5.0f );
	SetParamRange( PID_INTERVAL, 0.5f, 60.0f );

	SetParamInfo( PID_LOOP, "Loop", FF_TYPE_BOOLEAN, true );

	SetOptionParamInfo( PID_SCALE_MODE, "Scale Mode", 4, 1.0f );
	SetParamElementInfo( PID_SCALE_MODE, 0, "Native", 0.0f );
	SetParamElementInfo( PID_SCALE_MODE, 1, "Fit", 1.0f );
	SetParamElementInfo( PID_SCALE_MODE, 2, "Fill", 2.0f );
	SetParamElementInfo( PID_SCALE_MODE, 3, "Stretch", 3.0f );

	SetParamInfo( PID_EXPORT_WIDTH, "Export Width", FF_TYPE_INTEGER, 3840.0f );
	SetParamRange( PID_EXPORT_WIDTH, 1280.0f, 4096.0f );

	SetOptionParamInfo( PID_SYNC, "Sync", 4, 0.0f );
	SetParamElementInfo( PID_SYNC, 0, "Off", 0.0f );
	SetParamElementInfo( PID_SYNC, 1, "Group A", 1.0f );
	SetParamElementInfo( PID_SYNC, 2, "Group B", 2.0f );
	SetParamElementInfo( PID_SYNC, 3, "Group C", 3.0f );

	SetOptionParamInfo( PID_PREVIEW, "Preview", 4, 0.0f );
	SetParamElementInfo( PID_PREVIEW, 0, "Off", 0.0f );
	SetParamElementInfo( PID_PREVIEW, 1, "Next Only", 1.0f );
	SetParamElementInfo( PID_PREVIEW, 2, "Split", 2.0f );
	SetParamElementInfo( PID_PREVIEW, 3, "Corner", 3.0f );

	// Copies the rendered images next to the .pptx, so the deck can be carried to a machine
	// that has no PowerPoint and no LibreOffice and still played.
	SetParamInfo( PID_EXPORT_DECK, "Export Deck", FF_TYPE_EVENT, false );

	SetParamInfo( PID_STATUS, "Status", FF_TYPE_TEXT, "Idle" );

	// SetParamInfo clamps FF_TYPE_STANDARD defaults into 0..1, which is wrong for a
	// parameter that carries real units. Put the intended default back.
	if( ParamInfo* info = FindParamInfo( PID_INTERVAL ) )
		info->defaultFloatVal = 5.0f;

	worker.Start();
}

LXSlideDeck::~LXSlideDeck()
{
	worker.Stop();
}

FFResult LXSlideDeck::InitGL( const FFGLViewportStruct* vp )
{
	try
	{
		if( !shader.Compile( kVertexShader, kFragmentShader ) )
		{
			DeInitGL();
			return FF_FAIL;
		}
		// The quad's uvs are flipped so that row 0 of a decoded PNG — its top row — lands at
		// the top of the output instead of being mirrored.
		if( !quad.Initialise( true ) )
		{
			DeInitGL();
			return FF_FAIL;
		}

		ScopedShaderBinding binding( shader.GetGLID() );
		locTexPrev     = shader.FindUniform( "texPrev" );
		locTexCur      = shader.FindUniform( "texCur" );
		locTexNext     = shader.FindUniform( "texNext" );
		locMixT        = shader.FindUniform( "mixT" );
		locUvPrev      = shader.FindUniform( "uvPrev" );
		locUvCur       = shader.FindUniform( "uvCur" );
		locUvNext      = shader.FindUniform( "uvNext" );
		locHasPrev     = shader.FindUniform( "hasPrev" );
		locHasCur      = shader.FindUniform( "hasCur" );
		locHasNext     = shader.FindUniform( "hasNext" );
		locRectMain    = shader.FindUniform( "rectMain" );
		locRectPreview = shader.FindUniform( "rectPreview" );
		locBorderWidth = shader.FindUniform( "borderWidth" );
		locMode        = shader.FindUniform( "mode" );
		locProgress    = shader.FindUniform( "progress" );
		locSteps       = shader.FindUniform( "steps" );
		locClock       = shader.FindUniform( "clock" );
		locTint        = shader.FindUniform( "tint" );

		glUniform1i( locTexPrev, 0 );
		glUniform1i( locTexCur, 1 );
		glUniform1i( locTexNext, 2 );

		return CFFGLPlugin::InitGL( vp );
	}
	catch( ... )
	{
		return FF_FAIL;
	}
}

FFResult LXSlideDeck::DeInitGL()
{
	try
	{
		ReleaseAllTextures();
		shader.FreeGLResources();
		quad.Release();
	}
	catch( ... )
	{
	}
	return FF_SUCCESS;
}

void LXSlideDeck::ReleaseAllTextures()
{
	for( auto& kv : textures )
		if( kv.second.id != 0 )
			glDeleteTextures( 1, &kv.second.id );
	textures.clear();
}

const LXSlideDeck::SlideTexture* LXSlideDeck::FindTexture( int step ) const
{
	auto it = textures.find( step );
	return it == textures.end() ? nullptr : &it->second;
}

void LXSlideDeck::UploadTexture( const DecodedImage& img )
{
	if( !img.image.Valid() )
		return;

	SlideTexture& tex = textures[ img.stepIndex ];
	if( tex.id == 0 )
		glGenTextures( 1, &tex.id );

	GLint prevBinding = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &prevBinding );

	glBindTexture( GL_TEXTURE_2D, tex.id );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, img.image.width, img.image.height, 0,
				  GL_RGBA, GL_UNSIGNED_BYTE, img.image.pixels.data() );

	// Mipmaps only pay off when the slide is much bigger than the viewport, which is the
	// usual case for a 3840px export on a 1080p comp but not for a wall-sized one.
	const bool wantMipmaps = renderWidth > 0 && img.image.width >= 2 * renderWidth;
	if( wantMipmaps )
	{
		glGenerateMipmap( GL_TEXTURE_2D );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	}
	else
	{
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	}
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( prevBinding ) );

	tex.width    = img.image.width;
	tex.height   = img.image.height;
	tex.lastUsed = frameCounter;

	requestedSteps.erase( img.stepIndex );
}

void LXSlideDeck::PumpDecodedImages()
{
	int uploaded = 0;
	while( uploaded < 2 )// SPEC §4.2 — a hard cap keeps frame time flat while preloading
	{
		DecodedImage img;
		if( !worker.TryPopResult( img ) )
			break;
		if( img.generation != deckGeneration )
			continue;// belongs to a deck the user already replaced
		UploadTexture( img );
		++uploaded;
	}
}

void LXSlideDeck::EvictTextures()
{
	if( textures.size() <= kMaxTextures )
		return;

	std::vector< LruEntry > entries;
	entries.reserve( textures.size() );
	for( const auto& kv : textures )
		entries.push_back( LruEntry{ kv.first, kv.second.lastUsed } );

	// Whatever is on screen this frame has to survive the eviction pass.
	std::vector< int > keep{ MainStep(), MainPrevStep() };
	if( PreviewStep() >= 0 )
		keep.push_back( PreviewStep() );
	for( int key : SelectLruEvictions( entries, kMaxTextures, keep ) )
	{
		auto it = textures.find( key );
		if( it == textures.end() )
			continue;
		if( it->second.id != 0 )
			glDeleteTextures( 1, &it->second.id );
		textures.erase( it );
	}
}

int LXSlideDeck::MainStep() const
{
	return preview == PreviewMode::NextOnly ? WrapStep( currentStep + 1 ) : currentStep;
}

int LXSlideDeck::MainPrevStep() const
{
	return preview == PreviewMode::NextOnly ? WrapStep( prevStep + 1 ) : prevStep;
}

int LXSlideDeck::PreviewStep() const
{
	if( preview != PreviewMode::Split && preview != PreviewMode::Corner )
		return -1;
	if( knownStepCount <= 0 )
		return -1;
	return WrapStep( currentStep + 1 );
}

int LXSlideDeck::WrapStep( int step ) const
{
	if( knownStepCount <= 0 )
		return 0;
	if( step >= knownStepCount )
		return loopDeck ? step % knownStepCount : knownStepCount - 1;
	if( step < 0 )
		return loopDeck ? ( step % knownStepCount + knownStepCount ) % knownStepCount : 0;
	return step;
}

int LXSlideDeck::ClampStep( int step ) const
{
	if( knownStepCount <= 0 )
		return 0;
	return std::min( knownStepCount - 1, std::max( 0, step ) );
}

void LXSlideDeck::GoToStep( int step, bool fromHost, bool broadcast )
{
	step = ClampStep( step );
	if( step == currentStep )
		return;
	prevStep    = currentStep;
	currentStep = step;
	fadeT       = 0.0f;
	autopilotClock = 0.0f;
	stepParam   = currentStep + 1;
	if( !fromHost )
		PublishStepToHost();
	if( broadcast )
		PublishToSyncGroup();
}

void LXSlideDeck::PublishToSyncGroup()
{
	if( syncGroup <= 0 )
		return;
	SyncGroupState& group = SyncGroup( syncGroup );
	std::lock_guard< std::mutex > lock( group.mutex );
	group.step = currentStep;
	lastSyncSerial = ++group.serial;
}

void LXSlideDeck::FollowSyncGroup()
{
	if( syncGroup <= 0 )
		return;
	int step        = 0;
	uint64_t serial = 0;
	{
		SyncGroupState& group = SyncGroup( syncGroup );
		std::lock_guard< std::mutex > lock( group.mutex );
		step   = group.step;
		serial = group.serial;
	}
	if( serial == lastSyncSerial )
		return;
	lastSyncSerial = serial;
	GoToStep( step, false, false );
}

void LXSlideDeck::PublishStepToHost()
{
	// Lets Resolume's ui and any recorded automation follow along when the plugin moves the
	// step by itself (autopilot, or a deck that just finished loading).
	RaiseParamEvent( PID_STEP, FF_EVENT_FLAG_VALUE );
}

void LXSlideDeck::PublishStatusToHost()
{
	RaiseParamEvent( PID_STATUS, FF_EVENT_FLAG_VALUE );
}

void LXSlideDeck::UpdateDiagnostics( const SlideTexture* curTex )
{
	// Surfaced in the Status parameter so that what the host actually sent can be read off
	// the panel instead of guessed at.
	char buf[ 160 ];
	// The build tag is here so a screenshot of the panel says which binary is running —
	// without it, a fix that has not been picked up looks exactly like a fix that failed.
	snprintf( buf, sizeof( buf ), "  [%s scale=%d vp=%dx%d img=%dx%d]",
			  kBuildTag,
			  static_cast< int >( scaleMode ),
			  renderWidth,
			  renderHeight,
			  curTex ? curTex->width : 0,
			  curTex ? curTex->height : 0 );

	if( diagnostics == buf )
		return;
	diagnostics = buf;
	PublishStatusToHost();
}

void LXSlideDeck::SyncWorkerState()
{
	const uint64_t generation = worker.shared.generation.load();
	if( generation != deckGeneration )
	{
		deckGeneration = generation;
		ReleaseAllTextures();
		requestedSteps.clear();
		knownStepCount = 0;
		currentStep    = 0;
		prevStep       = 0;
		fadeT          = 1.0f;
	}

	const int count = worker.shared.stepCount.load();
	if( count != knownStepCount )
	{
		const bool firstDeck = knownStepCount == 0 && count > 0;
		knownStepCount       = count;
		// Best effort: hosts that re-read ranges get a tidy 1..N slider, the rest keep the
		// wide range declared in the constructor. Either way clamping happens below.
		SetParamRange( PID_STEP, 1.0f, static_cast< float >( std::max( 1, count ) ) );
		currentStep = ClampStep( currentStep );
		prevStep    = ClampStep( prevStep );
		if( firstDeck )
		{
			currentStep = 0;
			prevStep    = 0;
			fadeT       = 1.0f;
			stepParam   = 1;
			PublishStepToHost();
		}
	}

	const uint64_t serial = worker.shared.statusSerial.load();
	if( serial != knownStatusSerial )
	{
		knownStatusSerial = serial;
		{
			std::lock_guard< std::mutex > lock( textMutex );
			statusText = worker.shared.GetStatus();
		}
		PublishStatusToHost();
	}
}

void LXSlideDeck::AdvanceAutopilot( float dt )
{
	if( !autopilot || knownStepCount <= 1 )
		return;
	autopilotClock += dt;
	if( autopilotClock < interval )
		return;
	autopilotClock = 0.0f;

	int next = currentStep + 1;
	if( next >= knownStepCount )
	{
		if( !loopDeck )
			return;
		next = 0;
	}
	GoToStep( next, false );
}

void LXSlideDeck::UpdateFade( float dt )
{
	if( fadeT >= 1.0f )
		return;
	if( fadeTime <= 0.0f )
	{
		fadeT = 1.0f;
		return;
	}
	fadeT = std::min( 1.0f, fadeT + dt / fadeTime );
}

void LXSlideDeck::RequestPreload()
{
	if( knownStepCount <= 0 )
		return;
	// SPEC §4.1 — the step we are on first, then the neighbours a clicker is most likely
	// to reach for.
	const int wanted[] = { MainStep(), MainPrevStep(), PreviewStep(),
						   currentStep + 1, currentStep - 1, currentStep + 2 };
	for( int step : wanted )
	{
		if( step < 0 || step >= knownStepCount )
			continue;
		if( textures.find( step ) != textures.end() )
			continue;
		if( requestedSteps.find( step ) != requestedSteps.end() )
			continue;
		requestedSteps.insert( step );

		Command cmd;
		cmd.type       = Command::Type::RequestStep;
		cmd.stepIndex  = step;
		cmd.generation = deckGeneration;
		worker.Post( cmd );
	}
}

void LXSlideDeck::RequestDeckLoad( bool forceReload )
{
	Command cmd;
	cmd.type        = Command::Type::LoadDeck;
	cmd.path        = deckFile;
	cmd.exportWidth = exportWidth;
	cmd.forceReload = forceReload;
	cmd.generation  = worker.BumpGeneration();
	worker.Post( std::move( cmd ) );
}

FFResult LXSlideDeck::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	try
	{
		++frameCounter;

		const auto now = std::chrono::steady_clock::now();
		float dt       = 0.0f;
		if( haveLastFrameTime )
			dt = std::chrono::duration< float >( now - lastFrameTime ).count();
		lastFrameTime     = now;
		haveLastFrameTime = true;
		// A paused host or a breakpoint can hand us an enormous delta; clamping keeps a
		// crossfade from snapping and autopilot from skipping a pile of steps at once.
		dt = std::min( dt, 0.25f );
		placeholderClock += dt;

		// FFGL hands over a viewport once, in InitGL, and never mentions it again — there is
		// no per-frame size anywhere in ProcessOpenGLStruct. When the host renders us at a
		// different size than it announced (a composition resized after the clip was added,
		// for one), that stale number silently breaks every scale mode. Ask OpenGL for the
		// size actually being rendered into and believe that instead.
		{
			GLint viewport[ 4 ] = { 0, 0, 0, 0 };
			glGetIntegerv( GL_VIEWPORT, viewport );
			if( viewport[ 2 ] > 0 && viewport[ 3 ] > 0 )
			{
				renderWidth  = viewport[ 2 ];
				renderHeight = viewport[ 3 ];
			}
			else
			{
				renderWidth  = static_cast< int >( currentViewport.width );
				renderHeight = static_cast< int >( currentViewport.height );
			}
		}

		SyncWorkerState();
		PumpDecodedImages();

		FollowSyncGroup();

		// The host owns the Step parameter, so a value it set wins over our own position.
		const int hostStep = ClampStep( stepParam - 1 );
		if( hostStep != currentStep )
			GoToStep( hostStep, true );

		AdvanceAutopilot( dt );
		UpdateFade( dt );
		RequestPreload();
		EvictTextures();

		const int mainStep     = MainStep();
		const int mainPrev     = MainPrevStep();
		const int previewStep  = PreviewStep();
		const SlideTexture* curTex     = FindTexture( mainStep );
		const SlideTexture* prevTex    = FindTexture( mainPrev );
		const SlideTexture* previewTex = previewStep >= 0 ? FindTexture( previewStep ) : nullptr;
		if( curTex != nullptr )
			textures[ mainStep ].lastUsed = frameCounter;
		if( prevTex != nullptr )
			textures[ mainPrev ].lastUsed = frameCounter;
		if( previewTex != nullptr )
			textures[ previewStep ].lastUsed = frameCounter;

		const DeckState state = worker.shared.GetState();
		const bool placeholder = curTex == nullptr && prevTex == nullptr;

		UpdateDiagnostics( curTex );

		// ---- save the bits of gl state we are about to touch (SPEC §5.4) ----
		GLint prevActiveTexture = GL_TEXTURE0;
		glGetIntegerv( GL_ACTIVE_TEXTURE, &prevActiveTexture );
		GLint prevTex0 = 0, prevTex1 = 0, prevTex2 = 0;
		glActiveTexture( GL_TEXTURE0 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &prevTex0 );
		glActiveTexture( GL_TEXTURE1 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &prevTex1 );
		glActiveTexture( GL_TEXTURE2 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &prevTex2 );
		const GLboolean blendWasEnabled = glIsEnabled( GL_BLEND );
		GLint blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
		glGetIntegerv( GL_BLEND_SRC_RGB, &blendSrcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &blendDstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &blendSrcA );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &blendDstA );

		// We write the whole viewport ourselves, letterbox included, so blending would only
		// mix us with whatever the host left in the buffer.
		glDisable( GL_BLEND );

		{
			ScopedShaderBinding shaderBinding( shader.GetGLID() );

			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, prevTex ? prevTex->id : 0 );
			glActiveTexture( GL_TEXTURE1 );
			glBindTexture( GL_TEXTURE_2D, curTex ? curTex->id : 0 );
			glActiveTexture( GL_TEXTURE2 );
			glBindTexture( GL_TEXTURE_2D, previewTex ? previewTex->id : 0 );

			glUniform1i( locTexPrev, 0 );
			glUniform1i( locTexCur, 1 );
			glUniform1i( locTexNext, 2 );

			if( placeholder )
			{
				const int done  = worker.shared.progressDone.load();
				const int total = worker.shared.progressTotal.load();
				float progress  = total > 0 ? static_cast< float >( done ) / static_cast< float >( total ) : 0.0f;
				if( state == DeckState::Error )
					progress = 1.0f;// a full red bar reads as "stopped", not "nearly there"

				glUniform1i( locMode, 1 );
				glUniform1f( locProgress, std::min( 1.0f, std::max( 0.0f, progress ) ) );
				glUniform1i( locSteps, state == DeckState::Converting ? total : 0 );
				glUniform1f( locClock, placeholderClock );
				if( state == DeckState::Error )
					glUniform3f( locTint, 0.85f, 0.25f, 0.20f );
				else
					glUniform3f( locTint, 0.55f, 0.60f, 0.70f );
				glUniform1f( locHasPrev, 0.0f );
				glUniform1f( locHasCur, 0.0f );
				glUniform1f( locHasNext, 0.0f );
				glUniform1f( locMixT, 0.0f );
				glUniform4f( locUvPrev, 1.0f, 1.0f, 0.0f, 0.0f );
				glUniform4f( locUvCur, 1.0f, 1.0f, 0.0f, 0.0f );
				glUniform4f( locUvNext, 1.0f, 1.0f, 0.0f, 0.0f );
				glUniform4f( locRectMain, 0.0f, 0.0f, 1.0f, 1.0f );
				glUniform4f( locRectPreview, 0.0f, 0.0f, 0.0f, 0.0f );
				glUniform2f( locBorderWidth, 0.0f, 0.0f );
			}
			else
			{
				const float vpW = static_cast< float >( renderWidth );
				const float vpH = static_cast< float >( renderHeight );

				// Where the deck goes, and where the upcoming step goes.
				float mainRect[ 4 ]    = { 0.0f, 0.0f, 1.0f, 1.0f };
				float previewRect[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };

				if( preview == PreviewMode::Split )
				{
					mainRect[ 2 ]    = 0.5f;
					previewRect[ 0 ] = 0.5f;
					previewRect[ 1 ] = 0.0f;
					previewRect[ 2 ] = 1.0f;
					previewRect[ 3 ] = 1.0f;
				}
				else if( preview == PreviewMode::Corner && previewTex != nullptr )
				{
					// Sized off the slide's own aspect so the inset is a small copy of the
					// slide rather than a squashed box.
					const float margin = 0.02f;
					const float width  = 0.26f;
					const float aspect = previewTex->height > 0
											 ? static_cast< float >( previewTex->width ) / previewTex->height
											 : 16.0f / 9.0f;
					const float height = ( width * vpW / aspect ) / std::max( 1.0f, vpH );
					previewRect[ 0 ]   = 1.0f - margin - width;
					previewRect[ 1 ]   = margin;
					previewRect[ 2 ]   = 1.0f - margin;
					previewRect[ 3 ]   = margin + height;
				}

				const int mainW    = static_cast< int >( ( mainRect[ 2 ] - mainRect[ 0 ] ) * vpW );
				const int mainH    = static_cast< int >( ( mainRect[ 3 ] - mainRect[ 1 ] ) * vpH );
				const int previewW = static_cast< int >( ( previewRect[ 2 ] - previewRect[ 0 ] ) * vpW );
				const int previewH = static_cast< int >( ( previewRect[ 3 ] - previewRect[ 1 ] ) * vpH );

				// Two steps can differ in size when Export Width changed mid-deck, so each
				// texture gets its own transform.
				UvTransform tp = prevTex ? ComputeUvTransform( scaleMode, prevTex->width, prevTex->height, mainW, mainH )
										 : UvTransform{};
				UvTransform tc = curTex ? ComputeUvTransform( scaleMode, curTex->width, curTex->height, mainW, mainH )
										: UvTransform{};
				// The preview is always Fit: its whole job is to show what is coming, so
				// cropping it would defeat the point whatever the main Scale Mode is.
				UvTransform tn = previewTex ? ComputeUvTransform( ScaleMode::Fit, previewTex->width, previewTex->height,
																 previewW, previewH )
											: UvTransform{};

				glUniform1i( locMode, 0 );
				glUniform1f( locProgress, 0.0f );
				glUniform3f( locTint, 0.0f, 0.0f, 0.0f );
				glUniform1f( locHasPrev, prevTex ? 1.0f : 0.0f );
				glUniform1f( locHasCur, curTex ? 1.0f : 0.0f );
				glUniform1f( locHasNext, previewTex ? 1.0f : 0.0f );
				// With only one of the two available, hold it fully rather than fading to
				// black — a missing neighbour must never flash on the wall.
				float t = ApplyCurve( fadeT, fadeCurve );
				if( curTex == nullptr )
					t = 0.0f;
				else if( prevTex == nullptr )
					t = 1.0f;
				glUniform1f( locMixT, t );
				glUniform4f( locUvPrev, tp.scaleX, tp.scaleY, tp.offX, tp.offY );
				glUniform4f( locUvCur, tc.scaleX, tc.scaleY, tc.offX, tc.offY );
				glUniform4f( locUvNext, tn.scaleX, tn.scaleY, tn.offX, tn.offY );
				glUniform4f( locRectMain, mainRect[ 0 ], mainRect[ 1 ], mainRect[ 2 ], mainRect[ 3 ] );
				glUniform4f( locRectPreview, previewRect[ 0 ], previewRect[ 1 ], previewRect[ 2 ], previewRect[ 3 ] );
				glUniform2f( locBorderWidth, 3.0f / std::max( 1.0f, vpW ), 3.0f / std::max( 1.0f, vpH ) );
			}

			quad.Draw();
		}

		// ---- put it all back ----
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( prevTex2 ) );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( prevTex1 ) );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( prevTex0 ) );
		glActiveTexture( static_cast< GLenum >( prevActiveTexture ) );
		if( blendWasEnabled )
			glEnable( GL_BLEND );
		else
			glDisable( GL_BLEND );
		glBlendFuncSeparate( blendSrcRGB, blendDstRGB, blendSrcA, blendDstA );

		return FF_SUCCESS;
	}
	catch( ... )
	{
		// SPEC §7: the plugin never reports failure to the host, whatever happens.
		return FF_SUCCESS;
	}
}

FFResult LXSlideDeck::SetFloatParameter( unsigned int index, float value )
{
	try
	{
		switch( index )
		{
		case PID_RELOAD:
			if( value > 0.5f && reloadDown <= 0.5f && !deckFile.empty() )
				RequestDeckLoad( true );
			reloadDown = value;
			break;

		case PID_EXPORT_DECK:
			if( value > 0.5f && exportDown <= 0.5f )
			{
				Command cmd;
				cmd.type       = Command::Type::ExportDeck;
				cmd.generation = deckGeneration;
				worker.Post( std::move( cmd ) );
			}
			exportDown = value;
			break;

		case PID_STEP:
			stepParam = static_cast< int >( std::lround( value ) );
			break;

		case PID_NEXT:
			if( value > 0.5f && nextDown <= 0.5f )
			{
				int next = currentStep + 1;
				if( next >= knownStepCount )
					next = loopDeck ? 0 : knownStepCount - 1;
				GoToStep( next, false );
			}
			nextDown = value;
			break;

		case PID_PREV:
			if( value > 0.5f && prevDown <= 0.5f )
			{
				int prev = currentStep - 1;
				if( prev < 0 )
					prev = loopDeck ? std::max( 0, knownStepCount - 1 ) : 0;
				GoToStep( prev, false );
			}
			prevDown = value;
			break;

		case PID_FADE_TIME: fadeTime = std::max( 0.0f, value ); break;
		case PID_FADE_CURVE: fadeCurve = static_cast< int >( std::lround( value ) ); break;
		case PID_AUTOPILOT: autopilot = value > 0.5f; break;
		case PID_INTERVAL: interval = std::max( 0.1f, value ); break;
		case PID_LOOP: loopDeck = value > 0.5f; break;
		case PID_SCALE_MODE: scaleMode = static_cast< ScaleMode >( static_cast< int >( std::lround( value ) ) ); break;
		case PID_PREVIEW: preview = static_cast< PreviewMode >( static_cast< int >( std::lround( value ) ) ); break;

		case PID_SYNC:
		{
			const int group = static_cast< int >( std::lround( value ) );
			if( group != syncGroup )
			{
				syncGroup = group;
				// Joining a group means adopting where it already is, not dragging it here.
				lastSyncSerial = 0;
			}
			break;
		}

		case PID_EXPORT_WIDTH:
		{
			const int w = static_cast< int >( std::lround( value ) );
			if( w != exportWidth )
			{
				exportWidth = w;
				// Changing it points at a different cache folder, so the deck has to be
				// resolved again — but only when there is one.
				if( !deckFile.empty() )
					RequestDeckLoad( false );
			}
			break;
		}

		default: return FF_FAIL;
		}
		return FF_SUCCESS;
	}
	catch( ... )
	{
		return FF_SUCCESS;
	}
}

float LXSlideDeck::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PID_RELOAD: return reloadDown;
	case PID_EXPORT_DECK: return exportDown;
	case PID_STEP: return static_cast< float >( stepParam );
	case PID_NEXT: return nextDown;
	case PID_PREV: return prevDown;
	case PID_FADE_TIME: return fadeTime;
	case PID_FADE_CURVE: return static_cast< float >( fadeCurve );
	case PID_AUTOPILOT: return autopilot ? 1.0f : 0.0f;
	case PID_INTERVAL: return interval;
	case PID_LOOP: return loopDeck ? 1.0f : 0.0f;
	case PID_SCALE_MODE: return static_cast< float >( static_cast< int >( scaleMode ) );
	case PID_PREVIEW: return static_cast< float >( static_cast< int >( preview ) );
	case PID_SYNC: return static_cast< float >( syncGroup );
	case PID_EXPORT_WIDTH: return static_cast< float >( exportWidth );
	default: return 0.0f;
	}
}

FFResult LXSlideDeck::SetTextParameter( unsigned int index, const char* value )
{
	try
	{
		if( index == PID_DECK_FILE )
		{
			const std::string incoming = value ? value : "";
			if( incoming == deckFile )
				return FF_SUCCESS;
			deckFile = incoming;
			{
				std::lock_guard< std::mutex > lock( textMutex );
				deckFileForHost = deckFile;
			}
			// This is also the path taken when Resolume restores a saved composition
			// (SPEC §6), which is exactly the behaviour we want: cache hit, no reconvert.
			RequestDeckLoad( false );
			return FF_SUCCESS;
		}
		if( index == PID_STATUS )
			return FF_SUCCESS;// read-only in spirit; the SDK has no flag for it yet

		return FF_FAIL;
	}
	catch( ... )
	{
		return FF_SUCCESS;
	}
}

char* LXSlideDeck::GetTextParameter( unsigned int index )
{
	try
	{
		std::lock_guard< std::mutex > lock( textMutex );
		if( index == PID_DECK_FILE )
		{
			deckFileForHost = deckFile;
			return const_cast< char* >( deckFileForHost.c_str() );
		}
		if( index == PID_STATUS )
		{
			statusForHost = statusText + diagnostics;
			return const_cast< char* >( statusForHost.c_str() );
		}
	}
	catch( ... )
	{
	}
	return const_cast< char* >( "" );
}
}// namespace lxsd
