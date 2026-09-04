#include "Paths.h"

#include <cstdlib>
#include <filesystem>

namespace cartridge
{
namespace paths
{
namespace
{

constexpr const char* kAppName = "Cartridge";

std::string environmentVariable( const char* name )
{
	const char* value = std::getenv( name );
	return value ? std::string( value ) : std::string();
}

std::string homeDirectory()
{
#if defined( _WIN32 )
	std::string local = environmentVariable( "LOCALAPPDATA" );
	if( !local.empty() )
		return local;
	return environmentVariable( "USERPROFILE" );
#else
	return environmentVariable( "HOME" );
#endif
}

/// The per-user data root, matching where Diag puts logs so one folder holds
/// everything the plugin owns.
std::string dataRoot()
{
	const std::string home = homeDirectory();
	if( home.empty() )
		return {};

#if defined( _WIN32 )
	return home + "\\" + kAppName;
#elif defined( __APPLE__ )
	return home + "/Library/Application Support/" + kAppName;
#else
	const std::string data = environmentVariable( "XDG_DATA_HOME" );
	return ( data.empty() ? home + "/.local/share" : data ) + "/" + kAppName;
#endif
}

std::string subdirectory( const char* override_, const char* leaf )
{
	const std::string forced = environmentVariable( override_ );
	if( !forced.empty() )
		return forced;

	const std::string root = dataRoot();
	if( root.empty() )
		return {};

#if defined( _WIN32 )
	return root + "\\" + leaf;
#else
	return std::string( root ) + "/" + leaf;
#endif
}

} // namespace

std::string SystemDirectory()
{
	return subdirectory( "CARTRIDGE_SYSTEM_DIR", "system" );
}

std::string SaveDirectory()
{
	return subdirectory( "CARTRIDGE_SAVE_DIR", "saves" );
}

bool EnsureDirectory( const std::string& path )
{
	if( path.empty() )
		return false;

	// std::filesystem, not a shelled-out `mkdir -p`. Diag still shells out —
	// that is the fleet-wide Diag issue and is not this change's to fix — but
	// there is no reason to add a second one.
	std::error_code ec;
	std::filesystem::create_directories( path, ec );

	// create_directories reports false with no error when the directory was
	// already there, so the existence check is what decides.
	return std::filesystem::is_directory( path, ec );
}

} // namespace paths
} // namespace cartridge
