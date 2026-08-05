/*
	cartest -- the offline harness.

	The fleet pattern: drive the real classes through the real sequence,
	headless, and assert on what actually came out. No Resolume, no GL context,
	no ROM, no network. `tools/verify.sh` runs the lot.

	It defaults to the synthetic core in `tools/testcore`, which is why the
	checks can assert exact pixel coordinates and exact sample values rather
	than eyeballing a picture. Point it at a real core with `--core` to smoke
	one out.

	---

	## The coordinate trap this harness has to get right

	Three different ideas of "row 0" meet here, and getting them confused
	produces a picture that is upside down in exactly one of the three places
	you look at it:

	- **libretro software framebuffers are top-left origin.** Row 0 is the top
	  of the picture.
	- **GL textures are bottom-left origin**, and so are FFGL's. A buffer
	  uploaded row 0 first puts row 0 at the *bottom*. So `pixels::Convert`
	  flips, and the RGBA buffer it produces has row 0 = bottom of picture.
	  That is the buffer Resolume wants and it is what `Frame::pixels` holds.
	- **PNG is top-left origin.** So writing `Frame::pixels` straight out gives
	  an upside-down file.

	`SampleDisplayed` and `WritePng` both compensate, in one place each, and
	every check is written in picture coordinates with y = 0 at the top. Nothing
	else in this file thinks about the flip.
*/

#include "Core.h"
#include "Diag.h"
#include "Runner.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cartridge;

namespace
{

int g_failures = 0;

void Fail( const std::string& what )
{
	std::fprintf( stderr, "FAIL  %s\n", what.c_str() );
	++g_failures;
}

void Pass( const std::string& what )
{
	std::fprintf( stdout, "ok    %s\n", what.c_str() );
}

// ---------------------------------------------------------------------------
// Picture-space sampling. y = 0 is the top of the picture, whatever the buffer
// is doing underneath. See the header comment.
// ---------------------------------------------------------------------------
struct Rgb
{
	int r = 0, g = 0, b = 0, a = 0;

	bool Near( int rr, int gg, int bb, int tolerance = 8 ) const
	{
		return std::abs( r - rr ) <= tolerance && std::abs( g - gg ) <= tolerance
			   && std::abs( b - bb ) <= tolerance;
	}

	std::string Str() const
	{
		char buf[ 64 ];
		std::snprintf( buf, sizeof( buf ), "(%d,%d,%d,%d)", r, g, b, a );
		return buf;
	}
};

Rgb SampleDisplayed( const Frame& f, unsigned x, unsigned y )
{
	if( x >= f.width || y >= f.height )
		return {};

	const unsigned row = f.height - 1 - y; // buffer is bottom-up
	const uint8_t* p   = f.pixels.data() + ( size_t( row ) * f.width + x ) * 4;
	return { p[ 0 ], p[ 1 ], p[ 2 ], p[ 3 ] };
}

// ---------------------------------------------------------------------------
// A minimal PNG writer. zlib ships with the OS, which is why this is fifty
// lines here rather than a vendored dependency -- same call as orrery's.
// ---------------------------------------------------------------------------
void PushBE32( std::vector< uint8_t >& v, uint32_t n )
{
	v.push_back( uint8_t( n >> 24 ) );
	v.push_back( uint8_t( n >> 16 ) );
	v.push_back( uint8_t( n >> 8 ) );
	v.push_back( uint8_t( n ) );
}

void PushChunk( std::vector< uint8_t >& out, const char* type, const std::vector< uint8_t >& data )
{
	PushBE32( out, uint32_t( data.size() ) );

	std::vector< uint8_t > body( type, type + 4 );
	body.insert( body.end(), data.begin(), data.end() );

	out.insert( out.end(), body.begin(), body.end() );
	PushBE32( out, uint32_t( crc32( 0, body.data(), uInt( body.size() ) ) ) );
}

bool WritePng( const std::string& path, const Frame& f )
{
	if( f.width == 0 || f.height == 0 )
		return false;

	// Raw scanlines, top-down for PNG, each prefixed with filter type 0.
	std::vector< uint8_t > raw;
	raw.reserve( size_t( f.height ) * ( size_t( f.width ) * 4 + 1 ) );
	for( unsigned y = 0; y < f.height; ++y )
	{
		raw.push_back( 0 );
		const unsigned row = f.height - 1 - y; // un-flip on the way out
		const uint8_t* p   = f.pixels.data() + size_t( row ) * f.width * 4;
		raw.insert( raw.end(), p, p + size_t( f.width ) * 4 );
	}

	uLongf bound = compressBound( uLong( raw.size() ) );
	std::vector< uint8_t > deflated( bound );
	if( compress2( deflated.data(), &bound, raw.data(), uLong( raw.size() ), 6 ) != Z_OK )
		return false;
	deflated.resize( bound );

	std::vector< uint8_t > png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

	std::vector< uint8_t > ihdr;
	PushBE32( ihdr, f.width );
	PushBE32( ihdr, f.height );
	ihdr.push_back( 8 ); // bit depth
	ihdr.push_back( 6 ); // RGBA
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	PushChunk( png, "IHDR", ihdr );
	PushChunk( png, "IDAT", deflated );
	PushChunk( png, "IEND", {} );

	FILE* fp = std::fopen( path.c_str(), "wb" );
	if( !fp )
		return false;
	const bool ok = std::fwrite( png.data(), 1, png.size(), fp ) == png.size();
	std::fclose( fp );
	return ok;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

// Mirrors of the test core's own constants. Deliberately re-declared rather
// than shared through a header: if the core's layout changes, these must be
// updated by hand and the mismatch shows up as a failure, instead of both
// sides moving together and the test continuing to pass while testing nothing.
constexpr unsigned kTcWidth    = 320;
constexpr unsigned kTcHeight   = 224;
constexpr unsigned kTcBlock    = 16;
constexpr unsigned kTcBarWidth = 8;
constexpr unsigned kTcBarTop   = 32;

void CheckGeometry( const Frame& f )
{
	if( f.width == kTcWidth && f.height == kTcHeight )
		Pass( "geometry is 320x224" );
	else
		Fail( "geometry: expected 320x224, got " + std::to_string( f.width ) + "x"
			  + std::to_string( f.height ) );

	// 4:3 declared against a 320x224 raster -- i.e. NOT square pixels. A
	// frontend that quietly substitutes width/height would report 1.4286.
	if( f.aspectRatio > 1.32f && f.aspectRatio < 1.35f )
		Pass( "aspect ratio is the core's 4:3, not the raster's" );
	else
		Fail( "aspect ratio: expected ~1.3333, got " + std::to_string( f.aspectRatio ) );
}

void CheckCorners( const Frame& f )
{
	// Sampled inside each block rather than at the exact corner, so a one-pixel
	// edge effect is not what decides the result.
	const struct
	{
		const char* name;
		unsigned x, y;
		int r, g, b;
	} corners[] = {
		{ "top-left red", 4, 4, 255, 0, 0 },
		{ "top-right green", kTcWidth - 5, 4, 0, 255, 0 },
		{ "bottom-left blue", 4, kTcHeight - 5, 0, 0, 255 },
		{ "bottom-right white", kTcWidth - 5, kTcHeight - 5, 255, 255, 255 },
	};

	for( const auto& c : corners )
	{
		const Rgb got = SampleDisplayed( f, c.x, c.y );
		if( got.Near( c.r, c.g, c.b ) )
			Pass( std::string( "corner " ) + c.name );
		else
			Fail( std::string( "corner " ) + c.name + ": got " + got.Str() );
	}

	// Alpha must be opaque everywhere. The X in XRGB8888 is undefined, and a
	// frontend that copies it through gives Resolume a transparent layer.
	const Rgb a = SampleDisplayed( f, 4, 4 );
	if( a.a == 255 )
		Pass( "alpha is forced opaque" );
	else
		Fail( "alpha: expected 255, got " + std::to_string( a.a ) );
}

void CheckBarPosition( const Frame& f, unsigned framesRun )
{
	// The core draws the bar for frame N at x = (N * 8) % (320 - 8), and the
	// frame the harness holds after running N frames is frame N-1.
	const unsigned expected = ( ( framesRun - 1 ) * kTcBarWidth ) % ( kTcWidth - kTcBarWidth );

	const unsigned y   = kTcBarTop + 4;
	const Rgb inBar    = SampleDisplayed( f, expected + kTcBarWidth / 2, y );
	const bool present = inBar.Near( 255, 255, 0 );

	if( present )
		Pass( "frame counter bar at x=" + std::to_string( expected ) );
	else
		Fail( "frame counter bar missing at x=" + std::to_string( expected ) + ", got "
			  + inBar.Str() );

	// And nothing where the *previous* frame's bar was, which is what catches
	// a stale slot coming back out of the triple buffer.
	if( framesRun >= 2 )
	{
		const unsigned prev = ( ( framesRun - 2 ) * kTcBarWidth ) % ( kTcWidth - kTcBarWidth );
		if( prev != expected )
		{
			const Rgb old = SampleDisplayed( f, prev + kTcBarWidth / 2, y );
			if( !old.Near( 255, 255, 0 ) )
				Pass( "previous frame's bar is gone (no stale slot)" );
			else
				Fail( "stale frame: bar still present at the previous position x="
					  + std::to_string( prev ) );
		}
	}
}

void CheckInput( Runner& runner )
{
	Core& core = runner.GetCore();

	// Nothing held: the centre block must be absent. Asserting the negative
	// first means a frontend that draws the block unconditionally fails here
	// rather than passing the positive case by accident.
	core.Input().Clear();
	runner.StepSynchronous( 2 );
	core.Frames().Acquire();
	{
		const Rgb centre = SampleDisplayed( core.Frames().Current(), kTcWidth / 2, kTcHeight / 2 );
		if( !centre.Near( 0, 255, 255 ) )
			Pass( "input: centre block absent with no buttons held" );
		else
			Fail( "input: centre block present with no buttons held" );
	}

	// Port 0, button A.
	core.Input().SetButton( 0, 8 /* RETRO_DEVICE_ID_JOYPAD_A */, true );
	runner.StepSynchronous( 2 );
	core.Frames().Acquire();
	{
		const Rgb centre = SampleDisplayed( core.Frames().Current(), kTcWidth / 2, kTcHeight / 2 );
		if( centre.Near( 0, 255, 255 ) )
			Pass( "input: port 0 button A reaches the core" );
		else
			Fail( "input: port 0 button A did not reach the core, got " + centre.Str() );
	}

	// Port 1, button B -- catches a frontend answering every port from port 0.
	core.Input().Clear();
	core.Input().SetButton( 1, 0 /* RETRO_DEVICE_ID_JOYPAD_B */, true );
	runner.StepSynchronous( 2 );
	core.Frames().Acquire();
	{
		const Rgb lower = SampleDisplayed( core.Frames().Current(),
										   kTcWidth / 2, kTcHeight / 2 + kTcBlock + 4 );
		if( lower.Near( 255, 0, 255 ) )
			Pass( "input: port 1 is a separate port" );
		else
			Fail( "input: port 1 button B did not reach the core, got " + lower.Str() );
	}

	core.Input().Clear();
}

void CheckAudio( Core& core )
{
	// 48000 / 60 = 800 frames per emulated frame. After the frames already run
	// there must be something in the ring, and it must be the square wave.
	std::vector< AudioRing::Sample > buf( 4096 );
	const size_t got = core.Audio().Read( buf.data(), buf.size() );

	if( got == 0 )
	{
		Fail( "audio: ring is empty" );
		return;
	}
	Pass( "audio: " + std::to_string( got ) + " frames buffered" );

	bool onlySquare = true;
	bool sawBoth    = false;
	bool sawHigh = false, sawLow = false;
	for( size_t i = 0; i < got; ++i )
	{
		const int16_t v = buf[ i ].left;
		if( v == 8000 )
			sawHigh = true;
		else if( v == -8000 )
			sawLow = true;
		else
			onlySquare = false;

		if( buf[ i ].left != buf[ i ].right )
			sawBoth = true;
	}
	sawBoth = !sawBoth; // both channels should match

	if( onlySquare && sawHigh && sawLow )
		Pass( "audio: square wave arrived intact at both levels" );
	else
		Fail( "audio: samples are not the expected square wave" );

	if( sawBoth )
		Pass( "audio: channels are interleaved correctly" );
	else
		Fail( "audio: left and right differ" );

	if( core.Audio().Dropped() == 0 )
		Pass( "audio: no overruns" );
	else
		Fail( "audio: " + std::to_string( core.Audio().Dropped() ) + " frames dropped" );
}

void CheckDeterminism( const std::string& corePath )
{
	// The same number of frames from a cold start must give byte-identical
	// pixels. This is what makes every other check above trustworthy, and it is
	// what would catch uninitialised padding leaking into the picture.
	auto renderOnce = []( const std::string& path, std::vector< uint8_t >& out ) -> bool {
		Runner r;
		std::string err;
		if( !r.GetCore().Load( path, err, /*uniqueInstance*/ true ) )
		{
			Fail( "determinism: core load failed: " + err );
			return false;
		}
		if( !r.GetCore().LoadContent( "", err ) )
		{
			Fail( "determinism: content load failed: " + err );
			return false;
		}
		r.StepSynchronous( 17 );
		r.GetCore().Frames().Acquire();
		out = r.GetCore().Frames().Current().pixels;
		return true;
	};

	std::vector< uint8_t > a, b;
	if( !renderOnce( corePath, a ) || !renderOnce( corePath, b ) )
		return;

	if( a.size() == b.size() && !a.empty() && std::memcmp( a.data(), b.data(), a.size() ) == 0 )
		Pass( "determinism: two cold runs of 17 frames are byte-identical" );
	else
		Fail( "determinism: two cold runs differ" );
}

void CheckTwoInstances( const std::string& corePath )
{
	// Two Cores on the same library, each with its own private copy. Advance
	// them by different amounts and the bar must land in different places. If
	// the private-copy trick is not working they share globals and both show
	// the same frame -- see the trap in Core.h.
	Runner a, b;
	std::string err;

	if( !a.GetCore().Load( corePath, err, true ) || !a.GetCore().LoadContent( "", err ) )
	{
		Fail( "two instances: first core failed: " + err );
		return;
	}
	if( !b.GetCore().Load( corePath, err, true ) || !b.GetCore().LoadContent( "", err ) )
	{
		Fail( "two instances: second core failed: " + err );
		return;
	}

	a.StepSynchronous( 3 );
	b.StepSynchronous( 9 );

	a.GetCore().Frames().Acquire();
	b.GetCore().Frames().Acquire();

	const unsigned ax = ( 2 * kTcBarWidth ) % ( kTcWidth - kTcBarWidth );
	const unsigned bx = ( 8 * kTcBarWidth ) % ( kTcWidth - kTcBarWidth );

	const bool aOk = SampleDisplayed( a.GetCore().Frames().Current(),
									  ax + kTcBarWidth / 2, kTcBarTop + 4 )
						 .Near( 255, 255, 0 );
	const bool bOk = SampleDisplayed( b.GetCore().Frames().Current(),
									  bx + kTcBarWidth / 2, kTcBarTop + 4 )
						 .Near( 255, 255, 0 );

	if( aOk && bOk )
		Pass( "two instances of one core keep separate state" );
	else
		Fail( "two instances share state: instance A "
			  + std::string( aOk ? "ok" : "wrong" ) + ", instance B "
			  + std::string( bOk ? "ok" : "wrong" ) );
}

void Usage()
{
	std::printf(
		"cartest -- offline harness for cartridge\n\n"
		"  --core PATH      libretro core to load (default: the built test core)\n"
		"  --content PATH   content to load (default: none)\n"
		"  --frames N       frames to run before checking or writing (default 8)\n"
		"  --out PATH       write the resulting frame as a PNG\n"
		"  --press ID       hold a joypad button id on port 0 while running\n"
		"  --check          run the full assertion suite against the test core\n"
		"  --info           print what the core says about itself\n"
		"  --list           list the checks and exit\n" );
}

} // namespace

int main( int argc, char** argv )
{
	diag::setConsoleEcho( true );

	std::string corePath;
	std::string contentPath;
	std::string outPath;
	unsigned frames = 8;
	int press       = -1;
	bool doCheck    = false;
	bool doInfo     = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		auto next           = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : ""; };

		if( a == "--core" )
			corePath = next();
		else if( a == "--content" )
			contentPath = next();
		else if( a == "--out" )
			outPath = next();
		else if( a == "--frames" )
			frames = unsigned( std::stoul( next() ) );
		else if( a == "--press" )
			press = std::stoi( next() );
		else if( a == "--check" )
			doCheck = true;
		else if( a == "--info" )
			doInfo = true;
		else if( a == "--list" )
		{
			std::printf( "geometry, corners, alpha, bar position, stale slot, input ports,\n"
						 "audio ring, determinism, two instances\n" );
			return 0;
		}
		else
		{
			Usage();
			return a == "--help" || a == "-h" ? 0 : 2;
		}
	}

	if( corePath.empty() )
	{
		// Sitting next to the executable, which is where CMake puts it.
		const std::string exe = argv[ 0 ];
		const size_t slash    = exe.find_last_of( '/' );
		const std::string dir = slash == std::string::npos ? std::string( "." ) : exe.substr( 0, slash );
#if defined( __APPLE__ )
		corePath = dir + "/cartridge_testcore.dylib";
#elif defined( _WIN32 )
		corePath = dir + "/cartridge_testcore.dll";
#else
		corePath = dir + "/cartridge_testcore.so";
#endif
	}

	Runner runner;
	std::string error;

	if( !runner.GetCore().Load( corePath, error, /*uniqueInstance*/ true ) )
	{
		std::fprintf( stderr, "could not load core: %s\n", error.c_str() );
		return 1;
	}

	if( doInfo )
	{
		const auto& info = runner.GetCore().CoreInfo();
		std::printf( "name        %s\n", info.libraryName.c_str() );
		std::printf( "version     %s\n", info.libraryVersion.c_str() );
		std::printf( "extensions  %s\n", info.validExtensions.c_str() );
		std::printf( "fullpath    %s\n", info.needFullpath ? "yes" : "no" );
		std::printf( "no-content  %s\n", info.supportsNoGame ? "yes" : "no" );
	}

	if( !runner.GetCore().LoadContent( contentPath, error ) )
	{
		std::fprintf( stderr, "could not load content: %s\n", error.c_str() );
		return 1;
	}

	if( doInfo )
	{
		std::printf( "fps         %.4f\n", runner.GetCore().Fps() );
		std::printf( "samplerate  %.0f\n", runner.GetCore().SampleRate() );
	}

	if( press >= 0 )
		runner.GetCore().Input().SetButton( 0, unsigned( press ), true );

	runner.StepSynchronous( frames );
	runner.GetCore().Frames().Acquire();
	const Frame& frame = runner.GetCore().Frames().Current();

	if( frame.width == 0 )
	{
		std::fprintf( stderr, "the core produced no frames\n" );
		return 1;
	}

	if( !outPath.empty() )
	{
		if( WritePng( outPath, frame ) )
			std::printf( "wrote %s (%ux%u)\n", outPath.c_str(), frame.width, frame.height );
		else
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
	}

	if( doCheck )
	{
		CheckGeometry( frame );
		CheckCorners( frame );
		CheckBarPosition( frame, frames );
		CheckAudio( runner.GetCore() );
		CheckInput( runner );

		// These two build their own Cores, so they come after everything that
		// uses the one above.
		CheckDeterminism( corePath );
		CheckTwoInstances( corePath );

		std::printf( "\n%s\n", g_failures == 0 ? "all checks passed"
											   : ( std::to_string( g_failures ) + " check(s) failed" ).c_str() );
		return g_failures == 0 ? 0 : 1;
	}

	return 0;
}
