/*
	A synthetic libretro core, so this repo can be verified without shipping or
	downloading an emulator or a ROM.

	It is not a toy. Every part of the picture it draws exists to pin down one
	thing the frontend can get wrong, and each of those has a failure mode that
	looks like "the plugin is black" or "the plugin works" from the outside:

	- **Four corner blocks in pure primaries.** Catches a channel swap (R and B
	  transposed is invisible on greyscale test content and obvious here) and
	  catches the vertical flip landing the wrong way up.
	- **A vertical bar whose x position is the frame counter.** Catches the core
	  not advancing, the frontend running it twice per displayed frame, and the
	  triple buffer handing back a stale slot. Position is exact, so the harness
	  asserts an integer rather than eyeballing motion.
	- **A centre block that appears only while a button is held.** Catches the
	  whole input path -- atomic pad word, poll routing, port and id mapping.
	- **A padded pitch.** THE POINT. `pitch` is deliberately the *maximum* width,
	  not the current one, exactly as a real emulator allocates. A frontend that
	  walks the buffer as `width * bpp` gets a picture that shears further to
	  one side on every row. Every real core does this and nothing else in the
	  test suite would catch it.
	- **A square wave.** Catches the audio ring, including the batch callback.

	Build: a MODULE library exporting the libretro ABI, loaded by dlopen exactly
	as a real core is. It goes through the same `Core::Load` path with the same
	symbol binding and the same ABI check.

	`CARTRIDGE_TESTCORE_FORMAT=565` switches the pixel format, so the RGB565
	conversion is exercised by the same tests rather than by a separate unit
	test that agrees with its own assumptions.
*/

#include "libretro.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TC_WIDTH      320
#define TC_HEIGHT     224
#define TC_MAX_WIDTH  640
#define TC_MAX_HEIGHT 480
#define TC_FPS        60.0
#define TC_SAMPLERATE 48000.0

#define TC_BLOCK      16
#define TC_BAR_WIDTH   8
#define TC_BAR_TOP    32
#define TC_BAR_BOTTOM 64

static retro_environment_t        env_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;

/* Allocated at MAX geometry and only partly used -- see the pitch note above. */
static uint32_t frame_rgba[ TC_MAX_WIDTH * TC_MAX_HEIGHT ];
static uint16_t frame_16[ TC_MAX_WIDTH * TC_MAX_HEIGHT ];

static unsigned frame_count;
static int      use_565;
static unsigned phase;

unsigned retro_api_version( void ) { return RETRO_API_VERSION; }

void retro_get_system_info( struct retro_system_info* info )
{
	memset( info, 0, sizeof( *info ) );
	info->library_name     = "cartridge test pattern";
	info->library_version   = "1.0";
	info->valid_extensions  = "";
	info->need_fullpath     = false;
	info->block_extract     = false;
}

void retro_get_system_av_info( struct retro_system_av_info* info )
{
	memset( info, 0, sizeof( *info ) );
	info->geometry.base_width   = TC_WIDTH;
	info->geometry.base_height  = TC_HEIGHT;
	info->geometry.max_width    = TC_MAX_WIDTH;
	info->geometry.max_height   = TC_MAX_HEIGHT;
	/* Deliberately not width/height: 320x224 on a Mega Drive is a 4:3 picture
	   with non-square pixels, and a frontend that assumes square is wrong on
	   almost every real console. */
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps            = TC_FPS;
	info->timing.sample_rate    = TC_SAMPLERATE;
}

void retro_set_environment( retro_environment_t cb )
{
	bool no_game = true;
	env_cb = cb;

	/* Must be set here rather than in retro_init: the frontend reads it before
	   deciding whether it may call load_game(NULL). */
	cb( RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game );
}

void retro_set_video_refresh( retro_video_refresh_t cb )       { video_cb = cb; }
void retro_set_audio_sample( retro_audio_sample_t cb )         { audio_cb = cb; }
void retro_set_audio_sample_batch( retro_audio_sample_batch_t cb ) { audio_batch_cb = cb; }
void retro_set_input_poll( retro_input_poll_t cb )             { input_poll_cb = cb; }
void retro_set_input_state( retro_input_state_t cb )           { input_state_cb = cb; }
void retro_set_controller_port_device( unsigned port, unsigned device ) { (void)port; (void)device; }

void retro_init( void )
{
	const char* fmt = getenv( "CARTRIDGE_TESTCORE_FORMAT" );
	use_565 = ( fmt && strcmp( fmt, "565" ) == 0 );
	frame_count = 0;
	phase = 0;
}

void retro_deinit( void ) { }

bool retro_load_game( const struct retro_game_info* game )
{
	enum retro_pixel_format fmt =
		use_565 ? RETRO_PIXEL_FORMAT_RGB565 : RETRO_PIXEL_FORMAT_XRGB8888;

	(void)game; /* no content: this core declared SET_SUPPORT_NO_GAME */

	if( !env_cb( RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt ) )
		return false;

	frame_count = 0;
	return true;
}

void retro_unload_game( void ) { }

/* Set for the duration of retro_run, so retro_reset can notice it was called
   while a frame was in flight. A real core would not report this — it would
   corrupt its own state and crash somewhere unrelated, which is exactly what
   made the in-process Reset race hard to see. `tc_get_overlap()` is how
   cartest asks. */
static volatile int in_run  = 0;
static volatile int overlap = 0;

/* Exported for the test harness, not part of the libretro API. */
static volatile int resets = 0;

int tc_get_overlap( void );
int tc_get_overlap( void ) { return overlap; }
int tc_get_resets( void );
int tc_get_resets( void ) { return resets; }

void retro_reset( void )
{
	if( in_run )
		overlap = 1;

	resets++;
	frame_count = 0;
	phase = 0;
}

/* ------------------------------------------------------------------------- */

static void put_pixel( unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b )
{
	/* Row stride is the MAX width, not the current width. This is the padded
	   pitch the frontend has to honour. */
	const size_t i = (size_t)y * TC_MAX_WIDTH + x;

	if( use_565 )
		frame_16[ i ] = (uint16_t)( ( ( r >> 3 ) << 11 ) | ( ( g >> 2 ) << 5 ) | ( b >> 3 ) );
	else
		frame_rgba[ i ] = ( (uint32_t)r << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)b;
}

static void fill_block( unsigned x0, unsigned y0, unsigned w, unsigned h,
                        uint8_t r, uint8_t g, uint8_t b )
{
	unsigned x, y;
	for( y = y0; y < y0 + h && y < TC_HEIGHT; ++y )
		for( x = x0; x < x0 + w && x < TC_WIDTH; ++x )
			put_pixel( x, y, r, g, b );
}

void retro_run( void )
{
	unsigned bar_x;
	int16_t  samples[ 2048 ];
	unsigned n_samples = (unsigned)( TC_SAMPLERATE / TC_FPS ); /* 800 */
	unsigned i;
	int16_t  a_pressed;

	in_run = 1;

	input_poll_cb();

	/* --- picture ---------------------------------------------------------- */

	/* Clear only the used region; the padding stays whatever it was, which is
	   the honest thing to do -- a frontend reading past `width` should see
	   stale data, because that is what it will see from a real core. */
	{
		unsigned y;
		for( y = 0; y < TC_HEIGHT; ++y )
		{
			unsigned x;
			for( x = 0; x < TC_WIDTH; ++x )
				put_pixel( x, y, 0, 0, 0 );
		}
	}

	fill_block( 0, 0, TC_BLOCK, TC_BLOCK, 255, 0, 0 );                                  /* TL red    */
	fill_block( TC_WIDTH - TC_BLOCK, 0, TC_BLOCK, TC_BLOCK, 0, 255, 0 );                /* TR green  */
	fill_block( 0, TC_HEIGHT - TC_BLOCK, TC_BLOCK, TC_BLOCK, 0, 0, 255 );               /* BL blue   */
	fill_block( TC_WIDTH - TC_BLOCK, TC_HEIGHT - TC_BLOCK,
	            TC_BLOCK, TC_BLOCK, 255, 255, 255 );                                    /* BR white  */

	/* The frame counter, drawn. Modulo keeps it on screen; the harness applies
	   the same modulo, so a frontend that runs the core twice per displayed
	   frame lands the bar somewhere the harness is not looking. */
	bar_x = ( frame_count * TC_BAR_WIDTH ) % ( TC_WIDTH - TC_BAR_WIDTH );
	fill_block( bar_x, TC_BAR_TOP, TC_BAR_WIDTH, TC_BAR_BOTTOM - TC_BAR_TOP,
	            255, 255, 0 );

	/* Input, drawn. Port 0, button A. */
	a_pressed = input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A );
	if( a_pressed )
		fill_block( TC_WIDTH / 2 - TC_BLOCK / 2, TC_HEIGHT / 2 - TC_BLOCK / 2,
		            TC_BLOCK, TC_BLOCK, 0, 255, 255 );

	/* Port 1, button B -- so a frontend that answers every port from port 0
	   fails rather than passing by coincidence. */
	if( input_state_cb( 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B ) )
		fill_block( TC_WIDTH / 2 - TC_BLOCK / 2, TC_HEIGHT / 2 + TC_BLOCK,
		            TC_BLOCK, TC_BLOCK, 255, 0, 255 );

	if( use_565 )
		video_cb( frame_16, TC_WIDTH, TC_HEIGHT, TC_MAX_WIDTH * sizeof( uint16_t ) );
	else
		video_cb( frame_rgba, TC_WIDTH, TC_HEIGHT, TC_MAX_WIDTH * sizeof( uint32_t ) );

	/* --- audio ------------------------------------------------------------ */

	/* 1 kHz square at a quiet level. Square rather than sine so the harness can
	   assert exact sample values without worrying about rounding. */
	for( i = 0; i < n_samples; ++i )
	{
		const int16_t v = ( ( phase / 24 ) & 1 ) ? 8000 : -8000;
		samples[ i * 2 + 0 ] = v;
		samples[ i * 2 + 1 ] = v;
		phase++;
	}
	audio_batch_cb( samples, n_samples );

	frame_count++;

	in_run = 0;
}

/* ------------------------------------------------------------------------- */
/* Required by the ABI. This core has no state to serialise and no memory to
   expose, but a frontend is entitled to call these and a missing symbol makes
   RTLD_NOW fail the whole load. */

size_t retro_serialize_size( void ) { return 0; }
bool   retro_serialize( void* d, size_t s ) { (void)d; (void)s; return false; }
bool   retro_unserialize( const void* d, size_t s ) { (void)d; (void)s; return false; }
void   retro_cheat_reset( void ) { }
void   retro_cheat_set( unsigned i, bool e, const char* c ) { (void)i; (void)e; (void)c; }
bool   retro_load_game_special( unsigned t, const struct retro_game_info* i, size_t n )
	{ (void)t; (void)i; (void)n; return false; }
unsigned retro_get_region( void ) { return RETRO_REGION_NTSC; }
void*  retro_get_memory_data( unsigned id ) { (void)id; return NULL; }
size_t retro_get_memory_size( unsigned id ) { (void)id; return 0; }
