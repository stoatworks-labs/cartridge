#include "Core.h"

#include "Diag.h"

#include "libretro.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dlfcn.h>
	#include <unistd.h>
#endif

namespace cartridge
{

// ---------------------------------------------------------------------------
// The entry points every libretro core exports.
// ---------------------------------------------------------------------------
struct Core::Api
{
	unsigned ( *api_version )( void )                                = nullptr;
	void ( *init )( void )                                           = nullptr;
	void ( *deinit )( void )                                         = nullptr;
	void ( *get_system_info )( struct retro_system_info* )           = nullptr;
	void ( *get_system_av_info )( struct retro_system_av_info* )     = nullptr;
	void ( *set_environment )( retro_environment_t )                 = nullptr;
	void ( *set_video_refresh )( retro_video_refresh_t )             = nullptr;
	void ( *set_audio_sample )( retro_audio_sample_t )               = nullptr;
	void ( *set_audio_sample_batch )( retro_audio_sample_batch_t )   = nullptr;
	void ( *set_input_poll )( retro_input_poll_t )                   = nullptr;
	void ( *set_input_state )( retro_input_state_t )                 = nullptr;
	void ( *set_controller_port_device )( unsigned, unsigned )       = nullptr;
	void ( *reset )( void )                                          = nullptr;
	void ( *run )( void )                                            = nullptr;
	bool ( *load_game )( const struct retro_game_info* )             = nullptr;
	void ( *unload_game )( void )                                    = nullptr;
};

namespace
{

// ---------------------------------------------------------------------------
// Callback routing.
//
// See the header for why this is a thread_local rather than a member: the
// libretro ABI has no user pointer on any callback, so the only thing a
// callback can use to find its way home is ambient state. Each Core is driven
// from exactly one thread, so "the Core belonging to this thread" is a unique
// answer.
//
// The guard is re-entrant-safe by saving and restoring rather than clearing,
// because a core may call environment from inside load_game, which we are
// already inside.
// ---------------------------------------------------------------------------
thread_local Core* tCurrent = nullptr;

struct Enter
{
	Core* previous;
	explicit Enter( Core* c ) :
		previous( tCurrent )
	{
		tCurrent = c;
	}
	~Enter()
	{
		tCurrent = previous;
	}
};

void* OpenLibrary( const std::string& path, std::string& error )
{
#if defined( _WIN32 )
	void* h = (void*)LoadLibraryA( path.c_str() );
	if( !h )
		error = "LoadLibrary failed for " + path;
	return h;
#else
	// RTLD_NOW so a core missing a symbol fails here, with a message, instead
	// of taking the host down on first use from inside retro_run.
	void* h = dlopen( path.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( !h )
	{
		const char* e       = dlerror();
		const std::string d = e ? e : ( "dlopen failed for " + path );
		error               = d;

		/*
			The two failures that account for nearly every "the layer is black",
			annotated because dlerror describes the symptom and not the fix, and
			because both look identical from inside Resolume.

			Neither is a bug in the core or in us -- they are what happens when
			you download a core from a browser onto an Apple Silicon Mac.
		*/
		if( d.find( "incompatible architecture" ) != std::string::npos )
		{
			error += "\n  -> This core is built for a different architecture than the process "
					 "loading it.\n     What matters is the host process, not the machine: "
					 "in-process must match\n     Resolume, Helper mode must match "
					 "cartridge-helper. Get the matching\n     build from the libretro "
					 "buildbot -- see docs/CORES.md.";
		}
		else if( d.find( "code signature" ) != std::string::npos
				 || d.find( "cdhash" ) != std::string::npos
				 || d.find( "quarantine" ) != std::string::npos )
		{
			error += "\n  -> Gatekeeper is refusing this file, most likely because it was "
					 "downloaded\n     and is still quarantined. Strip it with:\n"
					 "       xattr -dr com.apple.quarantine <folder>\n"
					 "     Quarantine is per file, so this recurs with each new core.";
		}
	}
	return h;
#endif
}

void CloseLibrary( void* handle )
{
	if( !handle )
		return;
#if defined( _WIN32 )
	FreeLibrary( (HMODULE)handle );
#else
	dlclose( handle );
#endif
}

void* Symbol( void* handle, const char* name )
{
#if defined( _WIN32 )
	return (void*)GetProcAddress( (HMODULE)handle, name );
#else
	return dlsym( handle, name );
#endif
}

/// Copy a core to a private path so this instance gets its own globals.
/// See trap 2 in the header.
std::string MakePrivateCopy( const std::string& source, std::string& error )
{
	static std::atomic< unsigned > counter{ 0 };

	std::string dir;
#if defined( _WIN32 )
	char buf[ MAX_PATH ] = {};
	GetTempPathA( MAX_PATH, buf );
	dir = buf;
#else
	const char* t = getenv( "TMPDIR" );
	dir           = t ? t : "/tmp/";
	if( dir.back() != '/' )
		dir += '/';
#endif

	// Keep the original extension: dlopen does not care, but a bundle loader
	// and every debugging tool does.
	std::string ext;
	const size_t dot = source.find_last_of( '.' );
	if( dot != std::string::npos )
		ext = source.substr( dot );

	char name[ 128 ];
	std::snprintf( name, sizeof( name ), "cartridge-%d-%u%s",
				   int(
#if defined( _WIN32 )
					   GetCurrentProcessId()
#else
					   getpid()
#endif
						   ),
				   counter.fetch_add( 1 ), ext.c_str() );

	const std::string dest = dir + name;

	std::ifstream in( source, std::ios::binary );
	if( !in )
	{
		error = "cannot read core at " + source;
		return {};
	}
	std::ofstream out( dest, std::ios::binary | std::ios::trunc );
	if( !out )
	{
		error = "cannot write private core copy to " + dest;
		return {};
	}
	out << in.rdbuf();
	out.close();

	if( !out.good() )
	{
		error = "private core copy to " + dest + " did not complete";
		return {};
	}

	return dest;
}

} // namespace

// ---------------------------------------------------------------------------
// C trampolines. Each finds its Core through the thread_local and forwards.
// ---------------------------------------------------------------------------
struct CoreCallbacks
{
	static bool Environment( unsigned cmd, void* data )
	{
		return tCurrent ? tCurrent->OnEnvironment( cmd, data ) : false;
	}

	static void Video( const void* data, unsigned w, unsigned h, size_t pitch )
	{
		if( tCurrent )
			tCurrent->OnVideo( data, w, h, pitch );
	}

	static size_t AudioBatch( const int16_t* data, size_t frames )
	{
		if( tCurrent )
			tCurrent->OnAudioBatch( data, frames );
		return frames;
	}

	static void AudioSample( int16_t left, int16_t right )
	{
		// The single-sample callback. Cores that use the batch form never call
		// this, but a few older ones use only this one, and a frontend that
		// leaves it null gets a crash rather than silence.
		const int16_t pair[ 2 ] = { left, right };
		if( tCurrent )
			tCurrent->OnAudioBatch( pair, 1 );
	}

	static void InputPoll( void )
	{
		// Nothing to do: the pad is already an atomic word that SetPad wrote.
		// Cores call this once per frame before reading, and some assert if it
		// is null.
	}

	static int16_t InputState( unsigned port, unsigned device, unsigned index, unsigned id )
	{
		return tCurrent ? tCurrent->OnInput( port, device, index, id ) : 0;
	}

	static void Log( enum retro_log_level level, const char* fmt, ... )
	{
		if( !tCurrent )
			return;
		va_list args;
		va_start( args, fmt );
		tCurrent->OnLog( fmt, args );
		va_end( args );
		(void)level;
	}
};

// ---------------------------------------------------------------------------

Core::Core() :
	mApi( new Api )
{
}

Core::~Core()
{
	Unload();
	delete mApi;
}

bool Core::Load( const std::string& corePath, std::string& error, bool uniqueInstance )
{
	Unload();

	std::string openPath = corePath;
	if( uniqueInstance )
	{
		const std::string copy = MakePrivateCopy( corePath, error );
		if( copy.empty() )
			return false;
		openPath  = copy;
		mTempCopy = copy;
	}

	mHandle = OpenLibrary( openPath, error );
	if( !mHandle )
	{
		if( !mTempCopy.empty() )
		{
			std::remove( mTempCopy.c_str() );
			mTempCopy.clear();
		}
		return false;
	}

#define BIND( field, symbol )                                            \
	do                                                                   \
	{                                                                    \
		*(void**)( &mApi->field ) = Symbol( mHandle, symbol );            \
		if( mApi->field == nullptr )                                     \
		{                                                                \
			error = std::string( "core is missing " ) + symbol;          \
			Unload();                                                    \
			return false;                                                \
		}                                                                \
	} while( 0 )

	BIND( api_version, "retro_api_version" );
	BIND( init, "retro_init" );
	BIND( deinit, "retro_deinit" );
	BIND( get_system_info, "retro_get_system_info" );
	BIND( get_system_av_info, "retro_get_system_av_info" );
	BIND( set_environment, "retro_set_environment" );
	BIND( set_video_refresh, "retro_set_video_refresh" );
	BIND( set_audio_sample, "retro_set_audio_sample" );
	BIND( set_audio_sample_batch, "retro_set_audio_sample_batch" );
	BIND( set_input_poll, "retro_set_input_poll" );
	BIND( set_input_state, "retro_set_input_state" );
	BIND( set_controller_port_device, "retro_set_controller_port_device" );
	BIND( reset, "retro_reset" );
	BIND( run, "retro_run" );
	BIND( load_game, "retro_load_game" );
	BIND( unload_game, "retro_unload_game" );
#undef BIND

	const unsigned version = mApi->api_version();
	if( version != RETRO_API_VERSION )
	{
		// Refuse rather than try. The ABI version is the only thing standing
		// between us and calling a struct layout that has moved underneath us,
		// and the failure mode of getting that wrong is memory corruption
		// inside Resolume.
		char buf[ 160 ];
		std::snprintf( buf, sizeof( buf ),
					   "core reports libretro API %u, this build speaks %u",
					   version, unsigned( RETRO_API_VERSION ) );
		error = buf;
		Unload();
		return false;
	}

	Enter guard( this );

	// set_environment must come first: cores call environment from inside it,
	// and several decide their pixel format and options there.
	mApi->set_environment( &CoreCallbacks::Environment );
	mApi->init();

	mApi->set_video_refresh( &CoreCallbacks::Video );
	mApi->set_audio_sample( &CoreCallbacks::AudioSample );
	mApi->set_audio_sample_batch( &CoreCallbacks::AudioBatch );
	mApi->set_input_poll( &CoreCallbacks::InputPoll );
	mApi->set_input_state( &CoreCallbacks::InputState );

	retro_system_info info = {};
	mApi->get_system_info( &info );
	mInfo.libraryName     = info.library_name ? info.library_name : "";
	mInfo.libraryVersion  = info.library_version ? info.library_version : "";
	mInfo.validExtensions = info.valid_extensions ? info.valid_extensions : "";
	mInfo.needFullpath    = info.need_fullpath;
	mInfo.blockExtract    = info.block_extract;

	diag::info( "loaded core " + mInfo.libraryName + " " + mInfo.libraryVersion );
	return true;
}

bool Core::LoadContent( const std::string& contentPath, std::string& error )
{
	if( !IsLoaded() )
	{
		error = "no core loaded";
		return false;
	}

	Enter guard( this );

	retro_game_info game = {};
	bool ok              = false;

	if( contentPath.empty() )
	{
		if( !mInfo.supportsNoGame )
		{
			error = "core " + mInfo.libraryName + " requires content";
			return false;
		}
		ok = mApi->load_game( nullptr );
	}
	else if( mInfo.needFullpath )
	{
		// The core opens the file itself. Handing it data as well is not just
		// wasteful -- some cores assert if both are set.
		game.path = contentPath.c_str();
		ok        = mApi->load_game( &game );
	}
	else
	{
		std::ifstream in( contentPath, std::ios::binary | std::ios::ate );
		if( !in )
		{
			error = "cannot read content at " + contentPath;
			return false;
		}
		const std::streamsize size = in.tellg();
		in.seekg( 0 );
		mContentData.resize( size_t( size ) );
		if( !in.read( reinterpret_cast< char* >( mContentData.data() ), size ) )
		{
			error = "short read on " + contentPath;
			return false;
		}

		// mContentData must outlive load_game *and* stay put -- cores are
		// permitted to keep the pointer for the life of the session rather than
		// copying it, so this vector is a member and is not resized afterwards.
		game.path = contentPath.c_str();
		game.data = mContentData.data();
		game.size = mContentData.size();
		ok        = mApi->load_game( &game );
	}

	if( !ok )
	{
		error = "core rejected the content";
		mContentData.clear();
		return false;
	}

	retro_system_av_info av = {};
	mApi->get_system_av_info( &av );

	mFps        = av.timing.fps > 0.0 ? av.timing.fps : 60.0;
	mSampleRate = av.timing.sample_rate > 0.0 ? av.timing.sample_rate : 48000.0;
	mMaxWidth   = av.geometry.max_width;
	mMaxHeight  = av.geometry.max_height;
	mAspect     = av.geometry.aspect_ratio;

	// Size all three slots now, on this thread. Growing a vector from inside
	// the video callback would allocate on the emulator thread mid-frame, and
	// at 60 Hz that shows up as a stutter every time a core changes geometry.
	if( mMaxWidth && mMaxHeight )
	{
		for( int i = 0; i < 3; ++i )
		{
			Frame& f = mFrames.BeginWrite();
			f.pixels.resize( size_t( mMaxWidth ) * size_t( mMaxHeight ) * 4 );
			mFrames.Publish();
		}
	}

	mContentLoaded = true;

	char buf[ 200 ];
	std::snprintf( buf, sizeof( buf ),
				   "content loaded: %ux%u max, %.4f fps, %.0f Hz audio, aspect %.4f",
				   mMaxWidth, mMaxHeight, mFps, mSampleRate, double( mAspect ) );
	diag::info( buf );

	return true;
}

void Core::RunFrame()
{
	if( !mContentLoaded )
		return;

	Enter guard( this );
	mApi->run();
}

void Core::Reset()
{
	if( !mContentLoaded )
		return;

	Enter guard( this );
	mApi->reset();
	mAudio.Reset();
}

void Core::Unload()
{
	if( mHandle )
	{
		Enter guard( this );

		if( mContentLoaded && mApi->unload_game )
			mApi->unload_game();
		if( mApi->deinit )
			mApi->deinit();
	}

	CloseLibrary( mHandle );
	mHandle = nullptr;

	if( !mTempCopy.empty() )
	{
		std::remove( mTempCopy.c_str() );
		mTempCopy.clear();
	}

	if( mApi )
		*mApi = Api{};

	mContentLoaded = false;
	mContentData.clear();
	mInfo = Info{};
}

std::vector< std::string > Core::DrainLog()
{
	std::vector< std::string > out;
	out.swap( mLogLines );
	return out;
}

// ---------------------------------------------------------------------------
// Callback bodies
// ---------------------------------------------------------------------------

bool Core::OnEnvironment( unsigned cmd, void* data )
{
	switch( cmd )
	{
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		// True means "you may pass NULL to video_refresh to repeat the last
		// frame". Cores use it to skip work on duplicate frames, and the
		// triple buffer already holds the previous frame, so this is free.
		*static_cast< bool* >( data ) = true;
		return true;

	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	{
		const auto fmt = *static_cast< const enum retro_pixel_format* >( data );
		switch( fmt )
		{
		case RETRO_PIXEL_FORMAT_XRGB8888: mFormat = pixels::Format::XRGB8888; return true;
		case RETRO_PIXEL_FORMAT_RGB565:   mFormat = pixels::Format::RGB565;   return true;
		case RETRO_PIXEL_FORMAT_0RGB1555: mFormat = pixels::Format::ORGB1555; return true;
		default:
			// Returning false here is the contract's way of saying "pick
			// something else", and cores that ask for 10-bit will fall back.
			// Claiming support we do not have would put noise on the output.
			mFormat = pixels::Format::Unknown;
			return false;
		}
	}

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		mInfo.supportsNoGame = *static_cast< const bool* >( data );
		return true;

	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*static_cast< const char** >( data ) = mSystemDir.empty() ? nullptr : mSystemDir.c_str();
		return true;

	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*static_cast< const char** >( data ) = mSaveDir.empty() ? nullptr : mSaveDir.c_str();
		return true;

	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		static_cast< retro_log_callback* >( data )->log = &CoreCallbacks::Log;
		return true;

	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	{
		// A core may change its own geometry mid-session -- the SNES switching
		// to hi-res, a handheld rotating. Accept it and re-read, but do NOT
		// resize the frame slots here: this arrives on the emulator thread,
		// inside retro_run. The slots were already allocated at max geometry
		// for exactly this reason.
		if( cmd == RETRO_ENVIRONMENT_SET_GEOMETRY )
		{
			const auto* g = static_cast< const retro_game_geometry* >( data );
			mAspect       = g->aspect_ratio;
		}
		else
		{
			const auto* av = static_cast< const retro_system_av_info* >( data );
			mAspect        = av->geometry.aspect_ratio;
			mFps           = av->timing.fps > 0.0 ? av->timing.fps : mFps;
			mSampleRate    = av->timing.sample_rate > 0.0 ? av->timing.sample_rate : mSampleRate;
		}
		return true;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		// Core options are not exposed yet, so there is never a value to give.
		//
		// This *returns false* rather than returning true with value = nullptr.
		// Both are legal readings of the header, but they are not equally safe:
		// a great many cores are written as
		//
		//     if( environ_cb( GET_VARIABLE, &var ) ) strcmp( var.value, "on" );
		//
		// and dereference the null the moment the call says true. fceumm and
		// Genesis Plus GX both segfault that way, during retro_load_game, which
		// looks exactly like a malformed ROM from the outside. Answering false
		// makes every core fall back to its own defaults, which is what we want
		// from a frontend that exposes no options.
		static_cast< retro_variable* >( data )->value = nullptr;
		return false;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*static_cast< bool* >( data ) = false;
		return true;

	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		// Accepted and ignored: all three are the core telling us about itself,
		// and none of them need an answer for the core to run.
		return true;

	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		// Deliberately refused in this build. Accepting means promising a GL
		// context, an FBO and a get_current_framebuffer callback on the
		// emulator thread, and a hardware core that gets a half-kept promise
		// does not fail cleanly -- it renders into whatever is bound, which in
		// the plugin build is Resolume's own framebuffer. See AGENTS.md for
		// what accepting it properly involves.
		diag::warn( "core asked for hardware rendering; refused (software cores only in this build)" );
		return false;

	default:
		return false;
	}
}

void Core::OnVideo( const void* data, unsigned width, unsigned height, size_t pitch )
{
	// NULL means "same as last frame" and is legal because we answered
	// GET_CAN_DUPE with true. Publishing nothing leaves the reader holding the
	// previous frame, which is exactly right.
	if( data == nullptr || width == 0 || height == 0 )
		return;

	// RETRO_HW_FRAME_BUFFER_VALID is ((void*)-1) and means the frame is already
	// on the GPU. We refuse SET_HW_RENDER, so seeing it means a core ignored
	// that refusal; dereferencing it would be an immediate segfault inside
	// Resolume.
	if( data == RETRO_HW_FRAME_BUFFER_VALID )
	{
		diag::error( "core delivered a hardware frame despite SET_HW_RENDER being refused" );
		return;
	}

	Frame& f = mFrames.BeginWrite();

	const size_t needed = size_t( width ) * size_t( height ) * 4;
	if( f.pixels.size() < needed )
		f.pixels.resize( needed ); // only if the core outgrew its declared max

	f.width       = width;
	f.height      = height;
	f.aspectRatio = mAspect > 0.0f ? mAspect : float( width ) / float( height );

	pixels::Convert( f.pixels.data(), data, width, height, pitch, mFormat,
					 /*flipVertical*/ true );

	mFrames.Publish();
}

void Core::OnAudioBatch( const int16_t* data, size_t frames )
{
	mAudio.Write( data, frames );
}

int16_t Core::OnInput( unsigned port, unsigned device, unsigned index, unsigned id )
{
	(void)index;
	if( device != RETRO_DEVICE_JOYPAD )
		return 0;
	return mInput.Poll( port, id );
}

void Core::OnLog( const char* fmt, va_list args )
{
	char buf[ 1024 ];
	std::vsnprintf( buf, sizeof( buf ), fmt, args );

	// Cores end their lines with \n and the diag log adds its own.
	std::string line( buf );
	while( !line.empty() && ( line.back() == '\n' || line.back() == '\r' ) )
		line.pop_back();

	if( line.empty() )
		return;

	if( mLogLines.size() < 512 )
		mLogLines.push_back( line );

	diag::info( "core: " + line );
}

} // namespace cartridge
