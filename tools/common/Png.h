#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/**
	A minimal PNG writer and an RGBA sampler, shared by both harnesses.

	zlib ships with the OS, which is why this is fifty lines rather than a
	vendored dependency -- the same call the rest of the fleet makes.

	**Both functions take buffers that are bottom-up**, because that is what
	everything in this repo holds: `pixels::Convert` flips libretro's top-left
	software framebuffer so the result can go straight into a GL texture, and
	`glReadPixels` hands back bottom-up rows for the same reason. PNG is
	top-left origin, so `WritePng` un-flips on the way out and `Sample` takes
	picture coordinates with y = 0 at the top. Nothing else in either harness
	has to think about it.
*/
namespace cartridge::png
{

struct Rgb
{
	int r = 0, g = 0, b = 0, a = 0;

	bool Near( int rr, int gg, int bb, int tolerance = 8 ) const
	{
		auto close = []( int x, int y, int t ) { return ( x > y ? x - y : y - x ) <= t; };
		return close( r, rr, tolerance ) && close( g, gg, tolerance ) && close( b, bb, tolerance );
	}

	std::string Str() const
	{
		char buf[ 64 ];
		std::snprintf( buf, sizeof( buf ), "(%d,%d,%d,%d)", r, g, b, a );
		return buf;
	}
};

/// Picture coordinates: y = 0 is the top, whatever the buffer is doing.
inline Rgb Sample( const uint8_t* rgba, unsigned width, unsigned height, unsigned x, unsigned y )
{
	if( rgba == nullptr || x >= width || y >= height )
		return {};

	const unsigned row = height - 1 - y;
	const uint8_t* p   = rgba + ( size_t( row ) * width + x ) * 4;
	return { p[ 0 ], p[ 1 ], p[ 2 ], p[ 3 ] };
}

namespace detail
{
inline void PushBE32( std::vector< uint8_t >& v, uint32_t n )
{
	v.push_back( uint8_t( n >> 24 ) );
	v.push_back( uint8_t( n >> 16 ) );
	v.push_back( uint8_t( n >> 8 ) );
	v.push_back( uint8_t( n ) );
}

inline void PushChunk( std::vector< uint8_t >& out, const char* type,
					   const std::vector< uint8_t >& data )
{
	PushBE32( out, uint32_t( data.size() ) );

	std::vector< uint8_t > body( type, type + 4 );
	body.insert( body.end(), data.begin(), data.end() );

	out.insert( out.end(), body.begin(), body.end() );
	PushBE32( out, uint32_t( crc32( 0, body.data(), uInt( body.size() ) ) ) );
}
} // namespace detail

/// `rgba` is bottom-up and tightly packed at `width`.
inline bool WritePng( const std::string& path, const uint8_t* rgba, unsigned width, unsigned height )
{
	if( rgba == nullptr || width == 0 || height == 0 )
		return false;

	std::vector< uint8_t > raw;
	raw.reserve( size_t( height ) * ( size_t( width ) * 4 + 1 ) );
	for( unsigned y = 0; y < height; ++y )
	{
		raw.push_back( 0 ); // filter type: none
		const unsigned row = height - 1 - y;
		const uint8_t* p   = rgba + size_t( row ) * width * 4;
		raw.insert( raw.end(), p, p + size_t( width ) * 4 );
	}

	uLongf bound = compressBound( uLong( raw.size() ) );
	std::vector< uint8_t > deflated( bound );
	if( compress2( deflated.data(), &bound, raw.data(), uLong( raw.size() ), 6 ) != Z_OK )
		return false;
	deflated.resize( bound );

	std::vector< uint8_t > out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

	std::vector< uint8_t > ihdr;
	detail::PushBE32( ihdr, width );
	detail::PushBE32( ihdr, height );
	ihdr.push_back( 8 ); // bit depth
	ihdr.push_back( 6 ); // RGBA
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );

	detail::PushChunk( out, "IHDR", ihdr );
	detail::PushChunk( out, "IDAT", deflated );
	detail::PushChunk( out, "IEND", {} );

	FILE* fp = std::fopen( path.c_str(), "wb" );
	if( !fp )
		return false;
	const bool ok = std::fwrite( out.data(), 1, out.size(), fp ) == out.size();
	std::fclose( fp );
	return ok;
}

} // namespace cartridge::png
