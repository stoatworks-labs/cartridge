#include "Plugin.h"

#include "Diag.h"
#include "Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cartridge
{

namespace
{

// The picture is one quad built from gl_VertexID -- no vertex buffer, same as
// the rest of the fleet's shaders. `Scale` letterboxes it; `UvScale` addresses
// the used corner of a texture allocated at the core's maximum geometry.
//
// Note the names: `filter`, `active`, `flat`, `input`, `output`, `sample` and
// `common` are GLSL reserved words, and a shader that fails to compile surfaces
// only at runtime as "the plugin does nothing". That is what Diag is for.
const char* kVertexShader = R"(#version 410 core
uniform vec2 Scale;
uniform vec2 UvScale;

out vec2 vUv;

void main()
{
	// 0,1,2,3 -> the four corners of a triangle strip.
	vec2 corner = vec2( float( gl_VertexID & 1 ), float( ( gl_VertexID >> 1 ) & 1 ) );

	vUv         = corner * UvScale;
	gl_Position = vec4( ( corner * 2.0 - 1.0 ) * Scale, 0.0, 1.0 );
}
)";

const char* kFragmentShader = R"(#version 410 core
uniform sampler2D Picture;

in vec2 vUv;
out vec4 fragColour;

void main()
{
	fragColour = vec4( texture( Picture, vUv ).rgb, 1.0 );
}
)";

} // namespace

// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

CartridgePlugin::CartridgePlugin()
{
	diag::init();

	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	// --- where the pixels come from ----------------------------------------
	// First, because it is the first decision -- see Controls.h. Defaults to
	// In Process: it is the one that works with no second application running,
	// which is what someone trying the plugin for the first time will do.
	SetOptionParamInfo( PT_SOURCE, "Source", 2, 0.0f );
	SetParamElementInfo( PT_SOURCE, 0, "In Process", 0.0f );
	SetParamElementInfo( PT_SOURCE, 1, "Helper", 1.0f );

	SetParamInfo( PT_CHANNEL, "Channel", FF_TYPE_TEXT, "default" );

	// --- content -----------------------------------------------------------
	// FF_TYPE_FILE gives the operator a real file picker. The extension list
	// for the core is the platform's shared-library suffix; for content it is
	// deliberately broad, because which extensions are valid depends on the
	// core that has not been chosen yet.
	SetFileParamInfo( PT_CORE, "Core",
#if defined( _WIN32 )
					  { "dll" },
#elif defined( __APPLE__ )
					  { "dylib" },
#else
					  { "so" },
#endif
					  "" );

	SetFileParamInfo( PT_CONTENT, "Content",
					  { "nes", "sfc", "smc", "gb", "gbc", "gba", "md", "gen", "smd",
						"sms", "gg", "pce", "z64", "n64", "iso", "cue", "chd", "zip", "bin" },
					  "" );

	SetParamInfo( PT_RUN, "Run", FF_TYPE_BOOLEAN, true );
	mParams[ PT_RUN ] = 1.0f;

	SetParamInfo( PT_RESET, "Reset", FF_TYPE_EVENT, false );

	// 0..1 on the host side, mapped in Controls.h -- see the note there about
	// SetParamInfo clamping a ranged default.
	SetParamInfo( PT_SPEED, "Speed", FF_TYPE_STANDARD, 0.5f );
	mParams[ PT_SPEED ] = 0.5f;

	SetOptionParamInfo( PT_SCALING, "Scaling", 4, 0.0f );
	SetParamElementInfo( PT_SCALING, 0, "Fit", 0.0f );
	SetParamElementInfo( PT_SCALING, 1, "Fill", 1.0f );
	SetParamElementInfo( PT_SCALING, 2, "Stretch", 2.0f );
	SetParamElementInfo( PT_SCALING, 3, "Integer", 3.0f );

	SetParamInfo( PT_PIXEL_ASPECT, "Pixel Aspect", FF_TYPE_BOOLEAN, true );
	mParams[ PT_PIXEL_ASPECT ] = 1.0f;

	// Off by default. See the header: linear filtering on 256-pixel-wide
	// content is the single fastest way to throw away what makes it look like
	// what it is.
	SetParamInfo( PT_SMOOTH, "Smoothing", FF_TYPE_BOOLEAN, false );

	// --- the pad -----------------------------------------------------------
	for( unsigned p = kFirstButton; p <= kLastButton; ++p )
	{
		SetParamInfo( p, ButtonName( p ), FF_TYPE_BOOLEAN, false );
		SetParamGroup( p, "Controller" );
	}

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

CartridgePlugin::~CartridgePlugin()
{
	// Stop the emulator thread before anything else is torn down. It is holding
	// a pointer to the Core and will keep calling into it otherwise.
	mRunner.Stop();

	if( mChannelBlock )
	{
		shared::Close( mChannelBlock );
		mChannelBlock = nullptr;
	}
}

// ---------------------------------------------------------------------------
// GL lifecycle
// ---------------------------------------------------------------------------

bool CartridgePlugin::BuildShader()
{
	if( !mShader.Compile( kVertexShader, kFragmentShader ) )
	{
		const GLubyte* vendor   = glGetString( GL_VENDOR );
		const GLubyte* renderer = glGetString( GL_RENDERER );
		const GLubyte* version  = glGetString( GL_VERSION );

		// The GL strings go next to the failure because a shader that builds on
		// one machine and not another is a driver answer, not a source answer.
		diag::error( std::string( "shader compile failed on " )
					 + ( vendor ? (const char*)vendor : "?" ) + " / "
					 + ( renderer ? (const char*)renderer : "?" ) + " / "
					 + ( version ? (const char*)version : "?" ) );
		return false;
	}
	return true;
}

FFResult CartridgePlugin::InitGL( const FFGLViewportStruct* vp )
{
	const GLubyte* version = glGetString( GL_VERSION );
	diag::info( std::string( "InitGL, GL " )
				+ ( version ? reinterpret_cast< const char* >( version ) : "unknown" ) );

	if( !BuildShader() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	// A core profile refuses to draw with no vertex array bound, even though
	// the shader builds its geometry from gl_VertexID and sources nothing.
	glGenVertexArrays( 1, &mVAO );

	glGenTextures( 1, &mTexture );

	mViewport = *vp;
	return FF_SUCCESS;
}

FFResult CartridgePlugin::DeInitGL()
{
	mRunner.Stop();

	mShader.FreeGLResources();

	if( mVAO != 0 )
	{
		glDeleteVertexArrays( 1, &mVAO );
		mVAO = 0;
	}
	if( mTexture != 0 )
	{
		glDeleteTextures( 1, &mTexture );
		mTexture = 0;
	}

	mTextureWidth = mTextureHeight = 0;
	mUploadedSerial               = 0;

	return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void CartridgePlugin::ApplyPendingLoad()
{
	mPendingLoad = false;

	if( mCorePath.empty() )
	{
		mRunner.Stop();
		mRunner.GetCore().Unload();
		mLoadedCorePath.clear();
		mLoadedContentPath.clear();
		return;
	}

	// The emulator thread must be down before the core is touched: it is inside
	// retro_run, and unloading the library underneath it is a use-after-free
	// in somebody else's process.
	mRunner.Stop();

	std::string error;
	Core& core = mRunner.GetCore();

	// Before Load, and before LoadContent: a core handed a null system directory
	// does not degrade gracefully — fceumm and Genesis Plus GX segfault inside
	// retro_load_game, which in this build means segfaulting Resolume. cartest
	// and the helper have taken --system/--save for a while; the plugin, which
	// is the one that runs inside the host, was still answering
	// GET_SYSTEM_DIRECTORY with a null pointer. See source/Paths.h.
	{
		const auto systemDir = paths::SystemDirectory();
		const auto saveDir   = paths::SaveDirectory();

		if( paths::EnsureDirectory( systemDir ) )
			core.SetSystemDirectory( systemDir );
		else
			diag::error( "could not create the system directory: " + systemDir );

		if( paths::EnsureDirectory( saveDir ) )
			core.SetSaveDirectory( saveDir );
		else
			diag::error( "could not create the save directory: " + saveDir );
	}

	if( mCorePath != mLoadedCorePath )
	{
		core.Unload();
		mLoadedCorePath.clear();
		mLoadedContentPath.clear();

		// uniqueInstance so two Cartridge layers in one composition get two
		// emulators rather than two views of one. See the trap in Core.h.
		if( !core.Load( mCorePath, error, /*uniqueInstance*/ true ) )
		{
			diag::error( "core load failed: " + error );
			mLoadFailed = true;
			return;
		}
		mLoadedCorePath = mCorePath;
	}

	if( mContentPath != mLoadedContentPath || !core.HasContent() )
	{
		if( !core.LoadContent( mContentPath, error ) )
		{
			diag::error( "content load failed: " + error );
			mLoadFailed = true;
			return;
		}
		mLoadedContentPath = mContentPath;
	}

	mLoadFailed     = false;
	mUploadedSerial = 0;

	mRunner.SetSpeed( SpeedFromParam( mParams[ PT_SPEED ] ) );
	mRunner.SetPaused( mParams[ PT_RUN ] < 0.5f );
	mRunner.Start();

	diag::info( "running " + core.CoreInfo().libraryName
				+ ( mContentPath.empty() ? " with no content" : " with " + mContentPath ) );
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

FFResult CartridgePlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	mParams[ index ] = value;

	switch( index )
	{
	case PT_SOURCE:
		// Switching mode tears down whichever side is running. Both are
		// deliberately not kept warm: a helper-mode layer that still had an
		// emulator thread running in process would be paying for two.
		mPendingAttach = true;
		if( SourceFromParam( value ) == Source::Helper )
		{
			mRunner.Stop();
		}
		else
		{
			mPendingLoad = true;
			mLoadFailed  = false;
		}
		mUploadedSerial = 0;
		break;

	case PT_SPEED:
		mRunner.SetSpeed( SpeedFromParam( value ) );
		break;

	case PT_RUN:
		mRunner.SetPaused( value < 0.5f );
		break;

	case PT_RESET:
		// An EVENT parameter arrives as a rising edge. Reset on the way up
		// only, or holding the button down resets every frame and the game
		// never gets past its boot logo.
		if( value >= 0.5f )
		{
			if( SourceFromParam( mParams[ PT_SOURCE ] ) == Source::Helper )
			{
				// The helper watches for a *change*, so the plugin never has to
				// clear this and a dropped write cannot leave the core resetting
				// forever.
				++mResetSeq;
				if( mChannelBlock )
					mChannelBlock->resetSeq.store( mResetSeq, std::memory_order_release );
			}
			else
			{
				// NOT GetCore().Reset(): this runs on the host's parameter
				// thread while the runner thread is inside retro_run. The
				// runner performs it between frames instead — see
				// Runner::RequestReset.
				mRunner.RequestReset();
			}
		}
		break;

	default:
		break;
	}

	return FF_SUCCESS;
}

float CartridgePlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? mParams[ index ] : 0.0f;
}

FFResult CartridgePlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Display-only, and it MUST still succeed: instantiateGL pushes every
	// declared default back through the setters on a fresh instance and deletes
	// the instance if one fails, so failing here means no real host can load
	// the plugin -- while every offline harness here carries on passing.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	// An unhandled TEXT or FILE parameter is not a no-op -- it is the trap that
	// kills the plugin on an instantiate sweep. Both are handled here and
	// anything else is explicitly refused rather than falling through.
	const std::string v = value ? value : "";

	if( index == PT_CORE )
	{
		if( v != mCorePath )
		{
			mCorePath    = v;
			mPendingLoad = true;
			mLoadFailed  = false;
		}
		return FF_SUCCESS;
	}

	if( index == PT_CONTENT )
	{
		if( v != mContentPath )
		{
			mContentPath = v;
			mPendingLoad = true;
			mLoadFailed  = false;
		}
		return FF_SUCCESS;
	}

	if( index == PT_CHANNEL )
	{
		if( v != mChannel )
		{
			mChannel           = v;
			mPendingAttach     = true;
			mNextAttachAttempt = 0;
		}
		return FF_SUCCESS;
	}

	return FF_FAIL;
}

char* CartridgePlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call. Answered before the
		// lock below -- it shares no state with the text parameters.
		static const std::string aboutLine = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutLine.c_str() );
	}

	if( index == PT_CORE )
		return const_cast< char* >( mCorePath.c_str() );
	if( index == PT_CONTENT )
		return const_cast< char* >( mContentPath.c_str() );
	if( index == PT_CHANNEL )
		return const_cast< char* >( mChannel.c_str() );
	return const_cast< char* >( "" );
}

uint16_t CartridgePlugin::PadWord() const
{
	// One word, assembled here and stored once -- see Input.h for why this is
	// not sixteen separate writes.
	uint16_t bits = 0;
	for( unsigned p = kFirstButton; p <= kLastButton; ++p )
		if( mParams[ p ] >= 0.5f )
			bits |= uint16_t( 1u << ButtonId( p ) );
	return bits;
}

// ---------------------------------------------------------------------------
// Helper mode
// ---------------------------------------------------------------------------

void CartridgePlugin::ApplyPendingAttach()
{
	mPendingAttach = false;

	if( mChannelBlock )
	{
		shared::Close( mChannelBlock );
		mChannelBlock = nullptr;
		mReader       = shared::Reader( nullptr );
		mAttachedChannel.clear();
		mHelperWasAlive = false;
	}

	if( SourceFromParam( mParams[ PT_SOURCE ] ) != Source::Helper || mChannel.empty() )
		return;

	std::string error;
	// create = false: the plugin never creates a channel. A helper that is not
	// running is a normal state -- the composition may load first -- and
	// creating the segment here would leave a channel with no writer that the
	// real helper would then find already present.
	mChannelBlock = shared::Open( mChannel, /*create*/ false, error );

	if( mChannelBlock == nullptr )
	{
		// Not logged as an error: "no helper yet" is expected, and logging it
		// every retry would bury the failures that matter.
		mNextAttachAttempt = shared::NowMillis() + 1000;
		return;
	}

	mReader          = shared::Reader( mChannelBlock );
	mAttachedChannel = mChannel;
	mUploadedSerial  = 0;

	diag::info( "attached to helper channel '" + mChannel + "'" );
}

bool CartridgePlugin::UpdateTextureFromHelper()
{
	// Retry an attach that has not succeeded yet, at a human rate rather than a
	// frame rate.
	if( mChannelBlock == nullptr )
	{
		const uint64_t now = shared::NowMillis();
		if( now >= mNextAttachAttempt )
		{
			mPendingAttach = true;
			ApplyPendingAttach();
		}
		if( mChannelBlock == nullptr )
			return false;
	}

	const uint64_t now  = shared::NowMillis();
	const uint64_t beat = mChannelBlock->helperBeat.load( std::memory_order_acquire );
	const bool alive    = beat != 0 && ( now - beat ) < shared::kBeatTimeoutMs;

	if( alive != mHelperWasAlive )
	{
		// Logged on the transition only. A helper that died mid-show is the
		// single most useful line this plugin can write, and it is worthless if
		// it is repeated sixty times a second.
		if( alive )
			diag::info( std::string( "helper alive on '" ) + mAttachedChannel + "': "
						+ mChannelBlock->coreName );
		else
			diag::warn( std::string( "helper on '" ) + mAttachedChannel
						+ "' stopped responding; holding the last frame" );
		mHelperWasAlive = alive;
	}

	// --- control out -------------------------------------------------------
	// Written even when the helper looks dead: it may be a stall rather than a
	// crash, and a pad that stopped updating during a stall would leave a
	// button stuck down when it recovers.
	mChannelBlock->pad0.store( PadWord(), std::memory_order_relaxed );
	mChannelBlock->paused.store( mParams[ PT_RUN ] < 0.5f ? 1u : 0u, std::memory_order_relaxed );
	mChannelBlock->speedMilli.store(
		uint32_t( SpeedFromParam( mParams[ PT_SPEED ] ) * 1000.0 ), std::memory_order_relaxed );
	mChannelBlock->clientBeat.store( now, std::memory_order_release );

	// --- frame in ----------------------------------------------------------
	mReader.Acquire();
	const shared::SlotHeader& h = mReader.Header();

	if( h.width == 0 || h.height == 0 )
		return false;

	if( !PrepareTexture( h.width, h.height, h.aspect ) )
		return false;

	if( h.serial != mUploadedSerial )
	{
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, GLsizei( h.width ), GLsizei( h.height ),
						 GL_RGBA, GL_UNSIGNED_BYTE, mReader.Pixels() );
		mUploadedSerial = h.serial;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

bool CartridgePlugin::PrepareTexture( unsigned width, unsigned height, float aspect )
{
	if( width == 0 || height == 0 )
		return false;

	mFrameWidth  = width;
	mFrameHeight = height;
	mFrameAspect = aspect > 0.0f ? aspect : float( width ) / float( height );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );

	// Allocate at the largest geometry seen so far and never shrink. A core
	// that switches resolution then costs an upload, not a reallocation.
	if( width > mTextureWidth || height > mTextureHeight )
	{
		mTextureWidth  = std::max( mTextureWidth, width );
		mTextureHeight = std::max( mTextureHeight, height );

		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei( mTextureWidth ),
					  GLsizei( mTextureHeight ), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );

		// CLAMP_TO_EDGE, not the default REPEAT: the picture occupies a corner
		// of a larger texture, and REPEAT on the seam wraps the far edge into
		// view as a one-pixel stripe of the opposite side of the screen.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );

		mUploadedSerial = 0; // the new allocation holds nothing yet
	}

	const GLint filter = mParams[ PT_SMOOTH ] >= 0.5f ? GL_LINEAR : GL_NEAREST;
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );

	return true;
}

bool CartridgePlugin::UpdateTextureInProcess()
{
	FrameBuffer& frames = mRunner.GetCore().Frames();
	frames.Acquire();

	const Frame& f = frames.Current();
	if( !PrepareTexture( f.width, f.height, f.aspectRatio ) )
		return false;

	// Only upload a frame we have not already uploaded. A 50 Hz core in a 60 Hz
	// composition repeats one frame in six, and a paused core repeats all of
	// them.
	if( f.serial != mUploadedSerial )
	{
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, GLsizei( f.width ), GLsizei( f.height ),
						 GL_RGBA, GL_UNSIGNED_BYTE, f.pixels.data() );
		mUploadedSerial = f.serial;
	}

	return true;
}

void CartridgePlugin::ComputeQuadScale( int vpWidth, int vpHeight, float& sx, float& sy ) const
{
	sx = 1.0f;
	sy = 1.0f;

	if( vpWidth <= 0 || vpHeight <= 0 || mFrameWidth == 0 || mFrameHeight == 0 )
		return;

	const Scaling mode = ScalingFromParam( mParams[ PT_SCALING ] );

	if( mode == Scaling::Stretch )
		return;

	if( mode == Scaling::Integer )
	{
		// Whole-number pixel multiples only. Aspect correction is deliberately
		// ignored here: the entire point of this mode is that one console pixel
		// is an exact square block of output pixels, and a 8:7 correction makes
		// that impossible by definition.
		const int kx = int( vpWidth / mFrameWidth );
		const int ky = int( vpHeight / mFrameHeight );
		const int k  = std::max( 1, std::min( kx, ky ) );

		sx = float( mFrameWidth * k ) / float( vpWidth );
		sy = float( mFrameHeight * k ) / float( vpHeight );

		// A console picture larger than the composition cannot be shown at 1x
		// or more; fall back to fitting rather than overflowing silently.
		if( sx <= 1.0f && sy <= 1.0f )
			return;
	}

	// Display aspect: the core's own, unless the operator has asked for square
	// pixels. 320x224 on a Mega Drive is a 4:3 picture and 1:1 is visibly tall.
	const float pictureAspect = mParams[ PT_PIXEL_ASPECT ] >= 0.5f
									? mFrameAspect
									: float( mFrameWidth ) / float( mFrameHeight );

	const float viewAspect = float( vpWidth ) / float( vpHeight );

	const bool wider = pictureAspect > viewAspect;
	const bool fill  = mode == Scaling::Fill;

	// Fit shrinks the long axis to bring the whole picture in; Fill grows the
	// short one until nothing is left uncovered. Same comparison, opposite
	// branch.
	if( wider != fill )
		sy = viewAspect / pictureAspect;
	else
		sx = pictureAspect / viewAspect;
}

FFResult CartridgePlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	(void)pGL;

	const int width  = int( mViewport.width );
	const int height = int( mViewport.height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	const Source source = SourceFromParam( mParams[ PT_SOURCE ] );

	bool haveFrame = false;

	if( source == Source::Helper )
	{
		if( mPendingAttach )
			ApplyPendingAttach();

		haveFrame = UpdateTextureFromHelper();
	}
	else
	{
		if( mPendingLoad && !mLoadFailed )
			ApplyPendingLoad();

		mRunner.GetCore().Input().SetPad( 0, PadWord() );

		haveFrame = mRunner.GetCore().HasContent() && UpdateTextureInProcess();
	}

	if( !haveFrame )
	{
		// Nothing loaded, no helper, or no frame yet. Leave the layer alone
		// rather than clearing it -- Resolume has already cleared the target,
		// and a plugin that drew black here would flash on every composition
		// load and every helper restart.
		return FF_SUCCESS;
	}

	float sx = 1.0f, sy = 1.0f;
	ComputeQuadScale( width, height, sx, sy );

	// Plain glUseProgram and glBindTexture rather than the ffglex Scoped*
	// helpers, because every one of those CLEARS its binding to 0 on scope exit
	// instead of restoring what was there. State is put back by hand at the end.
	glBindVertexArray( mVAO );
	glUseProgram( mShader.GetGLID() );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );

	mShader.Set( "Picture", 0 );
	mShader.Set( "Scale", sx, sy );
	mShader.Set( "UvScale",
				 mTextureWidth ? float( mFrameWidth ) / float( mTextureWidth ) : 1.0f,
				 mTextureHeight ? float( mFrameHeight ) / float( mTextureHeight ) : 1.0f );

	glDisable( GL_BLEND );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

	return FF_SUCCESS;
}

} // namespace cartridge
