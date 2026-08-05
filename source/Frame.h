#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

/**
	The handoff between the thread running the emulator and the thread drawing
	it.

	**Why there is a buffer here at all.** A libretro core runs at its own rate
	-- 60.0988 Hz for NTSC, 59.7275 for Game Boy, exactly 50 for PAL -- and none
	of those is Resolume's composition rate. Running the core from inside
	`ProcessOpenGL` would tie emulation speed to the host's frame rate, so a
	heavy show would slow the game down: the same failure mode orrery avoids by
	refusing to integrate velocity. So the core gets its own thread at its own
	rate, and the render thread samples whatever the latest finished frame is.

	**Why three slots and not two.** With two, a writer that finishes while the
	reader still holds the other slot has nowhere to go and must either block or
	drop. Blocking the emulator thread on the render thread reintroduces exactly
	the coupling the thread was created to remove. The third slot is what lets
	both sides run freely: the writer always has a slot that is neither being
	read nor pending.

	This is the standard lock-free triple buffer -- writer owns `mWrite`, reader
	owns `mRead`, and `mReady` is the single atomic they trade through. The
	dirty bit distinguishes "there is a new frame" from "you already took it",
	which matters because a core running slower than the host (50 Hz PAL into a
	60 Hz composition) legitimately has nothing new on some frames, and the
	renderer must hold the previous one rather than flash black.

	No mutex anywhere on the hot path. `Publish` and `Acquire` are one
	`exchange` each.
*/
namespace cartridge
{

/// One decoded frame, already converted to the texture format the GPU wants.
struct Frame
{
	/// RGBA8, tightly packed, `width * height * 4` bytes. Sized on first use
	/// and then reused -- a core's geometry can change at runtime (the SNES
	/// switches between 256 and 512 wide mid-game) but not every frame.
	std::vector< uint8_t > pixels;

	unsigned width  = 0;
	unsigned height = 0;

	/// The core's declared pixel aspect. 256x224 on a Mega Drive is not square:
	/// displaying it 1:1 gives a picture that is visibly too tall, so this is
	/// carried through to the renderer rather than assumed to be 1.0.
	float aspectRatio = 0.0f;

	/// Monotonic count of frames the core has produced. The renderer uses it to
	/// tell a genuinely new frame from a repeat, and the harness uses it to
	/// assert determinism.
	uint64_t serial = 0;

	size_t byteSize() const
	{
		return size_t( width ) * size_t( height ) * 4;
	}
};

class FrameBuffer
{
public:
	/**
		Called by the emulator thread. Returns the slot to write into.

		The returned reference stays valid until the next `Publish` from the
		same thread, and no other thread can be looking at it.
	*/
	Frame& BeginWrite()
	{
		return mSlots[ mWrite ];
	}

	/**
		Called by the emulator thread once the slot is filled. The frame becomes
		visible to the reader, and the writer takes over whichever slot was
		previously pending.
	*/
	void Publish()
	{
		// Relaxed is enough for the counter itself: the release on the exchange
		// below is what publishes the pixels, and the counter is stored in the
		// slot the reader only reaches through that exchange.
		const uint64_t serial   = mSerial.fetch_add( 1, std::memory_order_relaxed ) + 1;
		mSlots[ mWrite ].serial = serial;

		const unsigned stale = mReady.exchange( mWrite | kDirty, std::memory_order_acq_rel );
		mWrite               = stale & kIndexMask;
	}

	/**
		Called by the render thread. Returns true if a new frame was taken.

		False means the core has not finished one since the last call -- normal
		for a 50 Hz core in a 60 Hz composition -- and the frame from the
		previous `Acquire` is still valid and still the right thing to draw.
	*/
	bool Acquire()
	{
		if( ( mReady.load( std::memory_order_acquire ) & kDirty ) == 0 )
			return false;

		const unsigned taken = mReady.exchange( mRead, std::memory_order_acq_rel );
		mRead                = taken & kIndexMask;
		return true;
	}

	/**
		The frame most recently taken by `Acquire`. Render-thread only.

		Before the first successful `Acquire` this is a default-constructed slot
		with `width == 0`. Callers must check, rather than assume a frame is
		waiting -- a core that is still loading its content has legitimately
		produced nothing yet.
	*/
	const Frame& Current() const
	{
		return mSlots[ mRead ];
	}

	/// Total frames the core has published, for the harness and the log.
	uint64_t Serial() const
	{
		return mSerial.load( std::memory_order_relaxed );
	}

private:
	// The dirty bit rides in the same word as the index so the whole handoff is
	// one atomic. Splitting them into two atomics would open a window where the
	// reader sees a new index with a stale flag.
	static constexpr unsigned kDirty     = 0x4;
	static constexpr unsigned kIndexMask = 0x3;

	Frame mSlots[ 3 ];

	unsigned mWrite = 0;                 ///< emulator thread only
	unsigned mRead  = 1;                 ///< render thread only
	std::atomic< unsigned > mReady{ 2 }; ///< traded between them

	std::atomic< uint64_t > mSerial{ 0 };
};

} // namespace cartridge
