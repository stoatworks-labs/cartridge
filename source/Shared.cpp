#include "Shared.h"

#include "Diag.h"

#include <chrono>

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <errno.h>
	#include <fcntl.h>
	#include <sys/mman.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace cartridge::shared
{

namespace
{
#if !defined( _WIN32 )
// macOS caps a POSIX shm name at SHM_NAME_MAX (31) including the leading slash,
// and fails with ENAMETOOLONG rather than truncating. Keeping the prefix short
// leaves the operator a usable amount of name.
constexpr size_t kMaxNameLength = 30;
#endif

struct Mapping
{
	Block* block = nullptr;
#if defined( _WIN32 )
	HANDLE file = nullptr;
#else
	int fd = -1;
#endif
	bool owner = false;
	std::string path;
};

// One mapping per process is enough: a plugin instance attaches to one channel,
// and the helper creates one. Keyed by the block pointer so Close can find it.
Mapping g_mappings[ 8 ];

Mapping* FindSlot( Block* block )
{
	for( auto& m : g_mappings )
		if( m.block == block )
			return &m;
	return nullptr;
}

Mapping* FreeSlot()
{
	for( auto& m : g_mappings )
		if( m.block == nullptr )
			return &m;
	return nullptr;
}
} // namespace

std::string ChannelPath( const std::string& name )
{
#if defined( _WIN32 )
	return "Local\\cartridge." + name;
#else
	std::string path = "/cart." + name;
	if( path.size() > kMaxNameLength )
		path.resize( kMaxNameLength );
	return path;
#endif
}

Block* Open( const std::string& name, bool create, std::string& error )
{
	Mapping* slot = FreeSlot();
	if( slot == nullptr )
	{
		error = "too many open channels in this process";
		return nullptr;
	}

	const std::string path = ChannelPath( name );

#if defined( _WIN32 )
	HANDLE file = nullptr;
	if( create )
	{
		file = CreateFileMappingA( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
								   DWORD( uint64_t( sizeof( Block ) ) >> 32 ),
								   DWORD( sizeof( Block ) & 0xFFFFFFFF ), path.c_str() );
	}
	else
	{
		file = OpenFileMappingA( FILE_MAP_ALL_ACCESS, FALSE, path.c_str() );
	}

	if( file == nullptr )
	{
		error = "could not " + std::string( create ? "create" : "open" ) + " channel " + path;
		return nullptr;
	}

	void* mapped = MapViewOfFile( file, FILE_MAP_ALL_ACCESS, 0, 0, sizeof( Block ) );
	if( mapped == nullptr )
	{
		CloseHandle( file );
		error = "could not map channel " + path;
		return nullptr;
	}
	slot->file = file;
#else
	if( create )
	{
		/*
			Unlink any existing segment before creating, and create exclusively.

			**Reusing one would be a latent SIGBUS.** A helper killed with
			SIGKILL -- which is exactly what a crashing core looks like -- never
			unlinks, so its segment outlives it. Opening that with O_CREAT
			succeeds, and then `ftruncate` to the new size **fails with EINVAL
			on macOS**, because a POSIX shared memory object may only be sized
			once. The result is a mapping of `sizeof(Block)` over an object that
			is smaller, and every access past the old end faults.

			That is invisible while the struct never changes size, and it turns
			into a crash on the first build that adds a field. Unlinking first
			makes it impossible instead of unlikely.
		*/
		shm_unlink( path.c_str() );
	}

	const int flags = create ? ( O_CREAT | O_EXCL | O_RDWR ) : O_RDWR;
	const int fd    = shm_open( path.c_str(), flags, 0600 );
	if( fd < 0 )
	{
		error = "could not " + std::string( create ? "create" : "open" ) + " channel " + path
				+ ": " + std::string( std::strerror( errno ) );
		return nullptr;
	}

	if( create && ftruncate( fd, off_t( sizeof( Block ) ) ) != 0 )
	{
		const std::string why = std::strerror( errno );
		::close( fd );
		shm_unlink( path.c_str() );
		error = "could not size channel " + path + ": " + why;
		return nullptr;
	}

	void* mapped = mmap( nullptr, sizeof( Block ), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
	if( mapped == MAP_FAILED )
	{
		::close( fd );
		error = "could not map channel " + path + ": " + std::string( std::strerror( errno ) );
		return nullptr;
	}
	slot->fd = fd;
#endif

	Block* block = static_cast< Block* >( mapped );

	if( create )
	{
		// Zero first: a segment reused from a crashed helper carries its old
		// heartbeat and its old ready index, and a client that attached in the
		// gap would read one stale frame as though it were live.
		std::memset( block, 0, sizeof( Block ) );
		block->magic   = kMagic;
		block->version = kVersion;
		block->ready.store( 2, std::memory_order_release );
		block->speedMilli.store( 1000, std::memory_order_release );
	}
	else if( block->magic != kMagic || block->version != kVersion )
	{
		// A stale segment from another build has the same name and a different
		// layout. Refusing is the only safe move -- reading it would present as
		// random pixels and a wrong geometry.
		error = "channel " + path + " is not a cartridge channel of this version";
#if defined( _WIN32 )
		UnmapViewOfFile( mapped );
		CloseHandle( slot->file );
		slot->file = nullptr;
#else
		munmap( mapped, sizeof( Block ) );
		::close( slot->fd );
		slot->fd = -1;
#endif
		return nullptr;
	}

	slot->block = block;
	slot->owner = create;
	slot->path  = path;

	diag::info( std::string( create ? "created" : "opened" ) + " channel " + path );
	return block;
}

void Close( Block* block )
{
	Mapping* slot = FindSlot( block );
	if( slot == nullptr )
		return;

#if defined( _WIN32 )
	UnmapViewOfFile( slot->block );
	if( slot->file )
		CloseHandle( slot->file );
	slot->file = nullptr;
#else
	munmap( slot->block, sizeof( Block ) );
	if( slot->fd >= 0 )
		::close( slot->fd );
	slot->fd = -1;

	// Only the creator unlinks. A client that unlinked would leave a running
	// helper writing into a segment no new client could ever find.
	if( slot->owner )
		shm_unlink( slot->path.c_str() );
#endif

	slot->block = nullptr;
	slot->owner = false;
	slot->path.clear();
}

uint64_t NowMillis()
{
	using namespace std::chrono;
	return uint64_t(
		duration_cast< milliseconds >( steady_clock::now().time_since_epoch() ).count() );
}

} // namespace cartridge::shared
