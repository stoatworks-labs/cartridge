#pragma once

#include <cstddef>
#include <cstdint>

/**
	Converting what a libretro core hands us into what the GPU wants.

	Cores emit one of three software formats and choose which at load time via
	`RETRO_ENVIRONMENT_SET_PIXEL_FORMAT`. All three are converted here to
	straight RGBA8 with opaque alpha.

	**The pitch is not the width.** `retro_video_refresh_t` carries a `pitch` in
	*bytes*, and cores routinely hand over a buffer whose rows are wider than the
	picture -- an emulator's internal framebuffer is usually allocated at the
	console's maximum geometry and only partly filled. Walking it as
	`width * bytesPerPixel` gives a picture that shears progressively to one
	side: recognisable, obviously wrong, and a full afternoon if you assume the
	core is at fault.

	**Alpha is forced to 255, it is not copied.** The X in XRGB8888 is
	*undefined*, not zero -- several cores leave stale bits there. Passing it
	through gives a texture that is mostly transparent in Resolume, which reads
	as "the plugin does nothing" against a black composition.
*/
namespace cartridge::pixels
{

enum class Format
{
	XRGB8888,  ///< 32bpp, native endian, high byte ignored
	RGB565,    ///< 16bpp
	ORGB1555,  ///< 16bpp, the libretro default if a core never asks
	Unknown
};

/// Bytes one source pixel occupies. Used to sanity-check a core's pitch.
inline unsigned BytesPerPixel( Format f )
{
	return f == Format::XRGB8888 ? 4 : 2;
}

/**
	Convert one frame into `dst`, which must hold `width * height * 4` bytes.

	`pitchBytes` is the source stride straight from the core -- see above.
	`flipVertical` handles the origin mismatch: libretro's software framebuffers
	are top-left origin, and a GL texture uploaded row 0 first ends up upside
	down unless either this or the texture coordinates compensate. Doing it here
	keeps the shader honest and costs nothing, since the rows are being copied
	regardless.
*/
void Convert( uint8_t* dst,
			  const void* src,
			  unsigned width,
			  unsigned height,
			  size_t pitchBytes,
			  Format format,
			  bool flipVertical );

} // namespace cartridge::pixels
