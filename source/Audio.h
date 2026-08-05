#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

/**
	Where the emulator's audio goes.

	**FFGL has no audio path.** A Resolume plugin returns a texture and nothing
	else; there is no callback through which a plugin can hand the host a buffer
	of samples. So a plugin that hosts an emulator either throws the audio away
	or opens its own output device, and either way the sound does not travel
	through Resolume's mixer and is not on Resolume's clock.

	This ring is the seam. The core writes into it from the emulator thread
	whatever the frontend does with it afterwards -- discard it, feed a CoreAudio
	unit, or (in the out-of-process build) hand it to the helper's own device.
	Keeping it here rather than in the consumer means the core never blocks on
	whether anyone is listening.

	**Overrun drops the oldest, it does not block.** A ring that blocked the
	emulator thread when nobody was draining it would stall the picture to
	protect audio nobody is playing -- exactly backwards for a video plugin.
	Dropped frames are counted so the log can say so rather than the operator
	guessing.
*/
namespace cartridge
{

class AudioRing
{
public:
	/// Interleaved stereo int16, which is what libretro emits, always.
	struct Sample
	{
		int16_t left  = 0;
		int16_t right = 0;
	};

	explicit AudioRing( size_t capacityFrames = 8192 ) :
		mBuffer( capacityFrames )
	{
	}

	/// Emulator thread. `frames` is sample-pairs, not bytes.
	void Write( const int16_t* interleaved, size_t frames )
	{
		if( interleaved == nullptr || frames == 0 )
			return;

		const size_t cap = mBuffer.size();
		size_t head      = mHead.load( std::memory_order_relaxed );
		const size_t tail = mTail.load( std::memory_order_acquire );

		for( size_t i = 0; i < frames; ++i )
		{
			const size_t next = ( head + 1 ) % cap;
			if( next == tail )
			{
				// Full. Count what we could not fit and stop -- see above for
				// why this does not wait.
				mDropped.fetch_add( frames - i, std::memory_order_relaxed );
				break;
			}
			mBuffer[ head ] = { interleaved[ i * 2 ], interleaved[ i * 2 + 1 ] };
			head            = next;
		}

		mHead.store( head, std::memory_order_release );
	}

	/// Consumer thread. Returns how many frames were actually available.
	size_t Read( Sample* out, size_t frames )
	{
		const size_t cap  = mBuffer.size();
		const size_t head = mHead.load( std::memory_order_acquire );
		size_t tail       = mTail.load( std::memory_order_relaxed );

		size_t n = 0;
		while( n < frames && tail != head )
		{
			out[ n++ ] = mBuffer[ tail ];
			tail       = ( tail + 1 ) % cap;
		}

		mTail.store( tail, std::memory_order_release );
		return n;
	}

	/// Throw away everything buffered -- on content change, or on unpause.
	void Reset()
	{
		mTail.store( mHead.load( std::memory_order_acquire ), std::memory_order_release );
	}

	uint64_t Dropped() const
	{
		return mDropped.load( std::memory_order_relaxed );
	}

private:
	std::vector< Sample > mBuffer;
	std::atomic< size_t > mHead{ 0 };
	std::atomic< size_t > mTail{ 0 };
	std::atomic< uint64_t > mDropped{ 0 };
};

} // namespace cartridge
