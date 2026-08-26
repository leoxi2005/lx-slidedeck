// LX SlideDeck — the handful of things that genuinely differ between Windows and macOS.
// Everything else in this plugin is portable C++17.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lxsd
{
/// Per-user cache root.
///   Windows  %LOCALAPPDATA%\LXSlideDeck
///   macOS    ~/Library/Caches/LXSlideDeck
std::string CacheRoot();

/// Scratch directory for the intermediate single-step .pptx files.
std::string TempRoot();

bool PathExists( const std::string& path );
bool IsDirectory( const std::string& path );
bool EnsureDirectory( const std::string& path );
bool RemoveDirectoryTree( const std::string& path );

/// Seconds since the epoch, 0 when unavailable. Part of the cache key.
int64_t FileMtime( const std::string& path );
uint64_t FileSize( const std::string& path );

/// Files directly inside `dir` whose extension matches (compared lowercase, no dot),
/// sorted so that step_2 comes before step_10.
std::vector< std::string > ListFilesByExtension( const std::string& dir, const std::string& ext );

std::string ParentDirectory( const std::string& path );
std::string FileName( const std::string& path );
std::string FileStem( const std::string& path );
std::string ExtensionLower( const std::string& path );
std::string JoinPath( const std::string& a, const std::string& b );
std::string AbsolutePath( const std::string& path );

bool ReadWholeFile( const std::string& path, std::string& out );
bool WriteWholeFile( const std::string& path, const std::string& data );

/// Runs a program and waits for it, with no console window on Windows.
/// `timeoutSeconds` <= 0 waits forever. Returns false on spawn failure or timeout;
/// `exitCode` is only meaningful when it returns true.
/// When `stdoutPath` is given, the program's stdout and stderr are written there.
bool RunProcess( const std::string& executable,
				 const std::vector< std::string >& args,
				 int timeoutSeconds,
				 int& exitCode,
				 const std::string* stdoutPath = nullptr );

/// Absolute path to a LibreOffice `soffice` binary, or empty when none is installed.
/// Looks in every place a Mac or Windows machine normally puts it, including Homebrew.
std::string FindLibreOffice();

/// Total size on disk of a directory tree, in bytes.
uint64_t DirectorySize( const std::string& path );

/// Deletes cache subdirectories whose mtime is older than `days` (SPEC §10).
void PruneOldCaches( const std::string& cacheRoot, int days );
}// namespace lxsd
