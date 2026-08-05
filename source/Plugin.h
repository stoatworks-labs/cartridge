#pragma once

#include "Controls.h"
#include "Runner.h"
#include "Shared.h"

#include <FFGLSDK.h>

#include <string>

/**
	The in-process build: a libretro core running inside Resolume, as an FFGL
	source.

	This is the low-latency half of the pair. The emulator thread publishes a
	frame, `ProcessOpenGL` uploads it and draws it, and nothing leaves the
	process -- so the picture is on screen the same frame it was produced.

	**What it costs, stated plainly: a core that crashes takes Resolume with
	it.** An emulator is a large body of C doing unusual things to memory, and
	some cores are more finished than others. That is the trade the
	out-of-process build exists to refuse, and it is why the helper is not an
	afterthought. Read AGENTS.md before deciding which one a show should use.

	## Traps this class is shaped by

	**`ffglex::Scoped*` bindings clear to 0 on scope exit rather than restoring
	what was there.** So the render path uses plain `glUseProgram` and
	`glBindTexture` and puts the state back by hand, exactly as orrery does.

	**No FBO is allocated anywhere.** `FFGLFBO::Initialise` allocates under a
	`ScopedTextureBinding` whose destructor clears the binding, which silently
	unbinds a texture for the frames on which it allocates; and
	`FFGLFBO::Release` leaks its colour texture. Nothing here needs one -- a
	source draws a textured quad and that is all.

	**The texture is allocated once at the core's declared maximum geometry.**
	Cores change resolution mid-game -- the SNES switching to hi-res, a handheld
	rotating -- and reallocating a texture from inside the video callback would
	both allocate on the emulator thread and thrash the driver. The used region
	is addressed with UV scaling instead.

	**Nearest filtering is the default and it matters.** A 256-pixel-wide
	picture scaled to a 4K output with linear filtering is a blurred mess; the
	whole point of this content is that the pixels are large and hard-edged. See
	`PT_SMOOTH` for when to turn it off.
*/
namespace cartridge
{

class CartridgePlugin : public CFFGLPlugin
{
public:
	CartridgePlugin();
	~CartridgePlugin() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

private:
	bool BuildShader();

	/// Apply a pending core/content change. Called from the render thread.
	void ApplyPendingLoad();

	/// Attach to, or detach from, a helper channel. Render thread.
	void ApplyPendingAttach();

	/// The twelve button parameters as one 16-bit joypad word.
	uint16_t PadWord() const;

	/// Upload the newest frame if there is one. Returns false if nothing to draw.
	bool UpdateTextureInProcess();
	bool UpdateTextureFromHelper();

	/// Common tail of both: bind, size and filter the texture. Returns the
	/// destination the caller should upload into, or false if nothing to do.
	bool PrepareTexture( unsigned width, unsigned height, float aspect );

	void ComputeQuadScale( int vpWidth, int vpHeight, float& sx, float& sy ) const;

	Runner mRunner;

	// Paths as the host gave them. Compared against what is loaded so that a
	// host re-sending the same value -- which Resolume does on composition load
	// and on undo -- does not tear the emulator down and build it again.
	std::string mCorePath;
	std::string mContentPath;
	std::string mLoadedCorePath;
	std::string mLoadedContentPath;
	bool mPendingLoad = false;

	/// Set when a load failed, cleared when the paths change. Stops the render
	/// thread retrying a broken core sixty times a second and filling the log.
	bool mLoadFailed = false;

	// --- helper mode -------------------------------------------------------
	std::string mChannel        = "default";
	std::string mAttachedChannel;
	shared::Block* mChannelBlock = nullptr;
	shared::Reader mReader{ nullptr };
	bool mPendingAttach = false;

	/// Throttles the reattach attempt so a composition saved against a helper
	/// that is not running does not try to open a channel every frame.
	uint64_t mNextAttachAttempt = 0;

	/// Tracks whether the helper was alive last frame, so the log records the
	/// transition once rather than every frame.
	bool mHelperWasAlive = false;

	/// Bumped to ask the helper to reset. The helper watches for a change
	/// rather than a value, so the plugin never has to clear it.
	uint32_t mResetSeq = 0;

	float mParams[ PT_COUNT ] = {};

	// --- GL ---
	ffglex::FFGLShader mShader;
	GLuint mVAO     = 0;
	GLuint mTexture = 0;

	unsigned mTextureWidth  = 0;
	unsigned mTextureHeight = 0;

	/// Serial of the frame currently in `mTexture`, so a repeat is not re-uploaded.
	uint64_t mUploadedSerial = 0;

	/// Size of the picture inside the texture, for the UV scale.
	unsigned mFrameWidth  = 0;
	unsigned mFrameHeight = 0;
	float mFrameAspect    = 1.0f;

	FFGLViewportStruct mViewport = {};
};

} // namespace cartridge
