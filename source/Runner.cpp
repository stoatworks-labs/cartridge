#include "Runner.h"

#include "Diag.h"

#include <algorithm>

#if defined( __APPLE__ )
	#include <pthread.h>
#endif

namespace cartridge
{

void Runner::Start()
{
	if( mRunning.load( std::memory_order_acquire ) )
		return;

	mQuit.store( false, std::memory_order_release );
	mRunning.store( true, std::memory_order_release );
	mThread = std::thread( &Runner::ThreadMain, this );
}

void Runner::Stop()
{
	if( !mRunning.load( std::memory_order_acquire ) )
		return;

	mQuit.store( true, std::memory_order_release );
	if( mThread.joinable() )
		mThread.join();
	mRunning.store( false, std::memory_order_release );
}

void Runner::StepSynchronous( unsigned count )
{
	// One thread per core, always -- see Core's header. Running the thread and
	// stepping by hand at the same time would put two threads through the
	// thread_local callback routing and silently mis-route half the frames.
	if( mRunning.load( std::memory_order_acquire ) )
	{
		diag::error( "StepSynchronous called while the runner thread is live; ignored" );
		return;
	}

	for( unsigned i = 0; i < count; ++i )
		mCore.RunFrame();
}

void Runner::SetSpeed( double multiplier )
{
	// Zero would divide by zero in the period; negative would run the clock
	// backwards. Pause is a separate control and is the honest way to stop.
	mSpeed.store( std::clamp( multiplier, 0.05, 8.0 ), std::memory_order_relaxed );
}

void Runner::ThreadMain()
{
#if defined( __APPLE__ )
	// See the header: without this, a covered Resolume window is enough for the
	// scheduler to decide this thread is background work.
	pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
#endif

	using clock    = std::chrono::steady_clock;
	using duration = std::chrono::duration< double >;

	auto next = clock::now();

	while( !mQuit.load( std::memory_order_acquire ) )
	{
		if( mPaused.load( std::memory_order_relaxed ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 8 ) );
			next = clock::now(); // do not accrue debt while deliberately stopped
			continue;
		}

		const double fps = mCore.Fps() * mSpeed.load( std::memory_order_relaxed );
		const duration period( fps > 0.0 ? 1.0 / fps : 1.0 / 60.0 );

		mCore.RunFrame();

		next += std::chrono::duration_cast< clock::duration >( period );

		const auto now = clock::now();
		if( now < next )
		{
			std::this_thread::sleep_until( next );
		}
		else
		{
			// Behind. Allow a few frames of catch-up by simply not sleeping,
			// then give up on the debt rather than spiral -- see the header.
			const double behind = duration( now - next ).count() / period.count();
			if( behind > kMaxCatchUp )
			{
				mUnderruns.fetch_add( uint64_t( behind ), std::memory_order_relaxed );
				next = now;
			}
		}
	}
}

} // namespace cartridge
