/*
	cartridge-helper -- the out-of-process build.

	Runs a libretro core in its own process and publishes frames into a shared
	channel. The FFGL plugin attaches to that channel in Helper mode and draws
	whatever is there.

	**What this buys, and it is the whole point: a core that crashes crashes
	this, not Resolume.** An emulator is a large body of C doing unusual things
	to memory, and cores vary a lot in how finished they are. In-process, a bad
	dereference in the middle of a show takes the show down. Out of process, the
	helper dies, the plugin notices the heartbeat stopped, and the layer holds
	its last frame until someone restarts the helper. That is a recoverable
	incident instead of a black room.

	**What it costs**: one frame of latency (the publish is seen by the next
	composition frame) and a 286 KB memcpy. See Shared.h for why the copy is the
	right trade at console resolutions and where it stops being one.

	It is also the only way to run **two instances of the same core** without
	the private-copy trick in Core.h -- separate processes have separate globals
	by construction.

		cartridge-helper --core PATH [--content PATH] [--channel NAME]

	The channel name defaults to "default". Two helpers on one machine need two
	names, and the plugin needs to be pointed at the matching one.
*/

#include "Core.h"
#include "Diag.h"
#include "Runner.h"
#include "Shared.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined( __APPLE__ )
	#include <pthread.h>
#endif

using namespace cartridge;

namespace
{

std::atomic< bool > g_quit{ false };

void OnSignal( int )
{
	// Only a flag. Doing real work in a signal handler -- unmapping shared
	// memory, closing a core -- is undefined behaviour, and a helper that
	// crashes while cleaning up after Ctrl-C leaves exactly the stale segment
	// the magic check in Shared.cpp exists to catch.
	g_quit.store( true, std::memory_order_release );
}

void SetStatus( shared::Block* block, const std::string& text )
{
	std::strncpy( block->status, text.c_str(), sizeof( block->status ) - 1 );
	block->status[ sizeof( block->status ) - 1 ] = '\0';
}

void Usage()
{
	std::printf(
		"cartridge-helper -- run a libretro core in its own process\n\n"
		"  --core PATH      libretro core to load (required)\n"
		"  --content PATH   content to load (omit for a no-content core)\n"
		"  --channel NAME   shared channel name (default \"default\")\n"
		"  --system DIR     system/BIOS directory handed to the core\n"
		"  --save DIR       save directory handed to the core\n"
		"  --quiet          do not echo the log to stderr\n" );
}

} // namespace

int main( int argc, char** argv )
{
	std::string corePath, contentPath, systemDir, saveDir;
	std::string channel = "default";
	bool quiet          = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		auto next           = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : ""; };

		if( a == "--core" )
			corePath = next();
		else if( a == "--content" )
			contentPath = next();
		else if( a == "--channel" )
			channel = next();
		else if( a == "--system" )
			systemDir = next();
		else if( a == "--save" )
			saveDir = next();
		else if( a == "--quiet" )
			quiet = true;
		else
		{
			Usage();
			return ( a == "--help" || a == "-h" ) ? 0 : 2;
		}
	}

	diag::init();
	diag::setConsoleEcho( !quiet );

	if( corePath.empty() )
	{
		Usage();
		return 2;
	}

	std::signal( SIGINT, OnSignal );
	std::signal( SIGTERM, OnSignal );

	std::string error;
	shared::Block* block = shared::Open( channel, /*create*/ true, error );
	if( block == nullptr )
	{
		std::fprintf( stderr, "%s\n", error.c_str() );
		return 1;
	}

	Runner runner;
	Core& core = runner.GetCore();

	if( !systemDir.empty() )
		core.SetSystemDirectory( systemDir );
	if( !saveDir.empty() )
		core.SetSaveDirectory( saveDir );

	// uniqueInstance is off here: this process hosts exactly one core, so there
	// is nothing to collide with and no reason to copy the library.
	if( !core.Load( corePath, error, /*uniqueInstance*/ false ) )
	{
		SetStatus( block, error );
		block->helperBeat.store( shared::NowMillis(), std::memory_order_release );
		std::fprintf( stderr, "core load failed: %s\n", error.c_str() );

		// Breathe for a moment before leaving, so a plugin that is already
		// attached can read the status rather than only seeing the channel
		// vanish.
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		shared::Close( block );
		return 1;
	}

	if( !core.LoadContent( contentPath, error ) )
	{
		SetStatus( block, error );
		block->helperBeat.store( shared::NowMillis(), std::memory_order_release );
		std::fprintf( stderr, "content load failed: %s\n", error.c_str() );
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		shared::Close( block );
		return 1;
	}

	std::strncpy( block->coreName, core.CoreInfo().libraryName.c_str(),
				  sizeof( block->coreName ) - 1 );
	block->fpsMilli.store( uint32_t( core.Fps() * 1000.0 ), std::memory_order_release );
	SetStatus( block, "running" );
	block->coreReady.store( 1, std::memory_order_release );

	runner.Start();

	shared::Writer writer( block );
	uint32_t lastResetSeq = block->resetSeq.load( std::memory_order_acquire );
	uint64_t lastSerial   = 0;

#if defined( __APPLE__ )
	// The publish loop is as latency-sensitive as the emulator thread, and for
	// the same reason: a covered or backgrounded helper window is enough for
	// the scheduler to demote it. See Runner.cpp.
	pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
#endif

	std::printf( "cartridge-helper: %s on channel '%s' (%s), %.4f fps\n",
				 core.CoreInfo().libraryName.c_str(), channel.c_str(),
				 shared::ChannelPath( channel ).c_str(), core.Fps() );

	while( !g_quit.load( std::memory_order_acquire ) )
	{
		const uint64_t now = shared::NowMillis();
		block->helperBeat.store( now, std::memory_order_release );

		// --- control in ----------------------------------------------------
		core.Input().SetPad( 0, uint16_t( block->pad0.load( std::memory_order_relaxed ) ) );
		core.Input().SetPad( 1, uint16_t( block->pad1.load( std::memory_order_relaxed ) ) );

		runner.SetPaused( block->paused.load( std::memory_order_relaxed ) != 0 );

		const uint32_t milli = block->speedMilli.load( std::memory_order_relaxed );
		if( milli > 0 )
			runner.SetSpeed( double( milli ) / 1000.0 );

		const uint32_t seq = block->resetSeq.load( std::memory_order_acquire );
		if( seq != lastResetSeq )
		{
			lastResetSeq = seq;
			// Reset has to happen on the emulator thread -- it is a call into
			// the core, and Core's callback routing assumes one thread at a
			// time. Pausing, resetting and unpausing is the simple way to get
			// there without a command queue.
			const bool wasPaused = runner.IsPaused();
			runner.Stop();
			core.Reset();
			runner.SetPaused( wasPaused );
			runner.Start();
		}

		// --- frames out ----------------------------------------------------
		FrameBuffer& frames = core.Frames();
		frames.Acquire();
		const Frame& f = frames.Current();

		if( f.width > 0 && f.serial != lastSerial )
		{
			if( f.width > shared::kMaxWidth || f.height > shared::kMaxHeight )
			{
				// Refuse rather than crop or scale. See Shared.h: the cap is
				// where the copy stops being free, and silently degrading would
				// hide the reason a show started dropping frames.
				SetStatus( block, "core geometry exceeds the channel maximum" );
				diag::error( "core produced " + std::to_string( f.width ) + "x"
							 + std::to_string( f.height ) + ", channel caps at "
							 + std::to_string( shared::kMaxWidth ) + "x"
							 + std::to_string( shared::kMaxHeight ) );
				break;
			}

			std::memcpy( writer.BeginWrite(), f.pixels.data(), f.byteSize() );
			writer.Publish( f.width, f.height, f.aspectRatio );
			lastSerial = f.serial;
		}

		// Poll at roughly twice the core's rate so a finished frame waits half
		// a frame at worst. Sleeping on a condition variable would be tidier
		// but it would have to live in shared memory, and a lock in shared
		// memory is the thing this design refuses -- see Shared.h.
		const double fps = core.Fps() > 0.0 ? core.Fps() : 60.0;
		std::this_thread::sleep_for(
			std::chrono::microseconds( int64_t( 500000.0 / fps ) ) );
	}

	runner.Stop();
	block->coreReady.store( 0, std::memory_order_release );
	SetStatus( block, "stopped" );

	core.Unload();
	shared::Close( block );

	std::printf( "cartridge-helper: stopped\n" );
	return 0;
}
