#include "Zip.h"

#include <algorithm>

#include "Platform.h"
#include "miniz.h"

namespace lxsd
{
bool ZipPackage::Has( const std::string& name ) const
{
	return Get( name ) != nullptr;
}

const std::string* ZipPackage::Get( const std::string& name ) const
{
	for( const Entry& e : entries )
		if( e.first == name )
			return &e.second;
	return nullptr;
}

void ZipPackage::Set( const std::string& name, std::string data )
{
	for( Entry& e : entries )
	{
		if( e.first == name )
		{
			e.second = std::move( data );
			return;
		}
	}
	entries.emplace_back( name, std::move( data ) );
}

void ZipPackage::Remove( const std::string& name )
{
	entries.erase( std::remove_if( entries.begin(), entries.end(),
								   [ & ]( const Entry& e ) { return e.first == name; } ),
				   entries.end() );
}

bool ReadZip( const std::string& path, ZipPackage& out, std::string& error )
{
	// Read the file ourselves rather than letting miniz open it, so that non-ASCII paths
	// behave the same on both platforms.
	std::string bytes;
	if( !ReadWholeFile( path, bytes ) )
	{
		error = "cannot read " + FileName( path );
		return false;
	}

	mz_zip_archive zip;
	memset( &zip, 0, sizeof( zip ) );
	if( !mz_zip_reader_init_mem( &zip, bytes.data(), bytes.size(), 0 ) )
	{
		error = "not a valid .pptx (zip could not be opened)";
		return false;
	}

	const mz_uint count = mz_zip_reader_get_num_files( &zip );
	for( mz_uint i = 0; i < count; ++i )
	{
		mz_zip_archive_file_stat stat;
		if( !mz_zip_reader_file_stat( &zip, i, &stat ) )
			continue;
		if( mz_zip_reader_is_file_a_directory( &zip, i ) )
			continue;

		size_t size    = 0;
		void* uncomp   = mz_zip_reader_extract_to_heap( &zip, i, &size, 0 );
		if( uncomp == nullptr )
		{
			mz_zip_reader_end( &zip );
			error = std::string( "cannot unpack " ) + stat.m_filename;
			return false;
		}
		out.Set( stat.m_filename, std::string( static_cast< const char* >( uncomp ), size ) );
		mz_free( uncomp );
	}

	mz_zip_reader_end( &zip );
	if( out.Size() == 0 )
	{
		error = "the file is empty";
		return false;
	}
	return true;
}

namespace
{
void PutU16( std::string& out, uint16_t v )
{
	out.push_back( static_cast< char >( v & 0xFF ) );
	out.push_back( static_cast< char >( ( v >> 8 ) & 0xFF ) );
}

void PutU32( std::string& out, uint32_t v )
{
	for( int i = 0; i < 4; ++i )
		out.push_back( static_cast< char >( ( v >> ( i * 8 ) ) & 0xFF ) );
}
}// namespace

bool WriteZip( const std::string& path, const ZipPackage& package, std::string& error )
{
	// Written by hand rather than with mz_zip_writer.
	//
	// miniz's writer marks every entry with the data-descriptor bit and leaves the sizes
	// and crc zeroed in the local header. That is legal streaming zip, but LibreOffice
	// rejects the whole package with nothing more than "source file could not be loaded",
	// and OPC readers in general expect the sizes up front. Here every local header is
	// complete before its data is written, which is what a normal .pptx looks like.
	//
	// Only the compression itself is miniz's, through tdefl.
	std::string out;
	std::string central;
	uint16_t entryCount = 0;

	// A fixed timestamp (1980-01-01) keeps the output byte-identical between runs, which
	// makes it possible to tell a real difference from a rebuild when debugging.
	const uint16_t dosTime = 0;
	const uint16_t dosDate = 33;// 1980-01-01

	for( const auto& entry : package.Entries() )
	{
		const std::string& name = entry.first;
		const std::string& data = entry.second;

		const uint32_t crc = static_cast< uint32_t >(
			mz_crc32( MZ_CRC32_INIT, reinterpret_cast< const mz_uint8* >( data.data() ), data.size() ) );

		std::string payload;
		uint16_t method = 0;// stored
		if( !data.empty() )
		{
			size_t compressedSize = 0;
			void* compressed      = tdefl_compress_mem_to_heap(
                data.data(), data.size(), &compressedSize, TDEFL_DEFAULT_MAX_PROBES );
			if( compressed != nullptr )
			{
				if( compressedSize < data.size() )
				{
					payload.assign( static_cast< const char* >( compressed ), compressedSize );
					method = 8;// deflate
				}
				mz_free( compressed );
			}
		}
		if( method == 0 )
			payload = data;

		const uint32_t localOffset = static_cast< uint32_t >( out.size() );

		out += "PK\x03\x04";
		PutU16( out, 20 );  // version needed
		PutU16( out, 0x0800 );// utf-8 names, and crucially no data descriptor
		PutU16( out, method );
		PutU16( out, dosTime );
		PutU16( out, dosDate );
		PutU32( out, crc );
		PutU32( out, static_cast< uint32_t >( payload.size() ) );
		PutU32( out, static_cast< uint32_t >( data.size() ) );
		PutU16( out, static_cast< uint16_t >( name.size() ) );
		PutU16( out, 0 );// no extra field
		out += name;
		out += payload;

		central += "PK\x01\x02";
		PutU16( central, 20 );// version made by
		PutU16( central, 20 );// version needed
		PutU16( central, 0x0800 );
		PutU16( central, method );
		PutU16( central, dosTime );
		PutU16( central, dosDate );
		PutU32( central, crc );
		PutU32( central, static_cast< uint32_t >( payload.size() ) );
		PutU32( central, static_cast< uint32_t >( data.size() ) );
		PutU16( central, static_cast< uint16_t >( name.size() ) );
		PutU16( central, 0 );// extra
		PutU16( central, 0 );// comment
		PutU16( central, 0 );// disk number
		PutU16( central, 0 );// internal attributes
		PutU32( central, 0 );// external attributes
		PutU32( central, localOffset );
		central += name;

		++entryCount;
	}

	const uint32_t centralOffset = static_cast< uint32_t >( out.size() );
	out += central;
	out += "PK\x05\x06";
	PutU16( out, 0 );// disk
	PutU16( out, 0 );// disk with central directory
	PutU16( out, entryCount );
	PutU16( out, entryCount );
	PutU32( out, static_cast< uint32_t >( central.size() ) );
	PutU32( out, centralOffset );
	PutU16( out, 0 );// comment length

	EnsureDirectory( ParentDirectory( path ) );
	if( !WriteWholeFile( path, out ) )
	{
		error = "cannot write " + FileName( path );
		return false;
	}
	return true;
}
}// namespace lxsd
