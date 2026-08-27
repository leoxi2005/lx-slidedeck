#include "ComDispatch.h"

#if defined( _WIN32 )

namespace lxsd
{
namespace com
{
ComError::ComError( HRESULT hr, const std::string& message ) :
	std::runtime_error( message ),
	result( hr )
{
}

std::wstring Widen( const std::string& utf8 )
{
	if( utf8.empty() )
		return {};
	const int size = MultiByteToWideChar( CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0 );
	std::wstring wide( size, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[ 0 ], size );
	return wide;
}

std::string Narrow( const std::wstring& wide )
{
	if( wide.empty() )
		return {};
	const int size = WideCharToMultiByte( CP_UTF8, 0, wide.c_str(), (int)wide.size(),
										  nullptr, 0, nullptr, nullptr );
	std::string utf8( size, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, wide.c_str(), (int)wide.size(), &utf8[ 0 ], size, nullptr, nullptr );
	return utf8;
}

//--------------------------------------------------------------------------------------
Variant::Variant()
{
	VariantInit( &value );
}

Variant::~Variant()
{
	VariantClear( &value );
}

Variant::Variant( const Variant& other )
{
	VariantInit( &value );
	VariantCopy( &value, const_cast< VARIANT* >( &other.value ) );
}

Variant::Variant( Variant&& other ) noexcept
{
	value = other.value;
	VariantInit( &other.value );
}

Variant& Variant::operator=( const Variant& other )
{
	if( this != &other )
	{
		VariantClear( &value );
		VariantCopy( &value, const_cast< VARIANT* >( &other.value ) );
	}
	return *this;
}

Variant& Variant::operator=( Variant&& other ) noexcept
{
	if( this != &other )
	{
		VariantClear( &value );
		value = other.value;
		VariantInit( &other.value );
	}
	return *this;
}

Variant Variant::Long( long v )
{
	Variant out;
	out.value.vt   = VT_I4;
	out.value.lVal = v;
	return out;
}

Variant Variant::Float( float v )
{
	Variant out;
	out.value.vt     = VT_R4;
	out.value.fltVal = v;
	return out;
}

Variant Variant::Text( const std::wstring& v )
{
	Variant out;
	out.value.vt      = VT_BSTR;
	out.value.bstrVal = SysAllocStringLen( v.c_str(), (UINT)v.size() );
	return out;
}

long Variant::AsLong() const
{
	VARIANT converted;
	VariantInit( &converted );
	if( FAILED( VariantChangeType( &converted, const_cast< VARIANT* >( &value ), 0, VT_I4 ) ) )
		return 0;
	const long result = converted.lVal;
	VariantClear( &converted );
	return result;
}

std::wstring Variant::AsText() const
{
	VARIANT converted;
	VariantInit( &converted );
	if( FAILED( VariantChangeType( &converted, const_cast< VARIANT* >( &value ), 0, VT_BSTR ) ) )
		return {};
	std::wstring result = converted.bstrVal ? std::wstring( converted.bstrVal, SysStringLen( converted.bstrVal ) )
											: std::wstring();
	VariantClear( &converted );
	return result;
}

IDispatch* Variant::AsDispatch() const
{
	if( value.vt == VT_DISPATCH )
		return value.pdispVal;
	if( value.vt == ( VT_DISPATCH | VT_BYREF ) && value.ppdispVal != nullptr )
		return *value.ppdispVal;
	return nullptr;
}

//--------------------------------------------------------------------------------------
Dispatch::Dispatch( IDispatch* p, bool addReference ) :
	pointer( p )
{
	if( pointer != nullptr && addReference )
		pointer->AddRef();
}

Dispatch::~Dispatch()
{
	if( pointer != nullptr )
		pointer->Release();
}

Dispatch::Dispatch( const Dispatch& other ) :
	pointer( other.pointer )
{
	if( pointer != nullptr )
		pointer->AddRef();
}

Dispatch::Dispatch( Dispatch&& other ) noexcept :
	pointer( other.pointer )
{
	other.pointer = nullptr;
}

Dispatch& Dispatch::operator=( const Dispatch& other )
{
	if( this != &other )
	{
		if( other.pointer != nullptr )
			other.pointer->AddRef();
		if( pointer != nullptr )
			pointer->Release();
		pointer = other.pointer;
	}
	return *this;
}

Dispatch& Dispatch::operator=( Dispatch&& other ) noexcept
{
	if( this != &other )
	{
		if( pointer != nullptr )
			pointer->Release();
		pointer       = other.pointer;
		other.pointer = nullptr;
	}
	return *this;
}

bool Dispatch::IsRegistered( const wchar_t* progId )
{
	CLSID clsid;
	return SUCCEEDED( CLSIDFromProgID( progId, &clsid ) );
}

Dispatch Dispatch::CreateObject( const wchar_t* progId )
{
	CLSID clsid;
	HRESULT hr = CLSIDFromProgID( progId, &clsid );
	if( FAILED( hr ) )
		throw ComError( hr, std::string( "not installed: " ) + Narrow( progId ) );

	IDispatch* created = nullptr;
	hr = CoCreateInstance( clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
						   IID_IDispatch, reinterpret_cast< void** >( &created ) );
	if( FAILED( hr ) || created == nullptr )
		throw ComError( hr, std::string( "could not start " ) + Narrow( progId ) );

	return Dispatch( created );
}

Variant Dispatch::Invoke( const wchar_t* name, WORD flags, const std::vector< Variant >& args ) const
{
	if( pointer == nullptr )
		throw ComError( E_POINTER, std::string( "no object to call " ) + Narrow( name ) + " on" );

	DISPID dispid  = 0;
	LPOLESTR asked = const_cast< LPOLESTR >( name );
	HRESULT hr     = pointer->GetIDsOfNames( IID_NULL, &asked, 1, LOCALE_USER_DEFAULT, &dispid );
	if( FAILED( hr ) )
		throw ComError( hr, std::string( "unknown name: " ) + Narrow( name ) );

	// IDispatch wants the arguments backwards.
	std::vector< VARIANT > reversed;
	reversed.reserve( args.size() );
	for( auto it = args.rbegin(); it != args.rend(); ++it )
		reversed.push_back( it->Raw() );

	DISPPARAMS params{};
	params.cArgs  = static_cast< UINT >( reversed.size() );
	params.rgvarg = reversed.empty() ? nullptr : reversed.data();

	// A property assignment has to name its single argument, or the call is rejected.
	DISPID putId = DISPID_PROPERTYPUT;
	if( flags & DISPATCH_PROPERTYPUT )
	{
		params.cNamedArgs         = 1;
		params.rgdispidNamedArgs  = &putId;
	}

	Variant result;
	EXCEPINFO exception{};
	UINT badArgument = 0;
	hr = pointer->Invoke( dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &params,
						  &result.Raw(), &exception, &badArgument );
	if( FAILED( hr ) )
	{
		std::string message = Narrow( name );
		if( exception.bstrDescription != nullptr )
			message += ": " + Narrow( std::wstring( exception.bstrDescription,
													SysStringLen( exception.bstrDescription ) ) );
		else
			message += " failed";
		SysFreeString( exception.bstrSource );
		SysFreeString( exception.bstrDescription );
		SysFreeString( exception.bstrHelpFile );
		throw ComError( hr, message );
	}
	SysFreeString( exception.bstrSource );
	SysFreeString( exception.bstrDescription );
	SysFreeString( exception.bstrHelpFile );
	return result;
}

Dispatch Dispatch::GetObject( const wchar_t* name, const std::vector< Variant >& args ) const
{
	Variant result   = Invoke( name, DISPATCH_PROPERTYGET | DISPATCH_METHOD, args );
	IDispatch* found = result.AsDispatch();
	if( found == nullptr )
		throw ComError( E_NOINTERFACE, Narrow( name ) + " did not return an object" );
	return Dispatch( found, /*addReference*/ true );
}

long Dispatch::GetLong( const wchar_t* name, const std::vector< Variant >& args ) const
{
	return Invoke( name, DISPATCH_PROPERTYGET | DISPATCH_METHOD, args ).AsLong();
}

void Dispatch::Put( const wchar_t* name, const Variant& value ) const
{
	Invoke( name, DISPATCH_PROPERTYPUT, { value } );
}

Variant Dispatch::Call( const wchar_t* name, const std::vector< Variant >& args ) const
{
	return Invoke( name, DISPATCH_METHOD, args );
}
}// namespace com
}// namespace lxsd

#endif// _WIN32
