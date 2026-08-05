#pragma once

#include "Audio.h"
#include "Frame.h"
#include "Input.h"
#include "Pixels.h"

#include <cstdarg>
#include <string>
#include <vector>

/**
	A libretro frontend, small enough to live inside a plugin.

	Loads a core (a shared library implementing the libretro ABI), hands it
	content, drives `retro_run`, and turns what comes back into RGBA frames in a
	`FrameBuffer`.

	**No core, no BIOS and no content is shipped with this repo, ever.** The
	plugin loads what the operator points it at. That is not only a licensing
	position -- several cores worth using carry non-commercial terms, and
	Genesis Plus GX is one -- it is also the only arrangement under which this
	repo can stay MIT. See AGENTS.md.

	---

	## The two traps that shape this class

	**1. libretro callbacks carry no user pointer.**

	Every callback in the ABI -- video, audio, input, environment -- is a bare
	function pointer with no context argument:

		void retro_set_video_refresh( retro_video_refresh_t );

	There is nowhere to put a `this`. The C API assumes a frontend hosts exactly
	one core, which is true of RetroArch and not true of a plugin that a VJ can
	drop on four layers.

	The routing here is a `thread_local` current-instance pointer, set by an RAII
	guard around *every* entry into the core. That works because each `Core` owns
	exactly one thread and only ever calls into the core from it -- so the
	callback that comes back out is guaranteed to be on that same thread, and the
	thread_local is guaranteed to be ours. Break the one-thread-per-core rule and
	this silently mis-routes.

	**2. Two instances of the same core in one process share state.**

	`dlopen` of a path already loaded returns the *same* handle with a bumped
	refcount -- not a second copy. So two `Core` objects pointed at the same
	`.dylib` get the same globals: the same emulated CPU, the same framebuffer,
	the same everything. The second `retro_init` walks over the first. Symptom is
	two Resolume layers showing the same picture and the game responding to both
	pads at once.

	`RTLD_LOCAL` does not help -- it scopes symbol *visibility*, not the library
	instance.

	The fix is to copy the core to a uniquely-named temporary file and open
	*that*, since dlopen keys on path. `Load` does it automatically when
	`uniqueInstance` is set. It costs a file copy per instance (a few MB) and is
	the only in-process answer that exists. The robust answer is the
	out-of-process build, where each instance is a separate process and the
	question does not arise.
*/
namespace cartridge
{

class Core
{
public:
	Core();
	~Core();

	Core( const Core& )            = delete;
	Core& operator=( const Core& ) = delete;

	/// What the core says about itself, after `Load`.
	struct Info
	{
		std::string libraryName;
		std::string libraryVersion;
		std::string validExtensions; ///< pipe-separated, e.g. "gb|gbc"
		bool needFullpath = false;   ///< core wants a path, not a data blob
		bool blockExtract = false;
		bool supportsNoGame = false; ///< can start with no content at all
	};

	/**
		Open a core.

		`uniqueInstance` copies the library to a private temporary path first, so
		this instance gets its own globals -- see trap 2 above. Leave it off for
		a single-instance frontend (the harness, the helper process) and on for
		the plugin.
	*/
	bool Load( const std::string& corePath, std::string& error, bool uniqueInstance = false );

	/**
		Hand the core its content.

		An empty path starts a core that declared `SET_SUPPORT_NO_GAME`. For
		cores with `needFullpath` the file is not read here -- the core opens it
		itself, which is how large-media cores avoid a copy in RAM.
	*/
	bool LoadContent( const std::string& contentPath, std::string& error );

	/// One emulated frame. Callbacks fire from inside. Emulator thread only.
	void RunFrame();

	/// `retro_reset` -- the console's reset button, not a reload.
	void Reset();

	void Unload();

	bool IsLoaded() const { return mHandle != nullptr; }
	bool HasContent() const { return mContentLoaded; }

	const Info& CoreInfo() const { return mInfo; }

	/// Native frame rate, e.g. 60.0988 for NTSC. Valid after `LoadContent`.
	double Fps() const { return mFps; }
	double SampleRate() const { return mSampleRate; }

	FrameBuffer& Frames() { return mFrames; }
	InputState& Input() { return mInput; }
	AudioRing& Audio() { return mAudio; }

	/// Directories handed to the core for BIOS and saves. Set before `Load`.
	void SetSystemDirectory( const std::string& dir ) { mSystemDir = dir; }
	void SetSaveDirectory( const std::string& dir ) { mSaveDir = dir; }

	/// Lines the core logged through `GET_LOG_INTERFACE`, newest last.
	std::vector< std::string > DrainLog();

private:
	// --- the libretro entry points, bound in Load ---
	struct Api;
	Api* mApi = nullptr;

	void* mHandle = nullptr;
	std::string mTempCopy; ///< non-empty when we made a private copy to delete

	Info mInfo;
	bool mContentLoaded = false;

	double mFps        = 60.0;
	double mSampleRate = 48000.0;

	pixels::Format mFormat = pixels::Format::ORGB1555; // libretro's default

	// Geometry the core declared, used to size the frame buffer up front so a
	// mid-game resolution change is a memcpy into an existing allocation rather
	// than a reallocation on the emulator thread.
	unsigned mMaxWidth  = 0;
	unsigned mMaxHeight = 0;
	float mAspect       = 0.0f;

	FrameBuffer mFrames;
	InputState mInput;
	AudioRing mAudio;

	std::vector< uint8_t > mContentData; ///< kept alive while the core holds it
	std::vector< std::string > mLogLines;

	std::string mSystemDir;
	std::string mSaveDir;

	// --- callback bodies, reached through the thread_local guard ---
	friend struct CoreCallbacks;
	bool OnEnvironment( unsigned cmd, void* data );
	void OnVideo( const void* data, unsigned width, unsigned height, size_t pitch );
	void OnAudioBatch( const int16_t* data, size_t frames );
	int16_t OnInput( unsigned port, unsigned device, unsigned index, unsigned id );
	void OnLog( const char* fmt, va_list args );
};

} // namespace cartridge
