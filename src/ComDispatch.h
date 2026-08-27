// LX SlideDeck — late-bound COM automation, Windows only.
//
// Written against raw IDispatch rather than #import / _com_ptr_t / comsuppw, for two
// reasons: those are MSVC-only, and this file has to keep compiling under a cross-compiler
// so the Windows half can be checked from a Mac. Nothing here needs a type library.
#pragma once

#if defined( _WIN32 )

#	include <stdexcept>
#	include <string>
#	include <vector>

#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>

#	include <oaidl.h>
#	include <objbase.h>
#	include <oleauto.h>

namespace lxsd
{
namespace com
{
/// MsoTriState. Effect.Exit, Shape.Visible and Application.Visible are tri-states, not
/// booleans — pass these, never C++ true / false (SPEC §3.4).
constexpr long kMsoTrue  = -1;
constexpr long kMsoFalse = 0;

class ComError : public std::runtime_error
{
public:
	ComError( HRESULT hr, const std::string& message );
	HRESULT Result() const
	{
		return result;
	}

private:
	HRESULT result;
};

std::wstring Widen( const std::string& utf8 );
std::string Narrow( const std::wstring& wide );

/// Owns a VARIANT and clears it on the way out.
class Variant
{
public:
	Variant();
	~Variant();
	Variant( const Variant& other );
	Variant( Variant&& other ) noexcept;
	Variant& operator=( const Variant& other );
	Variant& operator=( Variant&& other ) noexcept;

	static Variant Long( long value );
	static Variant Float( float value );
	static Variant Text( const std::wstring& value );

	VARIANT& Raw()
	{
		return value;
	}
	const VARIANT& Raw() const
	{
		return value;
	}

	long AsLong() const;
	std::wstring AsText() const;
	/// Returns the borrowed IDispatch, or nullptr. Does not add a reference.
	IDispatch* AsDispatch() const;

private:
	VARIANT value;
};

/// A COM object addressed by name.
class Dispatch
{
public:
	Dispatch() = default;
	/// Takes ownership of `pointer` unless `addReference` is true.
	explicit Dispatch( IDispatch* pointer, bool addReference = false );
	~Dispatch();
	Dispatch( const Dispatch& other );
	Dispatch( Dispatch&& other ) noexcept;
	Dispatch& operator=( const Dispatch& other );
	Dispatch& operator=( Dispatch&& other ) noexcept;

	bool Valid() const
	{
		return pointer != nullptr;
	}
	IDispatch* Get() const
	{
		return pointer;
	}

	/// Starts (or attaches to) an automation server, e.g. "PowerPoint.Application".
	static Dispatch CreateObject( const wchar_t* progId );
	/// Whether a ProgID is registered at all — no server is started.
	static bool IsRegistered( const wchar_t* progId );

	/// Reads a property, or calls a method, that returns an object.
	Dispatch GetObject( const wchar_t* name, const std::vector< Variant >& args = {} ) const;
	/// Reads a property that returns a number.
	long GetLong( const wchar_t* name, const std::vector< Variant >& args = {} ) const;
	/// Assigns a property.
	void Put( const wchar_t* name, const Variant& value ) const;
	/// Calls a method, keeping whatever it returns.
	Variant Call( const wchar_t* name, const std::vector< Variant >& args = {} ) const;

	/// The one place that actually talks to IDispatch. `flags` is a DISPATCH_ constant.
	Variant Invoke( const wchar_t* name, WORD flags, const std::vector< Variant >& args ) const;

private:
	IDispatch* pointer = nullptr;
};
}// namespace com
}// namespace lxsd

#endif// _WIN32
