#include "Pixels.h"

#include <cstring>

namespace cartridge::pixels
{

namespace
{
// 5- and 6-bit channels are expanded by replicating the high bits into the low
// ones, not by shifting left and leaving zeros. Shifting alone never reaches
// 255, so white comes out at 248 -- a dim, slightly warm picture that looks
// like a gamma problem and is really a missing three bits.
inline uint8_t Expand5( unsigned v )
{
	return uint8_t( ( v << 3 ) | ( v >> 2 ) );
}
inline uint8_t Expand6( unsigned v )
{
	return uint8_t( ( v << 2 ) | ( v >> 4 ) );
}

template< typename Row >
void ForEachRow( uint8_t* dst,
				 const void* src,
				 unsigned width,
				 unsigned height,
				 size_t pitchBytes,
				 bool flipVertical,
				 Row row )
{
	const uint8_t* base = static_cast< const uint8_t* >( src );

	for( unsigned y = 0; y < height; ++y )
	{
		const unsigned sy    = flipVertical ? ( height - 1 - y ) : y;
		const uint8_t* sline = base + size_t( sy ) * pitchBytes;
		uint8_t* dline       = dst + size_t( y ) * size_t( width ) * 4;
		row( dline, sline, width );
	}
}
} // namespace

void Convert( uint8_t* dst,
			  const void* src,
			  unsigned width,
			  unsigned height,
			  size_t pitchBytes,
			  Format format,
			  bool flipVertical )
{
	if( dst == nullptr || src == nullptr || width == 0 || height == 0 )
		return;

	switch( format )
	{
	case Format::XRGB8888:
		ForEachRow( dst, src, width, height, pitchBytes, flipVertical,
					[]( uint8_t* d, const uint8_t* s, unsigned w ) {
						const uint32_t* p = reinterpret_cast< const uint32_t* >( s );
						for( unsigned x = 0; x < w; ++x )
						{
							const uint32_t v = p[ x ];
							d[ x * 4 + 0 ]   = uint8_t( ( v >> 16 ) & 0xFF ); // R
							d[ x * 4 + 1 ]   = uint8_t( ( v >> 8 ) & 0xFF );  // G
							d[ x * 4 + 2 ]   = uint8_t( v & 0xFF );           // B
							d[ x * 4 + 3 ]   = 0xFF;
						}
					} );
		break;

	case Format::RGB565:
		ForEachRow( dst, src, width, height, pitchBytes, flipVertical,
					[]( uint8_t* d, const uint8_t* s, unsigned w ) {
						const uint16_t* p = reinterpret_cast< const uint16_t* >( s );
						for( unsigned x = 0; x < w; ++x )
						{
							const unsigned v = p[ x ];
							d[ x * 4 + 0 ]   = Expand5( ( v >> 11 ) & 0x1F );
							d[ x * 4 + 1 ]   = Expand6( ( v >> 5 ) & 0x3F );
							d[ x * 4 + 2 ]   = Expand5( v & 0x1F );
							d[ x * 4 + 3 ]   = 0xFF;
						}
					} );
		break;

	case Format::ORGB1555:
		ForEachRow( dst, src, width, height, pitchBytes, flipVertical,
					[]( uint8_t* d, const uint8_t* s, unsigned w ) {
						const uint16_t* p = reinterpret_cast< const uint16_t* >( s );
						for( unsigned x = 0; x < w; ++x )
						{
							const unsigned v = p[ x ];
							d[ x * 4 + 0 ]   = Expand5( ( v >> 10 ) & 0x1F );
							d[ x * 4 + 1 ]   = Expand5( ( v >> 5 ) & 0x1F );
							d[ x * 4 + 2 ]   = Expand5( v & 0x1F );
							d[ x * 4 + 3 ]   = 0xFF;
						}
					} );
		break;

	case Format::Unknown:
	default:
		// Black rather than garbage. A core that never called
		// SET_PIXEL_FORMAT is spec-wise 0RGB1555, and that case is handled
		// above by defaulting the field -- reaching here means the core asked
		// for something this build does not implement (XRGB2101010, HDR10),
		// and inventing an interpretation would put noise on a projector.
		std::memset( dst, 0, size_t( width ) * size_t( height ) * 4 );
		break;
	}
}

} // namespace cartridge::pixels
