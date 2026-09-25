/*
	cargl -- the GL harness.

	Where the plugin is actually verified rather than merely compiled. It drives
	**the real `CartridgePlugin`** through the real FFGL sequence -- constructor,
	`InitGL`, `SetTextParameter`, `ProcessOpenGL`, `DeInitGL` -- in a headless
	CGL 4.1 core-profile context, and reads the result back as pixels.

	`cartest` proves the libretro host is correct on the CPU side. This proves
	the half that only exists on the GPU, and that half has failure modes the
	CPU tests structurally cannot see:

	- **A shader that will not compile.** `InitGL` returns FF_FAIL and the
	  plugin renders nothing. From inside Resolume this is indistinguishable
	  from a core that failed to load.
	- **A uniform name that does not match the GLSL.** `glGetUniformLocation`
	  returns -1 and `glUniform(-1)` is a documented no-op, so a control is
	  stone dead while everything compiles, links and renders. Nothing else in
	  this repo would catch it -- it is the trap that cost orrery a day.
	- **The vertical flip landing the wrong way round.** There are three
	  conflicting ideas of "row 0" between libretro, GL and PNG, and the picture
	  is upside down in exactly one place if any of them is wrong. Checking the
	  corner colours after a real GL round trip pins all three at once.
	- **The letterbox arithmetic.** A 4:3 picture in a 16:9 composition must
	  pillarbox, and the bars must be where the maths says.

	It cannot check what only a host can: how the parameter groups land in
	Resolume's inspector, and whether a real controller MIDI-maps onto the pad
	the way it should. Those are Allan's to confirm.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include "Diag.h"
#include "Plugin.h"
#include "common/Png.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

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

CGLContextObj MakeContext()
{
	// Accelerated first, software as a fallback, so this runs on a machine with
	// no display attached as well as on a laptop.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, CGLPixelFormatAttribute( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, CGLPixelFormatAttribute( 24 ),
		kCGLPFAAlphaSize, CGLPixelFormatAttribute( 8 ),
		CGLPixelFormatAttribute( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, CGLPixelFormatAttribute( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, CGLPixelFormatAttribute( 24 ),
		kCGLPFAAlphaSize, CGLPixelFormatAttribute( 8 ),
		CGLPixelFormatAttribute( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint n                  = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &n ) != kCGLNoError || format == nullptr )
		CGLChoosePixelFormat( software, &format, &n );

	if( format == nullptr )
		return nullptr;

	CGLContextObj context = nullptr;
	CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );

	if( context )
		CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint fbo     = 0;
	GLuint colour  = 0;
	unsigned width  = 0;
	unsigned height = 0;
};

Target MakeTarget( unsigned width, unsigned height )
{
	Target t;
	t.width  = width;
	t.height = height;

	glGenTextures( 1, &t.colour );
	glBindTexture( GL_TEXTURE_2D, t.colour );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei( width ), GLsizei( height ), 0,
				  GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

	glGenFramebuffers( 1, &t.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.colour, 0 );

	glBindTexture( GL_TEXTURE_2D, 0 );
	return t;
}

std::vector< uint8_t > ReadBack( const Target& t )
{
	std::vector< uint8_t > rgba( size_t( t.width ) * t.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, GLsizei( t.width ), GLsizei( t.height ), GL_RGBA, GL_UNSIGNED_BYTE,
				  rgba.data() );
	return rgba;
}

/// One `ProcessOpenGL` into a cleared target, then read it back.
std::vector< uint8_t > DrawOnce( CartridgePlugin& plugin, const Target& target )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, GLsizei( target.width ), GLsizei( target.height ) );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );

	ProcessOpenGLStruct gl = {};
	gl.numInputTextures    = 0;
	gl.HostFBO             = target.fbo;

	plugin.ProcessOpenGL( &gl );
	glFinish();

	return ReadBack( target );
}

/**
	Draw until `ready` is satisfied, or give up.

	**Every wait in this harness has to be on a condition, never on a frame
	count.** The emulator runs on its own thread at its own rate, so after any
	parameter change there is an unknown number of composition frames before the
	core has produced a frame that reflects it and `ProcessOpenGL` has uploaded
	it. That is not a defect -- it is exactly what happens in Resolume when you
	press a mapped key.

	Waiting a fixed number of iterations instead is what made the first version
	of the input check fail: it returned on the first non-black pixel, which is
	true from the very first frame because the corner blocks are always drawn.
	The test was measuring nothing.
*/
bool PumpUntil( CartridgePlugin& plugin, const Target& target,
				const std::function< bool( const std::vector< uint8_t >& ) >& ready,
				int maxFrames = 240 )
{
	for( int i = 0; i < maxFrames; ++i )
	{
		const auto rgba = DrawOnce( plugin, target );
		if( ready( rgba ) )
			return true;
		std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
	}
	return false;
}

/// Wait for any picture at all to arrive.
bool PumpUntilDrawn( CartridgePlugin& plugin, const Target& target )
{
	return PumpUntil( plugin, target, []( const std::vector< uint8_t >& rgba ) {
		// The four corner blocks are the only thing guaranteed present in every
		// frame the test core draws, so look for one rather than for "anything
		// non-black at the centre" -- the centre is black unless a button is
		// held.
		for( size_t i = 0; i + 3 < rgba.size(); i += 4 )
			if( rgba[ i ] || rgba[ i + 1 ] || rgba[ i + 2 ] )
				return true;
		return false;
	} );
}

std::string TestCorePath( const char* argv0 )
{
	const std::string exe = argv0;
	const size_t slash    = exe.find_last_of( '/' );
	const std::string dir = slash == std::string::npos ? std::string( "." ) : exe.substr( 0, slash );
	return dir + "/cartridge_testcore.dylib";
}

// Mirrors of the test core's layout, re-declared rather than shared -- see the
// same note in cartest.
constexpr unsigned kTcWidth  = 320;
constexpr unsigned kTcHeight = 224;

/**
	The corner check, through the whole GL path.

	This is the strongest assertion in the repo. Passing means the pixel
	conversion, the vertical flip, the texture allocation at max geometry, the
	UV scale addressing the used corner, the letterbox arithmetic, the shader,
	the uniforms and the readback all agree at once -- and it fails if any single
	one of them is wrong.
*/
/// Where the plugin should have put the picture, worked out independently of
/// the plugin. Mirrors the Fit branch of `ComputeQuadScale`.
struct FittedRect
{
	unsigned left, right, top, bottom; // picture coords, y down
	unsigned blockW, blockH;           // size of a corner block on screen
	bool pillarboxed;                  // bars on the sides rather than top/bottom
};

FittedRect FitRect( unsigned vw, unsigned vh )
{
	const float pictureAspect = 4.0f / 3.0f; // what the test core declares
	const float viewAspect    = float( vw ) / float( vh );

	float sx = 1.0f, sy = 1.0f;
	if( pictureAspect > viewAspect )
		sy = viewAspect / pictureAspect; // wider than the frame: bars top/bottom
	else
		sx = pictureAspect / viewAspect; // taller: bars left/right

	FittedRect r;
	r.left   = unsigned( ( 1.0f - sx ) * 0.5f * float( vw ) );
	r.right  = vw - r.left;
	r.top    = unsigned( ( 1.0f - sy ) * 0.5f * float( vh ) );
	r.bottom = vh - r.top;

	r.blockW      = unsigned( 16.0f / float( kTcWidth ) * float( r.right - r.left ) );
	r.blockH      = unsigned( 16.0f / float( kTcHeight ) * float( r.bottom - r.top ) );
	r.pillarboxed = r.left > 2;
	return r;
}

/**
	The corner check, through the whole GL path, at whatever aspect.

	**Deliberately not hardcoded to pillarboxing.** The first version assumed
	bars on the sides, which is true for 4:3 into 16:9 and false for 4:3 into
	1:1 -- where the picture letterboxes instead. It passed at one size and
	failed at another for reasons that had nothing to do with the plugin.

	That is exactly why the suite runs at two aspects: a sign error in the fit
	branch is invisible whenever the picture happens to be wider than the frame,
	and a square render is the cheapest way to make the other branch matter.
*/
void CheckFittedCorners( const std::vector< uint8_t >& rgba, unsigned vw, unsigned vh )
{
	const FittedRect r = FitRect( vw, vh );

	const struct
	{
		const char* name;
		unsigned x, y;
		int cr, cg, cb;
	} corners[] = {
		{ "top-left red", r.left + r.blockW / 2, r.top + r.blockH / 2, 255, 0, 0 },
		{ "top-right green", r.right - r.blockW / 2, r.top + r.blockH / 2, 0, 255, 0 },
		{ "bottom-left blue", r.left + r.blockW / 2, r.bottom - r.blockH / 2, 0, 0, 255 },
		{ "bottom-right white", r.right - r.blockW / 2, r.bottom - r.blockH / 2, 255, 255, 255 },
	};

	for( const auto& c : corners )
	{
		const auto got = png::Sample( rgba.data(), vw, vh, c.x, c.y );
		if( got.Near( c.cr, c.cg, c.cb ) )
			Pass( std::string( "GL corner " ) + c.name );
		else
			Fail( std::string( "GL corner " ) + c.name + " at (" + std::to_string( c.x ) + ","
				  + std::to_string( c.y ) + "): got " + got.Str() );
	}

	// The bars themselves. A stretched picture would put content here, so this
	// is what catches Fit silently behaving like Stretch.
	if( r.pillarboxed )
	{
		const auto bar = png::Sample( rgba.data(), vw, vh, r.left / 2, vh / 2 );
		if( bar.r == 0 && bar.g == 0 && bar.b == 0 )
			Pass( "GL fit: pillarbox bars are empty" );
		else
			Fail( "GL fit: expected black at x=" + std::to_string( r.left / 2 ) + ", got "
				  + bar.Str() );
	}
	else if( r.top > 2 )
	{
		const auto bar = png::Sample( rgba.data(), vw, vh, vw / 2, r.top / 2 );
		if( bar.r == 0 && bar.g == 0 && bar.b == 0 )
			Pass( "GL fit: letterbox bars are empty" );
		else
			Fail( "GL fit: expected black at y=" + std::to_string( r.top / 2 ) + ", got "
				  + bar.Str() );
	}
}

void Usage()
{
	std::printf(
		"cargl -- GL harness for the cartridge FFGL plugin\n\n"
		"  --core PATH     core to load (default: the built test core)\n"
		"  --content P     content to load\n"
		"  --helper NAME   attach to a running helper on this channel instead\n"
		"                  of loading a core in process\n"
		"  --size WxH      render size (default 1280x720)\n"
		"  --out PATH      write the rendered frame as a PNG\n"
		"  --check         run the assertion suite\n"
		"  --survive SECS  attach to a helper, keep drawing for SECS, and assert\n"
		"                  the picture survives the helper being killed\n"
		"  --pipe          raw RGBA frames to stdout, paced on the wall clock:\n"
		"                  the fleet's filming mode (see RunPipe)\n"
		"  --fps N         --pipe frame rate (default 30)\n"
		"  --frames N      --pipe length in frames; 0 runs until the reader hangs up\n"
		"  --script PATH   --pipe cue sheet: `frame  Parameter Name  value` per line\n"
		"  --set NAME=V    --pipe: a parameter's value before the first frame\n" );
}

/*
	--pipe: raw RGBA frames out, a cue sheet in.

	The fleet's filming format (copperlist's cptest, resodoom's resogl), so one
	render script drives any plugin: `frame  Parameter Name  value` per line,
	`#` a comment. Parameters are addressed by the NAME the inspector shows,
	which is how Resolume addresses them too. A numeric track is held before its
	first key and after its last and linear between, so a boolean or an option
	is stepped with two keys a frame apart. **Core, Content and Channel are text
	parameters**, and a text track is a path per key, applied the frame its key
	lands on -- `210  Core  /path/to/mgba_libretro.dylib` swaps the core on the
	running layer exactly as picking a file in the inspector would, and a value
	of `-` clears it (a no-content core loads with none). A value only
	reaches the plugin when it CHANGES: a re-sent path is what Resolume does on
	composition load, and the plugin already ignores it, but Reset is an event
	and would fire every frame.

	**The frames are paced on the wall clock.** A shader plugin is a function of
	the host's time, so its harness can hand it frame n at n/fps and render as
	fast as the encoder takes them. This plugin is not: the core runs on its own
	thread at the console's own rate (Runner.h), exactly as Resolume drives it,
	so frame n is drawn at t0 + n/fps of real time and a 60 s take takes 60 s.
	A reader slower than the frame rate holds the write and the console keeps
	running without it -- the take skips rather than stalls, which is what an
	overloaded Resolume would show and the reason to encode the pipe with a fast
	preset and re-encode afterwards.

	The frames get a private copy of stdout and fd 1 itself is pointed at
	stderr, so a core that printf()s -- several do, at load -- lands in the log
	rather than in the frame stream. Frames are written top-down (glReadPixels
	is bottom-up). The reader hanging up before --frames have been written is
	exit 1; with --frames 0 the take runs until then and that is exit 0. SIGPIPE
	is ignored so the plugin is shut down either way.
*/
using Track = std::vector< std::pair< int, float > >;
using TextTrack = std::vector< std::pair< int, std::string > >;

struct Script
{
	std::map< std::string, Track >     numeric;
	std::map< std::string, TextTrack > text;
};

bool IsTextParam( unsigned type )
{
	return type == FF_TYPE_TEXT || type == FF_TYPE_FILE;
}

Script LoadScript( const std::string& path, const std::map< std::string, unsigned >& types,
				   std::string& error )
{
	Script        script;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return script;
	}
	std::string line;
	int         lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int                frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string                word;
		while( in >> word )
			words.push_back( word );
		const std::string where =
			path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
		if( words.size() < 2 )
		{
			error = where;
			return {};
		}

		// The longest prefix of the words that names a parameter is the name;
		// the rest is the value. A text value may itself contain spaces (a
		// path), a numeric one is one word.
		std::string name, value;
		bool        found = false;
		for( size_t n = words.size() - 1; n >= 1 && !found; --n )
		{
			std::string candidate;
			for( size_t i = 0; i < n; ++i )
				candidate += ( i ? " " : "" ) + words[ i ];
			if( types.count( candidate ) )
			{
				name  = candidate;
				found = true;
				for( size_t i = n; i < words.size(); ++i )
					value += ( i > n ? " " : "" ) + words[ i ];
			}
		}
		if( !found )
		{
			error = path + ":" + std::to_string( lineNumber )
					+ ": no automatable parameter named by \"" + line + "\"";
			return {};
		}
		if( IsTextParam( types.at( name ) ) )
			// `-` is the empty string: `Content -` clears the content so a
			// no-content core (2048, gong) loads with none, as the inspector's
			// cleared file picker would.
			script.text[ name ].emplace_back( frame, value == "-" ? std::string() : value );
		else
			script.numeric[ name ].emplace_back( frame, std::strtof( value.c_str(), nullptr ) );
	}
	for( auto& entry : script.numeric )
		std::sort( entry.second.begin(), entry.second.end() );
	for( auto& entry : script.text )
		std::stable_sort( entry.second.begin(), entry.second.end(),
						  []( const auto& a, const auto& b ) { return a.first < b.first; } );
	return script;
}

float ValueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = float( b.first - a.first );
			const float t    = span > 0.0f ? float( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

/// A text track holds its last key at or before `frame`; nothing before the first.
const std::string* TextAt( const TextTrack& track, int frame )
{
	const std::string* current = nullptr;
	for( const auto& key : track )
		if( key.first <= frame )
			current = &key.second;
	return current;
}

struct PipeOptions
{
	int         frames = 0; ///< 0: until the reader hangs up
	double      fps    = 30.0;
	std::string script;
	std::vector< std::pair< std::string, std::string > > sets;
};

bool WriteAll( int fd, const uint8_t* data, size_t bytes )
{
	size_t written = 0;
	while( written < bytes )
	{
		const ssize_t put = write( fd, data + written, bytes - written );
		if( put <= 0 )
			return false;
		written += size_t( put );
	}
	return true;
}

int RunPipe( const std::string& corePath, const std::string& contentPath, unsigned vw, unsigned vh,
			 const PipeOptions& o )
{
	std::signal( SIGPIPE, SIG_IGN );
	if( vw == 0 || vh == 0 || !( o.fps > 0.0 ) )
	{
		std::fprintf( stderr, "cargl: --pipe needs a positive size and --fps\n" );
		return 1;
	}

	const int frameFd = dup( STDOUT_FILENO );
	dup2( STDERR_FILENO, STDOUT_FILENO );
	if( frameFd < 0 )
	{
		std::fprintf( stderr, "cargl: cannot take stdout for the frames\n" );
		return 1;
	}

	CGLContextObj context = MakeContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "cargl: could not create a GL 4.1 core context\n" );
		return 1;
	}
	Target target = MakeTarget( vw, vh );

	CartridgePlugin plugin;

	// Names to ids and types: everything before the About block. An About
	// button that "moved" would open a browser.
	std::map< std::string, unsigned > byName;
	std::map< std::string, unsigned > typeByName;
	for( unsigned id = 0; id < PT_ABOUT_TEXT; ++id )
		if( const char* name = plugin.GetParamName( id ) )
		{
			byName[ name ]     = id;
			typeByName[ name ] = plugin.GetParamType( id );
		}

	std::map< unsigned, float >       lastNumeric;
	std::map< unsigned, std::string > lastText;

	auto setText = [ & ]( unsigned id, const std::string& value ) {
		const auto seen = lastText.find( id );
		if( seen != lastText.end() && seen->second == value )
			return;
		plugin.SetTextParameter( id, value.c_str() );
		lastText[ id ] = value;
	};
	auto setNumeric = [ & ]( unsigned id, float value ) {
		const auto seen = lastNumeric.find( id );
		if( seen != lastNumeric.end() && seen->second == value )
			return;
		plugin.SetFloatParameter( id, value );
		lastNumeric[ id ] = value;
	};

	Script script;
	if( !o.script.empty() )
	{
		std::string error;
		script = LoadScript( o.script, typeByName, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "cargl: %s\n", error.c_str() );
			return 1;
		}
	}

	auto apply = [ & ]( int frame ) {
		for( const auto& track : script.text )
			if( const std::string* value = TextAt( track.second, frame ) )
				setText( byName.at( track.first ), *value );
		for( const auto& track : script.numeric )
			setNumeric( byName.at( track.first ), ValueAt( track.second, frame ) );
	};

	FFGLViewportStruct vp = {};
	vp.width              = vw;
	vp.height             = vh;
	if( plugin.InitGL( &vp ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "cargl: InitGL failed -- shader did not build\n" );
		return 1;
	}

	// The command line's core and content first, then --set, then frame 0 of
	// the sheet, so the sheet can override either and the take opens on the
	// game rather than restarting it a frame in.
	if( !corePath.empty() )
		setText( PT_CORE, corePath );
	if( !contentPath.empty() )
		setText( PT_CONTENT, contentPath );
	for( const auto& kv : o.sets )
	{
		const auto found = byName.find( kv.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "cargl: no parameter named '%s'\n", kv.first.c_str() );
			plugin.DeInitGL();
			return 1;
		}
		if( IsTextParam( typeByName.at( kv.first ) ) )
			setText( found->second, kv.second );
		else
			setNumeric( found->second, float( std::atof( kv.second.c_str() ) ) );
	}
	apply( 0 );

	// The take starts when the console reaches the screen, not while the core
	// is still loading: Resolume would show black there, and nobody films that.
	// A core whose opening frames really are black (a console booting) is not a
	// failure, so this waits a few seconds and then films whatever is there.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
		while( std::chrono::steady_clock::now() < deadline )
		{
			const auto rgba = DrawOnce( plugin, target );
			bool       lit  = false;
			for( size_t i = 0; i + 3 < rgba.size() && !lit; i += 4 )
				lit = rgba[ i ] || rgba[ i + 1 ] || rgba[ i + 2 ];
			if( lit )
				break;
			std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
		}
	}

	const size_t           rowBytes = size_t( vw ) * 4;
	std::vector< uint8_t > frame( rowBytes * vh );
	int                    status = 0;

	const auto t0 = std::chrono::steady_clock::now();
	for( int f = 0; o.frames <= 0 || f < o.frames; ++f )
	{
		apply( f );

		// Real time, not the frame counter: the console's clock is the wall's.
		std::this_thread::sleep_until(
			t0 + std::chrono::duration_cast< std::chrono::steady_clock::duration >(
					 std::chrono::duration< double >( double( f ) / o.fps ) ) );

		const auto rgba = DrawOnce( plugin, target );
		for( unsigned y = 0; y < vh; ++y )
			std::memcpy( frame.data() + size_t( y ) * rowBytes,
						 rgba.data() + size_t( vh - 1 - y ) * rowBytes, rowBytes );

		if( !WriteAll( frameFd, frame.data(), frame.size() ) )
		{
			// The reader hung up. Short of a requested length that is a
			// failure the pipeline should see; "until then" is exit 0.
			status = o.frames > 0 ? 1 : 0;
			break;
		}
	}

	plugin.DeInitGL();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return status;
}

} // namespace

int main( int argc, char** argv )
{
	diag::setConsoleEcho( true );

	std::string corePath;
	std::string contentPath;
	std::string outPath;
	std::string helperChannel;
	unsigned vw = 1280, vh = 720;
	bool doCheck       = false;
	bool doPipe        = false;
	int surviveSeconds = 0;
	PipeOptions pipe;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		auto next           = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : ""; };

		if( a == "--pipe" )
			doPipe = true;
		else if( a == "--fps" )
			pipe.fps = std::atof( next().c_str() );
		else if( a == "--frames" )
			pipe.frames = std::atoi( next().c_str() );
		else if( a == "--script" )
			pipe.script = next();
		else if( a == "--set" )
		{
			const std::string kv     = next();
			const size_t      equals = kv.find( '=' );
			if( equals == std::string::npos )
			{
				Usage();
				return 2;
			}
			pipe.sets.emplace_back( kv.substr( 0, equals ), kv.substr( equals + 1 ) );
		}
		else if( a == "--core" )
			corePath = next();
		else if( a == "--helper" )
			helperChannel = next();
		else if( a == "--content" )
			contentPath = next();
		else if( a == "--out" )
			outPath = next();
		else if( a == "--check" )
			doCheck = true;
		else if( a == "--survive" )
			surviveSeconds = std::stoi( next() );
		else if( a == "--size" )
		{
			const std::string s = next();
			const size_t x      = s.find( 'x' );
			if( x != std::string::npos )
			{
				vw = unsigned( std::stoul( s.substr( 0, x ) ) );
				vh = unsigned( std::stoul( s.substr( x + 1 ) ) );
			}
		}
		else
		{
			Usage();
			return ( a == "--help" || a == "-h" ) ? 0 : 2;
		}
	}

	if( doPipe )
		// No default core: the sheet's frame 0 may name one, and the test core
		// is nothing anyone films.
		return RunPipe( corePath, contentPath, vw, vh, pipe );

	if( corePath.empty() )
		corePath = TestCorePath( argv[ 0 ] );

	CGLContextObj context = MakeContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create a GL 4.1 core context\n" );
		return 1;
	}

	{
		const GLubyte* v = glGetString( GL_VERSION );
		std::printf( "GL %s\n", v ? (const char*)v : "?" );
	}

	Target target = MakeTarget( vw, vh );

	// --- the real plugin, through the real sequence -------------------------
	CartridgePlugin plugin;

	FFGLViewportStruct vp = {};
	vp.x                  = 0;
	vp.y                  = 0;
	vp.width              = vw;
	vp.height             = vh;

	if( plugin.InitGL( &vp ) != FF_SUCCESS )
	{
		// The failure this harness exists for. The GL strings are already in
		// the diag log next to the compile error.
		Fail( "InitGL failed -- shader did not build" );
		return 1;
	}
	Pass( "InitGL: shader built and resources allocated" );

	if( !helperChannel.empty() )
	{
		// The out-of-process path, driven through exactly the same plugin
		// object and the same parameters an operator would use. Nothing about
		// the checks below changes -- which is the point: if the corner test
		// passes in both modes, the two transports agree pixel for pixel.
		plugin.SetTextParameter( PT_CHANNEL, helperChannel.c_str() );
		plugin.SetFloatParameter( PT_SOURCE, 1.0f ); // Helper
		std::printf( "attaching to helper channel '%s'\n", helperChannel.c_str() );
	}
	else
	{
		plugin.SetTextParameter( PT_CORE, corePath.c_str() );
		if( !contentPath.empty() )
			plugin.SetTextParameter( PT_CONTENT, contentPath.c_str() );
	}

	if( !PumpUntilDrawn( plugin, target ) )
	{
		Fail( "no frame reached the framebuffer within 240 attempts" );
		plugin.DeInitGL();
		return 1;
	}
	Pass( "a frame reached the framebuffer" );

	const auto rgba = ReadBack( target );

	if( !outPath.empty() )
	{
		if( png::WritePng( outPath, rgba.data(), vw, vh ) )
			std::printf( "wrote %s (%ux%u)\n", outPath.c_str(), vw, vh );
		else
			Fail( "could not write " + outPath );
	}

	if( surviveSeconds > 0 )
	{
		/*
			The claim the out-of-process build exists to make, tested rather
			than asserted in a README: **kill the helper and the consumer keeps
			running with its last frame intact.**

			The script that drives this sends SIGKILL, not SIGTERM -- a core
			that segfaults gets no chance to tidy up, so neither does the
			helper here. What must survive is the reader: a shared segment
			whose writer vanished mid-publish, a heartbeat that stops, and a
			`ready` word that may name a slot the helper was halfway through
			filling.
		*/
		const auto deadline = std::chrono::steady_clock::now()
							  + std::chrono::seconds( surviveSeconds );

		bool sawFrames  = false;
		bool everFailed = false;
		int draws       = 0;

		while( std::chrono::steady_clock::now() < deadline )
		{
			const auto s = DrawOnce( plugin, target );
			++draws;

			// The corner blocks are in every frame the test core draws, so
			// their presence is the test for "a picture is still there".
			const FittedRect r = FitRect( vw, vh );
			const bool corner  = png::Sample( s.data(), vw, vh, r.left + r.blockW / 2,
											  r.top + r.blockH / 2 )
									.Near( 255, 0, 0 );

			if( corner )
				sawFrames = true;
			else if( sawFrames )
				everFailed = true; // had a picture, then lost it

			std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
		}

		std::printf( "drew %d frames over %d s\n", draws, surviveSeconds );

		if( sawFrames && !everFailed )
			Pass( "helper death: the consumer kept drawing and held its last frame" );
		else if( !sawFrames )
			Fail( "helper death: never saw a picture at all -- was the helper running?" );
		else
			Fail( "helper death: the picture disappeared after the helper stopped" );

		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );

		std::printf( "\n%s\n", g_failures == 0
								   ? "all checks passed"
								   : ( std::to_string( g_failures ) + " check(s) failed" ).c_str() );
		return g_failures == 0 ? 0 : 1;
	}

	if( doCheck )
	{
		CheckFittedCorners( rgba, vw, vh );

		// Stretch must reach the frame edge. Scaling is applied on the very next
		// draw -- it is pure render-side arithmetic and does not wait on the
		// emulator -- but it is still expressed as a condition so a regression
		// that made it lag would fail rather than pass intermittently.
		plugin.SetFloatParameter( PT_SCALING, 2.0f ); // Stretch
		{
			const bool ok = PumpUntil(
				plugin, target,
				[ & ]( const std::vector< uint8_t >& s ) {
					// The very corner of the output is the very corner of the
					// picture, which is the top-left red block.
					return png::Sample( s.data(), vw, vh, 4, 4 ).Near( 255, 0, 0 );
				},
				30 );

			if( ok )
				Pass( "GL stretch: picture reaches the frame edge" );
			else
				Fail( "GL stretch: expected red in the very corner, got "
					  + png::Sample( ReadBack( target ).data(), vw, vh, 4, 4 ).Str() );
		}

		// Back to Fit, then hold a button and confirm it reaches the picture
		// through the plugin's own parameter path rather than the Core API.
		plugin.SetFloatParameter( PT_SCALING, 0.0f );
		plugin.SetFloatParameter( PT_A, 1.0f );
		{
			const bool ok = PumpUntil( plugin, target,
									   [ & ]( const std::vector< uint8_t >& s ) {
										   return png::Sample( s.data(), vw, vh, vw / 2, vh / 2 )
											   .Near( 0, 255, 255 );
									   } );
			if( ok )
				Pass( "GL input: the A parameter reaches the emulated pad" );
			else
				Fail( "GL input: expected cyan at the centre, got "
					  + png::Sample( ReadBack( target ).data(), vw, vh, vw / 2, vh / 2 ).Str() );
		}

		// And released again -- a pad that latches on would pass the check above
		// and be unusable.
		plugin.SetFloatParameter( PT_A, 0.0f );
		{
			const bool ok = PumpUntil( plugin, target,
									   [ & ]( const std::vector< uint8_t >& s ) {
										   return !png::Sample( s.data(), vw, vh, vw / 2, vh / 2 )
													   .Near( 0, 255, 255 );
									   } );
			if( ok )
				Pass( "GL input: releasing the A parameter releases the pad" );
			else
				Fail( "GL input: the pad latched on after A was released" );
		}
	}

	plugin.DeInitGL();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );

	if( doCheck )
		std::printf( "\n%s\n", g_failures == 0
								   ? "all checks passed"
								   : ( std::to_string( g_failures ) + " check(s) failed" ).c_str() );

	return g_failures == 0 ? 0 : 1;
}
