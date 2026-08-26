// LX SlideDeck — the FFGL source plugin itself. Everything in this file runs on the host's
// render thread; anything that could block lives on the Worker (see Worker.h).
#pragma once

#include <FFGLSDK.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ScaleMode.h"
#include "Worker.h"

namespace lxsd
{
enum ParamID : FFUInt32
{
	PID_DECK_FILE = 0,
	PID_RELOAD,
	PID_STEP,
	PID_NEXT,
	PID_PREV,
	PID_FADE_TIME,
	PID_FADE_CURVE,
	PID_AUTOPILOT,
	PID_INTERVAL,
	PID_LOOP,
	PID_SCALE_MODE,
	PID_EXPORT_WIDTH,
	PID_PREVIEW,
	PID_SYNC,
	PID_EXPORT_DECK,
	PID_STATUS,
	PID_COUNT
};

/// What to do with the step the presenter has not reached yet.
enum class PreviewMode : int
{
	Off      = 0,//!< just the deck
	NextOnly = 1,//!< show the upcoming step instead — a monitor feed for the operator
	Split    = 2,//!< current on the left, upcoming on the right
	Corner   = 3,//!< upcoming as an inset in the bottom right corner
};

class LXSlideDeck : public CFFGLPlugin
{
public:
	LXSlideDeck();
	~LXSlideDeck() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

private:
	struct SlideTexture
	{
		GLuint id          = 0;
		int width          = 0;
		int height         = 0;
		uint64_t lastUsed  = 0;
	};

	void RequestDeckLoad( bool forceReload );
	void PumpDecodedImages();      //!< uploads at most 2 textures per frame (SPEC §4.2)
	void SyncWorkerState();
	void AdvanceAutopilot( float dt );
	void UpdateFade( float dt );
	void RequestPreload();         //!< SPEC §4.1 — k-1, k+1, k+2
	void EvictTextures();
	void UploadTexture( const DecodedImage& img );
	void ReleaseAllTextures();
	const SlideTexture* FindTexture( int step ) const;

	/// Which step the main image shows and which one the preview shows.
	/// In NextOnly the main image is already the upcoming step, so there is nothing left
	/// to preview.
	int MainStep() const;
	int MainPrevStep() const;
	int PreviewStep() const;
	int WrapStep( int step ) const;

	/// `broadcast` is false only when the move came from the sync group in the first place,
	/// so that two linked instances cannot bounce a step back and forth forever.
	void GoToStep( int step, bool fromHost, bool broadcast = true );
	void FollowSyncGroup();
	void PublishToSyncGroup();
	int ClampStep( int step ) const;
	void PublishStepToHost();
	void PublishStatusToHost();
	void UpdateDiagnostics( const SlideTexture* curTex );

	// ---- parameters -------------------------------------------------------------
	std::string deckFile;
	int stepParam        = 1;// 1-based, what the host shows
	float fadeTime       = 0.4f;
	int fadeCurve        = 1;// 0 linear, 1 smooth, 2 ease out
	bool autopilot       = false;
	float interval       = 5.0f;
	bool loopDeck        = true;
	ScaleMode scaleMode  = ScaleMode::Fit;
	int exportWidth      = 3840;
	PreviewMode preview  = PreviewMode::Off;
	int syncGroup        = 0;//!< 0 = off, otherwise the shared group this instance follows

	float reloadDown = 0.0f;
	float exportDown = 0.0f;
	float nextDown   = 0.0f;
	float prevDown   = 0.0f;

	// ---- playback state ---------------------------------------------------------
	int currentStep = 0;//!< 0-based
	int prevStep    = 0;
	float fadeT     = 1.0f;
	float autopilotClock = 0.0f;
	float placeholderClock = 0.0f;//!< drives the sweep on the converting screen
	uint64_t frameCounter = 0;
	std::chrono::steady_clock::time_point lastFrameTime;
	bool haveLastFrameTime = false;

	/// The size actually being rendered into, read from GL every frame. currentViewport,
	/// which FFGL fills in at InitGL, goes stale as soon as the host changes its mind.
	int renderWidth  = 0;
	int renderHeight = 0;

	int knownStepCount        = 0;
	uint64_t knownStatusSerial = 0;
	uint64_t deckGeneration    = 0;
	uint64_t lastSyncSerial    = 0;
	std::unordered_set< int > requestedSteps;

	// ---- gl ---------------------------------------------------------------------
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
	std::unordered_map< int, SlideTexture > textures;
	static constexpr size_t kMaxTextures = 8;//!< SPEC §4.1
	/// Upper bound the host is told about for Step, since it only ever asks once.
	static constexpr int kMaxAddressableStep = 999;

	GLint locTexPrev = -1, locTexCur = -1, locTexNext = -1;
	GLint locMixT = -1, locUvPrev = -1, locUvCur = -1, locUvNext = -1;
	GLint locHasPrev = -1, locHasCur = -1, locHasNext = -1;
	GLint locRectMain = -1, locRectPreview = -1, locBorderWidth = -1;
	GLint locMode = -1, locProgress = -1, locSteps = -1, locClock = -1, locTint = -1;

	// ---- worker -----------------------------------------------------------------
	Worker worker;
	mutable std::mutex textMutex;
	std::string statusText = "Idle";
	std::string diagnostics;
	std::string statusForHost;
	std::string deckFileForHost;
};
}// namespace lxsd
