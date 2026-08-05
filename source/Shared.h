#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

/**
	The channel between the helper process and whoever is displaying it.

	This is the out-of-process build's whole reason for existing: the emulator
	runs in a **separate process**, so a core that segfaults takes down a helper
	nobody was looking at instead of taking down Resolume in the middle of a
	show. The plugin notices the helper stopped breathing and holds the last
	frame.

	---

	## Why shared memory and a copy, rather than a shared GPU texture

	The obvious "fast" answer is an IOSurface (or a Spout/DXGI shared texture)
	so the pixels never leave the GPU. That is the right answer for HD video and
	it is the wrong answer here, for a reason specific to this content:

	A console frame is **320x224x4 = 286 KB**. Copying it is on the order of
	20 microseconds -- against a 16.7 ms frame budget, that is a tenth of one
	percent. In exchange for that tenth of a percent the transport becomes a
	flat byte layout that works the same on macOS and Windows, needs no
	framework, no mach ports, no device-context negotiation, and can be tested
	by two processes with no GPU at all.

	**Where that stops being true**: past roughly 1280x960 the copy starts to
	matter, and a core running at 4K internally would want the zero-copy path.
	`kMaxWidth`/`kMaxHeight` cap the channel at exactly that point rather than
	silently degrading -- a core that asks for more is refused with a message,
	which is a better outcome than a show that mysteriously drops frames.

	## Layout rules

	Everything here is POD with a fixed layout, mapped into two processes that
	were built together. Three things follow and all three are load-bearing:

	- **`magic` and `version` are checked before anything else is read.** A
	  stale segment left behind by an older build has the same name and a
	  different layout, and reading it as though it were current is the kind of
	  bug that presents as random pixels.
	- **Every cross-process field is a lock-free atomic.** A mutex in shared
	  memory that a crashing helper holds is a plugin that hangs Resolume
	  forever -- exactly the failure this design exists to prevent. There are no
	  locks here at all.
	- **The frame ring is the same triple buffer as `FrameBuffer`**, for the
	  same reasons, but written out longhand as indices into a flat array
	  because a `std::vector` cannot live in shared memory.
*/
namespace cartridge::shared
{

constexpr uint32_t kMagic   = 0x43415254; // 'CART'
constexpr uint32_t kVersion = 1;

/// See the note above on why the cap is here and not higher.
constexpr uint32_t kMaxWidth  = 1280;
constexpr uint32_t kMaxHeight = 960;
constexpr size_t kSlotBytes   = size_t( kMaxWidth ) * kMaxHeight * 4;
constexpr unsigned kSlots     = 3;

struct SlotHeader
{
	uint32_t width  = 0;
	uint32_t height = 0;
	float aspect    = 0.0f;
	uint64_t serial = 0;
};

struct Block
{
	uint32_t magic;
	uint32_t version;

	// --- client -> helper -------------------------------------------------
	std::atomic< uint32_t > pad0;    ///< 16-bit joypad word for port 0
	std::atomic< uint32_t > pad1;    ///< ... and port 1
	std::atomic< uint32_t > paused;
	std::atomic< uint32_t > resetSeq;///< helper resets when this changes
	std::atomic< uint32_t > speedMilli; ///< speed x1000, so the field stays integral
	std::atomic< uint64_t > clientBeat;

	// --- helper -> client -------------------------------------------------
	std::atomic< uint64_t > helperBeat;
	std::atomic< uint32_t > coreReady; ///< 1 once content is loaded and running
	std::atomic< uint32_t > fpsMilli;  ///< the core's native rate x1000
	char coreName[ 64 ];
	char status[ 128 ];                ///< last error, for the plugin to log

	// --- the frame ring ---------------------------------------------------
	std::atomic< uint32_t > ready;     ///< index | kDirty, as in FrameBuffer
	SlotHeader headers[ kSlots ];

	// Pixels last, so everything above stays on the first page and a status
	// poll does not fault in twelve megabytes of framebuffer.
	uint8_t pixels[ kSlots ][ kSlotBytes ];
};

static_assert( std::atomic< uint32_t >::is_always_lock_free,
			   "a non-lock-free atomic in shared memory would take a lock the other "
			   "process cannot see, and a crashed helper would hang the host" );
static_assert( std::atomic< uint64_t >::is_always_lock_free,
			   "see above -- 64-bit atomics must be lock-free too" );

constexpr uint32_t kDirty     = 0x4;
constexpr uint32_t kIndexMask = 0x3;

/**
	The writer half. Lives in the helper.

	Deliberately not a class with state: the helper's write index is the only
	thing that needs remembering, and keeping it in the caller makes it obvious
	that it is process-local and not shared.
*/
class Writer
{
public:
	explicit Writer( Block* block ) :
		mBlock( block )
	{
	}

	uint8_t* BeginWrite() { return mBlock->pixels[ mWrite ]; }

	void Publish( uint32_t width, uint32_t height, float aspect )
	{
		SlotHeader& h = mBlock->headers[ mWrite ];
		h.width       = width;
		h.height      = height;
		h.aspect      = aspect;
		h.serial      = ++mSerial;

		const uint32_t stale =
			mBlock->ready.exchange( mWrite | kDirty, std::memory_order_acq_rel );
		mWrite = stale & kIndexMask;
	}

private:
	Block* mBlock    = nullptr;
	uint32_t mWrite  = 0;
	uint64_t mSerial = 0;
};

/// The reader half. Lives in the plugin.
class Reader
{
public:
	explicit Reader( Block* block ) :
		mBlock( block )
	{
	}

	/// True if a new frame was taken. False means hold the previous one.
	bool Acquire()
	{
		if( ( mBlock->ready.load( std::memory_order_acquire ) & kDirty ) == 0 )
			return false;

		const uint32_t taken = mBlock->ready.exchange( mRead, std::memory_order_acq_rel );
		mRead                = taken & kIndexMask;
		return true;
	}

	const SlotHeader& Header() const { return mBlock->headers[ mRead ]; }
	const uint8_t* Pixels() const { return mBlock->pixels[ mRead ]; }

private:
	Block* mBlock   = nullptr;
	uint32_t mRead  = 1;
};

/**
	Create or open the shared segment.

	`name` is a short channel name, not a path. **macOS caps a POSIX shared
	memory name at 31 characters including the leading slash**, and over-long
	names fail with ENAMETOOLONG rather than being truncated -- so the name is
	built and length-checked here rather than by each caller.

	`create` is the helper; the plugin opens without it and gets nullptr until a
	helper exists, which is the correct behaviour for a composition loaded
	before the helper is started.
*/
Block* Open( const std::string& name, bool create, std::string& error );

void Close( Block* block );

/// The OS-level name for a channel, for diagnostics and for `Open`.
std::string ChannelPath( const std::string& name );

/// Milliseconds since an arbitrary epoch, for the heartbeats.
uint64_t NowMillis();

/// A heartbeat older than this means the far side is gone.
constexpr uint64_t kBeatTimeoutMs = 2000;

} // namespace cartridge::shared
