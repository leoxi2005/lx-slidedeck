#include "Platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined( _WIN32 )
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#	include <shlobj.h>
#else
#	include <errno.h>
#	include <fcntl.h>
#	include <signal.h>
#	include <spawn.h>
#	include <sys/wait.h>
#	include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace lxsd
{
namespace
{
std::string EnvVar( const char* name )
{
#if defined( _WIN32 )
	char* buf   = nullptr;
	size_t size = 0;
	if( _dupenv_s( &buf, &size, name ) != 0 || buf == nullptr )
		return {};
	std::string out( buf );
	free( buf );
	return out;
#else
	const char* v = std::getenv( name );
	return v ? std::string( v ) : std::string();
#endif
}

/// Compares filenames so that step_2 sorts before step_10.
bool NaturalLess( const std::string& a, const std::string& b )
{
	size_t i = 0, j = 0;
	while( i < a.size() && j < b.size() )
	{
		const bool da = std::isdigit( static_cast< unsigned char >( a[ i ] ) ) != 0;
		const bool db = std::isdigit( static_cast< unsigned char >( b[ j ] ) ) != 0;
		if( da && db )
		{
			size_t si = i, sj = j;
			while( i < a.size() && std::isdigit( static_cast< unsigned char >( a[ i ] ) ) )
				++i;
			while( j < b.size() && std::isdigit( static_cast< unsigned char >( b[ j ] ) ) )
				++j;
			std::string na = a.substr( si, i - si );
			std::string nb = b.substr( sj, j - sj );
			na.erase( 0, std::min( na.find_first_not_of( '0' ), na.size() - 1 ) );
			nb.erase( 0, std::min( nb.find_first_not_of( '0' ), nb.size() - 1 ) );
			if( na.size() != nb.size() )
				return na.size() < nb.size();
			if( na != nb )
				return na < nb;
			continue;
		}
		const char ca = static_cast< char >( std::tolower( static_cast< unsigned char >( a[ i ] ) ) );
		const char cb = static_cast< char >( std::tolower( static_cast< unsigned char >( b[ j ] ) ) );
		if( ca != cb )
			return ca < cb;
		++i;
		++j;
	}
	return a.size() - i < b.size() - j;
}
}// namespace

std::string CacheRoot()
{
#if defined( _WIN32 )
	std::string base = EnvVar( "LOCALAPPDATA" );
	if( base.empty() )
		base = EnvVar( "TEMP" );
	return JoinPath( base, "LXSlideDeck" );
#else
	std::string home = EnvVar( "HOME" );
	if( home.empty() )
		home = "/tmp";
	return JoinPath( JoinPath( home, "Library/Caches" ), "LXSlideDeck" );
#endif
}

std::string TempRoot()
{
	return JoinPath( CacheRoot(), "tmp" );
}

bool PathExists( const std::string& path )
{
	std::error_code ec;
	return !path.empty() && fs::exists( fs::u8path( path ), ec );
}

bool IsDirectory( const std::string& path )
{
	std::error_code ec;
	return !path.empty() && fs::is_directory( fs::u8path( path ), ec );
}

bool EnsureDirectory( const std::string& path )
{
	std::error_code ec;
	if( fs::is_directory( fs::u8path( path ), ec ) )
		return true;
	fs::create_directories( fs::u8path( path ), ec );
	return !ec;
}

bool RemoveDirectoryTree( const std::string& path )
{
	if( path.empty() )
		return false;
	std::error_code ec;
	fs::remove_all( fs::u8path( path ), ec );
	return !ec;
}

int64_t FileMtime( const std::string& path )
{
	std::error_code ec;
	auto t = fs::last_write_time( fs::u8path( path ), ec );
	if( ec )
		return 0;
	// No portable clock_cast before C++20; the raw tick count is stable enough for a cache key.
	return static_cast< int64_t >( t.time_since_epoch().count() );
}

uint64_t FileSize( const std::string& path )
{
	std::error_code ec;
	auto s = fs::file_size( fs::u8path( path ), ec );
	return ec ? 0u : static_cast< uint64_t >( s );
}

std::vector< std::string > ListFilesByExtension( const std::string& dir, const std::string& ext )
{
	std::vector< std::string > out;
	std::error_code ec;
	for( fs::directory_iterator it( fs::u8path( dir ), ec ), end; it != end && !ec; it.increment( ec ) )
	{
		if( !it->is_regular_file( ec ) )
			continue;
		std::string p = it->path().u8string();
		if( ExtensionLower( p ) == ext )
			out.push_back( p );
	}
	std::sort( out.begin(), out.end(), []( const std::string& a, const std::string& b ) {
		return NaturalLess( FileName( a ), FileName( b ) );
	} );
	return out;
}

std::string ParentDirectory( const std::string& path )
{
	return fs::u8path( path ).parent_path().u8string();
}

std::string FileName( const std::string& path )
{
	return fs::u8path( path ).filename().u8string();
}

std::string FileStem( const std::string& path )
{
	return fs::u8path( path ).stem().u8string();
}

std::string ExtensionLower( const std::string& path )
{
	std::string e = fs::u8path( path ).extension().u8string();
	if( !e.empty() && e[ 0 ] == '.' )
		e.erase( 0, 1 );
	std::transform( e.begin(), e.end(), e.begin(), []( unsigned char c ) {
		return static_cast< char >( std::tolower( c ) );
	} );
	return e;
}

std::string JoinPath( const std::string& a, const std::string& b )
{
	if( a.empty() )
		return b;
	if( b.empty() )
		return a;
	return ( fs::u8path( a ) / fs::u8path( b ) ).u8string();
}

std::string AbsolutePath( const std::string& path )
{
	std::error_code ec;
	auto p = fs::absolute( fs::u8path( path ), ec );
	if( ec )
		return path;
	return p.lexically_normal().u8string();
}

bool ReadWholeFile( const std::string& path, std::string& out )
{
	std::ifstream in( fs::u8path( path ), std::ios::binary );
	if( !in )
		return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

bool WriteWholeFile( const std::string& path, const std::string& data )
{
	EnsureDirectory( ParentDirectory( path ) );
	std::ofstream o( fs::u8path( path ), std::ios::binary | std::ios::trunc );
	if( !o )
		return false;
	o.write( data.data(), static_cast< std::streamsize >( data.size() ) );
	return o.good();
}

#if defined( _WIN32 )
namespace
{
/// Windows takes one command line, not an argv, and the quoting rules are its own.
std::wstring Widen( const std::string& s )
{
	if( s.empty() )
		return {};
	int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[ 0 ], n );
	return w;
}

std::wstring QuoteArg( const std::string& arg )
{
	std::wstring a = Widen( arg );
	if( !a.empty() && a.find_first_of( L" \t\"" ) == std::wstring::npos )
		return a;
	std::wstring out = L"\"";
	size_t backslashes = 0;
	for( wchar_t c : a )
	{
		if( c == L'\\' )
		{
			++backslashes;
			continue;
		}
		if( c == L'"' )
		{
			out.append( backslashes * 2 + 1, L'\\' );
			backslashes = 0;
			out.push_back( L'"' );
			continue;
		}
		out.append( backslashes, L'\\' );
		backslashes = 0;
		out.push_back( c );
	}
	out.append( backslashes * 2, L'\\' );
	out.push_back( L'"' );
	return out;
}
}// namespace

bool RunProcess( const std::string& executable,
				 const std::vector< std::string >& args,
				 int timeoutSeconds,
				 int& exitCode,
				 const std::string* stdoutPath )
{
	exitCode         = -1;
	std::wstring cmd = QuoteArg( executable );
	for( const std::string& a : args )
	{
		cmd += L' ';
		cmd += QuoteArg( a );
	}

	STARTUPINFOW si{};
	si.cb          = sizeof( si );
	si.dwFlags     = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};

	HANDLE outFile = INVALID_HANDLE_VALUE;
	if( stdoutPath != nullptr )
	{
		SECURITY_ATTRIBUTES sa{};
		sa.nLength        = sizeof( sa );
		sa.bInheritHandle = TRUE;
		outFile = CreateFileW( Widen( *stdoutPath ).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
							   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		if( outFile != INVALID_HANDLE_VALUE )
		{
			si.dwFlags |= STARTF_USESTDHANDLES;
			si.hStdOutput = outFile;
			si.hStdError  = outFile;
		}
	}

	std::vector< wchar_t > buffer( cmd.begin(), cmd.end() );
	buffer.push_back( L'\0' );
	const BOOL inheritHandles = outFile != INVALID_HANDLE_VALUE ? TRUE : FALSE;
	if( !CreateProcessW( nullptr, buffer.data(), nullptr, nullptr, inheritHandles,
						 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
	{
		if( outFile != INVALID_HANDLE_VALUE )
			CloseHandle( outFile );
		return false;
	}
	if( outFile != INVALID_HANDLE_VALUE )
		CloseHandle( outFile );

	DWORD waitMs = timeoutSeconds > 0 ? static_cast< DWORD >( timeoutSeconds ) * 1000u : INFINITE;
	DWORD wait   = WaitForSingleObject( pi.hProcess, waitMs );
	if( wait != WAIT_OBJECT_0 )
	{
		TerminateProcess( pi.hProcess, 1 );
		CloseHandle( pi.hProcess );
		CloseHandle( pi.hThread );
		return false;
	}
	DWORD code = 1;
	GetExitCodeProcess( pi.hProcess, &code );
	exitCode = static_cast< int >( code );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return true;
}
#else
bool RunProcess( const std::string& executable,
				 const std::vector< std::string >& args,
				 int timeoutSeconds,
				 int& exitCode,
				 const std::string* stdoutPath )
{
	exitCode = -1;

	std::vector< std::string > storage;
	storage.reserve( args.size() + 1 );
	storage.push_back( executable );
	for( const std::string& a : args )
		storage.push_back( a );

	std::vector< char* > argv;
	argv.reserve( storage.size() + 1 );
	for( std::string& s : storage )
		argv.push_back( &s[ 0 ] );
	argv.push_back( nullptr );

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_t* actionsPtr = nullptr;
	if( stdoutPath != nullptr && posix_spawn_file_actions_init( &actions ) == 0 )
	{
		posix_spawn_file_actions_addopen( &actions, STDOUT_FILENO, stdoutPath->c_str(),
										  O_WRONLY | O_CREAT | O_TRUNC, 0644 );
		posix_spawn_file_actions_adddup2( &actions, STDOUT_FILENO, STDERR_FILENO );
		actionsPtr = &actions;
	}

	pid_t pid       = 0;
	const int spawn = posix_spawn( &pid, executable.c_str(), actionsPtr, nullptr, argv.data(), environ );
	if( actionsPtr != nullptr )
		posix_spawn_file_actions_destroy( actionsPtr );
	if( spawn != 0 )
		return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( timeoutSeconds > 0 ? timeoutSeconds : 0 );
	for( ;; )
	{
		int status = 0;
		pid_t r    = waitpid( pid, &status, timeoutSeconds > 0 ? WNOHANG : 0 );
		if( r == pid )
		{
			exitCode = WIFEXITED( status ) ? WEXITSTATUS( status ) : 1;
			return true;
		}
		if( r < 0 )
			return false;
		if( timeoutSeconds > 0 && std::chrono::steady_clock::now() > deadline )
		{
			kill( pid, SIGKILL );
			waitpid( pid, nullptr, 0 );
			return false;
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
	}
}
#endif

std::string FindLibreOffice()
{
	std::vector< std::string > candidates;
#if defined( _WIN32 )
	for( const char* env : { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" } )
	{
		std::string base = EnvVar( env );
		if( !base.empty() )
			candidates.push_back( JoinPath( base, "LibreOffice\\program\\soffice.exe" ) );
	}
#else
	candidates.push_back( "/Applications/LibreOffice.app/Contents/MacOS/soffice" );
	candidates.push_back( "/Applications/LibreOfficeDev.app/Contents/MacOS/soffice" );
	candidates.push_back( "/opt/homebrew/bin/soffice" );
	candidates.push_back( "/usr/local/bin/soffice" );
	candidates.push_back( "/opt/local/bin/soffice" );// MacPorts
	candidates.push_back( "/usr/bin/soffice" );
	std::string home = EnvVar( "HOME" );
	if( !home.empty() )
	{
		candidates.push_back( JoinPath( home, "Applications/LibreOffice.app/Contents/MacOS/soffice" ) );
		candidates.push_back( JoinPath( home, "Applications/Setapp/LibreOffice.app/Contents/MacOS/soffice" ) );
	}
#endif
	for( const std::string& c : candidates )
		if( PathExists( c ) )
			return c;
	return {};
}

uint64_t DirectorySize( const std::string& path )
{
	uint64_t total = 0;
	std::error_code ec;
	for( fs::recursive_directory_iterator it( fs::u8path( path ), ec ), end; it != end && !ec; it.increment( ec ) )
	{
		std::error_code fec;
		if( it->is_regular_file( fec ) )
			total += static_cast< uint64_t >( it->file_size( fec ) );
	}
	return total;
}

void PruneOldCaches( const std::string& cacheRoot, int days )
{
	if( days <= 0 || !IsDirectory( cacheRoot ) )
		return;
	std::error_code ec;
	const auto now    = fs::file_time_type::clock::now();
	const auto maxAge = std::chrono::hours( 24 * days );
	for( fs::directory_iterator it( fs::u8path( cacheRoot ), ec ), end; it != end && !ec; it.increment( ec ) )
	{
		std::error_code fec;
		if( !it->is_directory( fec ) )
			continue;
		if( it->path().filename().u8string() == "tmp" )
			continue;
		auto t = fs::last_write_time( it->path(), fec );
		if( fec )
			continue;
		if( now - t > maxAge )
			fs::remove_all( it->path(), fec );
	}
}
}// namespace lxsd
