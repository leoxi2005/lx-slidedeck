// LX SlideDeck — turning a .pptx into a folder of one-PNG-per-animation-step.
//
// The analysis half (which steps exist, what is visible in each) is portable: it reads the
// OOXML inside the .pptx directly. Only the final "draw this slide" step differs per
// machine — PowerPoint on Windows, LibreOffice on macOS — and that choice lives behind
// SelectRenderBackend().
#pragma once

#include <functional>
#include <string>

#include "Manifest.h"

namespace lxsd
{
enum class RenderBackend : int
{
	Auto        = 0,
	PowerPoint  = 1,
	LibreOffice = 2,
};

struct ConvertRequest
{
	std::string sourcePath; //!< absolute path to the .pptx / .ppt
	std::string cacheDir;   //!< where step_0001.png … and manifest.json go
	int exportWidth       = 3840;
	RenderBackend backend = RenderBackend::Auto;
	int timeoutSeconds    = 300;//!< SPEC §7 — give up rather than hang a show
};

struct ConvertCallbacks
{
	/// Called as slides finish. Safe to be empty.
	std::function< void( int done, int total, const std::string& note ) > progress;
	/// Return true to abort — the plugin uses this when the user picks another deck.
	std::function< bool() > cancelled;
};

/// What this machine can actually render with, and whether it works.
struct RendererStatus
{
	RenderBackend backend = RenderBackend::Auto;
	bool usable           = false;
	std::string name;   //!< "LibreOffice 26.2.5.2", "PowerPoint", …
	std::string path;   //!< where it was found
	std::string problem;//!< what to tell the user when it is not usable
};

/// Finds the renderer and proves it runs, rather than just checking a file exists — a
/// LibreOffice that is present but broken (a half-finished install, a quarantined copy,
/// one that needs a Gatekeeper approval nobody has clicked) looks identical on disk to a
/// working one, and the difference only shows up mid-show otherwise.
///
/// The probe runs once per process and the answer is cached; call it freely.
const RendererStatus& ProbeRenderer();

/// Runs the probe again, for the case where the user installs LibreOffice while Resolume
/// is still open.
const RendererStatus& ReprobeRenderer();

/// Which backend is actually usable on this machine right now.
RenderBackend SelectRenderBackend( RenderBackend requested, std::string& why );

/// Does the whole job. On success `cacheDir` holds the images and `outManifest` describes
/// them; on failure `error` is a message fit to show in the Status parameter.
bool ConvertDeck( const ConvertRequest& request,
				  const ConvertCallbacks& callbacks,
				  DeckManifest& outManifest,
				  std::string& error );
}// namespace lxsd
