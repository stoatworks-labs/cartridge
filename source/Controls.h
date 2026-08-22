#pragma once

#include <cmath>

/**
	The host-facing parameters, and the conversions off them.

	**Every ranged parameter is 0..1 and mapped here.** `SetParamInfo` clamps an
	`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
	can only be called afterwards -- there is no `SetParamDefault`. So a default
	speed of 1.0x expressed as a ranged parameter would silently become the top
	of the range. Same trap as orrery, same answer: keep the host in 0..1 and do
	the arithmetic on this side.

	**The twelve pad buttons are the interesting part.** They are plain boolean
	parameters, which means Resolume will MIDI-map them, keyboard-map them, and
	automate them off the timeline like anything else. A hardware controller
	becomes a joypad with no code on our side, and someone who wants to sequence
	a game as a visual can keyframe it.

	Button order follows a SNES-style pad rather than libretro's internal id
	order, because the inspector is read by a person. `ButtonId` is the
	translation, and it is the only place the two orders meet.
*/
namespace cartridge
{

enum ParamId : unsigned
{
	PT_SOURCE = 0, ///< FF_TYPE_OPTION -- in-process, or attach to a helper
	PT_CORE,       ///< FF_TYPE_FILE  -- the libretro core (in-process only)
	PT_CONTENT,    ///< FF_TYPE_FILE  -- the ROM / disc image (in-process only)
	PT_CHANNEL,    ///< FF_TYPE_TEXT  -- which helper to attach to
	PT_RUN,        ///< FF_TYPE_BOOLEAN
	PT_RESET,      ///< FF_TYPE_EVENT -- the console's reset button
	PT_SPEED,      ///< FF_TYPE_STANDARD
	PT_SCALING,    ///< FF_TYPE_OPTION
	PT_PIXEL_ASPECT, ///< FF_TYPE_BOOLEAN
	PT_SMOOTH,     ///< FF_TYPE_BOOLEAN

	// The pad. Order is what a player expects to read, not libretro's ids.
	PT_UP,
	PT_DOWN,
	PT_LEFT,
	PT_RIGHT,
	PT_A,
	PT_B,
	PT_X,
	PT_Y,
	PT_L,
	PT_R,
	PT_START,
	PT_SELECT,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line, then one button per link the block carries:
	// the guide, the project page, the source, the funding page. A button opens
	// a browser and stores nothing.
	//
	// How many buttons there are is decided by which URLs StoatworksAbout.h
	// actually holds, so Plugin.cpp static_asserts this run against
	// `about::kParamCount` -- writing a user guide later adds one, and without
	// the assert that would silently shift PT_COUNT and leave the last button
	// undeclared.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_COUNT
};

constexpr unsigned kFirstButton = PT_UP;
constexpr unsigned kLastButton  = PT_SELECT;

/// libretro's `RETRO_DEVICE_ID_JOYPAD_*` for a pad parameter.
inline unsigned ButtonId( unsigned param )
{
	switch( param )
	{
	case PT_B:      return 0;
	case PT_Y:      return 1;
	case PT_SELECT: return 2;
	case PT_START:  return 3;
	case PT_UP:     return 4;
	case PT_DOWN:   return 5;
	case PT_LEFT:   return 6;
	case PT_RIGHT:  return 7;
	case PT_A:      return 8;
	case PT_X:      return 9;
	case PT_L:      return 10;
	case PT_R:      return 11;
	default:        return 0;
	}
}

inline const char* ButtonName( unsigned param )
{
	switch( param )
	{
	case PT_UP:     return "Up";
	case PT_DOWN:   return "Down";
	case PT_LEFT:   return "Left";
	case PT_RIGHT:  return "Right";
	case PT_A:      return "A";
	case PT_B:      return "B";
	case PT_X:      return "X";
	case PT_Y:      return "Y";
	case PT_L:      return "L";
	case PT_R:      return "R";
	case PT_START:  return "Start";
	case PT_SELECT: return "Select";
	default:        return "";
	}
}

/**
	Speed multiplier from the 0..1 slider.

	Exponential and centred so the middle of the travel is exactly 1.0x -- the
	console's own rate, which is the value anyone actually wants and the one a
	linear map would put in an arbitrary place. Quarter speed at 0, 4x at 1.
*/
inline double SpeedFromParam( float v )
{
	return std::pow( 2.0, ( double( v ) - 0.5 ) * 4.0 );
}

inline float SpeedToParam( double speed )
{
	return float( std::log2( speed ) / 4.0 + 0.5 );
}

/**
	Where the pixels come from.

	The two builds are the same frontend differing only in which process the
	emulator runs in, and this is the switch between them. It is the first
	parameter because it is the first decision: everything below it means
	something slightly different depending on the answer.

	- **In Process** is lower latency -- the frame is drawn the same composition
	  frame it was produced -- and a core that crashes takes Resolume with it.
	- **Helper** costs one frame and a copy, and survives a core crash. It is
	  also the only way to run two instances of a core that does not tolerate
	  the private-copy trick.
*/
enum class Source
{
	InProcess = 0,
	Helper
};

inline Source SourceFromParam( float v )
{
	return int( v + 0.5f ) >= 1 ? Source::Helper : Source::InProcess;
}

/// How the console's picture is fitted into the composition.
enum class Scaling
{
	Fit = 0,   ///< whole picture, letterboxed
	Fill,      ///< fills the frame, crops the overflow
	Stretch,   ///< ignores aspect entirely
	Integer    ///< largest whole-number pixel multiple that fits, centred
};

inline Scaling ScalingFromParam( float v )
{
	// Option parameters hold the ELEMENT value the operator chose -- 0, 1, 2 --
	// not a 0..1 fraction. Rounded and clamped because a composition saved
	// against a build with more elements than this one must not index off the
	// end.
	const int i = int( v + 0.5f );
	if( i <= 0 )
		return Scaling::Fit;
	if( i >= 3 )
		return Scaling::Integer;
	return Scaling( i );
}

} // namespace cartridge
