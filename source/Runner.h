#pragma once

#include "Core.h"

#include <atomic>
#include <chrono>
#include <thread>

/**
	Drives a `Core` at the console's own frame rate, on its own thread.

	**Why not just call `RunFrame` from the renderer.** Because then the
	emulator's speed *is* the host's frame rate. Resolume's composition rate
	drops when the show gets heavy, and a Mega Drive that slows down when the
	projection load goes up is the same class of failure orrery avoids by
	refusing to integrate velocity -- except here it changes the pitch of the
	music as well. The console's rate and the composition's rate are independent
	facts and this class is the seam between them.

	It also makes 50 Hz content work at all. A PAL core in a 60 Hz composition
	must produce five frames for every six the renderer draws; driven from
	`ProcessOpenGL` it would instead run 20% fast.

	**Catch-up is capped.** If the thread is starved -- the machine stalls, a
	core hitches on a disc seek -- the naive accumulator wants to run every
	missed frame at once to "catch up", which takes even longer and digs in
	deeper. Past `kMaxCatchUp` frames the debt is written off and the clock
	resets. A moment of slow motion is recoverable; a spiral is not.

	**App Nap is a real hazard here.** macOS demoted a worker thread from 50 to 7
	fps in another fleet repo simply because the window was covered, and a
	dedicated thread was not enough on its own. The thread asks for
	`QOS_CLASS_USER_INTERACTIVE` at entry for that reason.
*/
namespace cartridge
{

class Runner
{
public:
	Runner()  = default;
	~Runner() { Stop(); }

	Runner( const Runner& )            = delete;
	Runner& operator=( const Runner& ) = delete;

	Core& GetCore() { return mCore; }
	const Core& GetCore() const { return mCore; }

	/// Begin free-running. Safe to call when already running.
	void Start();

	/// Stop and join. Safe to call when not running.
	void Stop();

	bool IsRunning() const { return mRunning.load( std::memory_order_acquire ); }

	/**
		Run `count` frames on the calling thread, with no timing and no thread.

		This is the harness's entry point, and the reason the harness is
		deterministic: no sleeping, no wall clock, no scheduler. Must not be
		called while the thread is running -- one thread per core, or the
		thread_local callback routing in `Core` mis-routes.
	*/
	void StepSynchronous( unsigned count );

	/// Paused stops `retro_run` but keeps the thread and the last frame alive.
	void SetPaused( bool paused ) { mPaused.store( paused, std::memory_order_relaxed ); }
	bool IsPaused() const { return mPaused.load( std::memory_order_relaxed ); }

	/// 1.0 is the console's own rate. Clamped to something sane on the way in.
	void SetSpeed( double multiplier );
	double Speed() const { return mSpeed.load( std::memory_order_relaxed ); }

	/// Frames the runner asked for but could not fit in real time.
	uint64_t Underruns() const { return mUnderruns.load( std::memory_order_relaxed ); }

private:
	void ThreadMain();

	static constexpr int kMaxCatchUp = 4;

	Core mCore;
	std::thread mThread;

	std::atomic< bool > mRunning{ false };
	std::atomic< bool > mQuit{ false };
	std::atomic< bool > mPaused{ false };
	std::atomic< double > mSpeed{ 1.0 };
	std::atomic< uint64_t > mUnderruns{ 0 };
};

} // namespace cartridge
