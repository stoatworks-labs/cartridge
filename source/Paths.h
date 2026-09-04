#pragma once

#include <string>

/**
	Where the plugin keeps the directories a libretro core needs.

	**A core handed a null system directory does not degrade gracefully.** Four
	cores fail four different ways: fceumm and Genesis Plus GX segfault inside
	`retro_load_game`, Nestopia and Mesen return false. `cartest` and
	`cartridge-helper` both take `--system` and `--save`, but the plugin — the
	one that runs inside Resolume — answered `GET_SYSTEM_DIRECTORY` with `true`
	and a null pointer, and there is no parameter through which an operator
	could supply one. So loading fceumm in In Process mode segfaulted Resolume.

	These are defaults rather than settings for that reason: there is nowhere to
	put a setting, and the failure they prevent is a host crash. Both honour an
	environment override, the same shape `CARTRIDGE_LOG_DIR` already has, so a
	site that keeps BIOS files elsewhere can point at them.
*/
namespace cartridge
{
namespace paths
{

/// BIOS and firmware. `CARTRIDGE_SYSTEM_DIR` overrides.
std::string SystemDirectory();

/// Battery saves and SRAM. `CARTRIDGE_SAVE_DIR` overrides.
std::string SaveDirectory();

/// Creates the directory if it is missing. Returns false if it could not be.
bool EnsureDirectory( const std::string& path );

} // namespace paths
} // namespace cartridge
