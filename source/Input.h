#pragma once

#include <atomic>
#include <cstdint>

/**
	Controller state, written by whoever is driving and read by the emulator
	thread.

	**The whole pad is one 16-bit word, updated atomically.** Not sixteen
	separate flags. A core polls every button inside a single `retro_run`, so if
	the word changed halfway through the poll it would see a state that never
	existed -- a diagonal that was never pressed, or Start without Select on a
	deliberate Start+Select reset combo. One `store` and one `load` per frame
	makes the pad state a snapshot by construction.

	Bit positions are libretro's own `RETRO_DEVICE_ID_JOYPAD_*` values, so
	`Poll` is a shift and a mask with no translation table to get out of step.

	Two ports. Not because two-player is a headline feature, but because several
	cores read port 1 during boot and a frontend that only answers for port 0
	makes them wait on a controller that never arrives.
*/
namespace cartridge
{

class InputState
{
public:
	static constexpr unsigned kPorts   = 2;
	static constexpr unsigned kButtons = 16;

	/// Set or clear one button. Safe from any thread.
	void SetButton( unsigned port, unsigned id, bool down )
	{
		if( port >= kPorts || id >= kButtons )
			return;

		const uint16_t bit = uint16_t( 1u << id );
		if( down )
			mPads[ port ].fetch_or( bit, std::memory_order_relaxed );
		else
			mPads[ port ].fetch_and( uint16_t( ~bit ), std::memory_order_relaxed );
	}

	/// Replace a whole port at once -- the path a real gamepad should use.
	void SetPad( unsigned port, uint16_t bits )
	{
		if( port < kPorts )
			mPads[ port ].store( bits, std::memory_order_relaxed );
	}

	/// Emulator thread. `id == RETRO_DEVICE_ID_JOYPAD_MASK` wants the word.
	int16_t Poll( unsigned port, unsigned id ) const
	{
		if( port >= kPorts )
			return 0;

		const uint16_t bits = mPads[ port ].load( std::memory_order_relaxed );

		if( id == 256 ) // RETRO_DEVICE_ID_JOYPAD_MASK
			return int16_t( bits );

		if( id >= kButtons )
			return 0;

		return ( bits >> id ) & 1;
	}

	void Clear()
	{
		for( auto& p : mPads )
			p.store( 0, std::memory_order_relaxed );
	}

private:
	std::atomic< uint16_t > mPads[ kPorts ] = {};
};

} // namespace cartridge
