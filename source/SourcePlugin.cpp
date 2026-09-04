#include "Plugin.h"

/**
	The generator registration.

	**This file is listed directly in the CartridgeSource target, not in
	cartridge_core.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a STATIC archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.
	That is why `cartridge_core` is an OBJECT library and why this lives here.

		nm -gU Cartridge.bundle/Contents/MacOS/Cartridge | grep plugMain
*/
static CFFGLPluginInfo PluginInfo(
	PluginFactory< cartridge::CartridgePlugin >,     // Create method
	"CG01",                                          // Plugin unique ID of maximum length 4
	"Cartridge",                                     // Plugin name
	2,                                               // API major version number
	1,                                               // API minor version number
	0,                                               // Plugin major version number
	1,                                               // Plugin minor version number
	FF_SOURCE,                                       // Plugin type
	"A libretro emulator core as a live source.\n\nPoint it at a core and a game and the console becomes a layer: composite it, key it, run it through other effects, and MIDI-map the joypad onto whatever controller is already on the desk.\n\nNo core, no BIOS and no game is shipped with this plugin, ever. It loads what you point it at, and that is a licensing position as much as a technical one.\n\nIt was built as a companion to old-cathode. A 240p console frame through a real composite encoder, with dot crawl and cross-colour that are consequences rather than decoration, is the thing this exists to make possible.",// Plugin description
	"Cartridge FFGL source"                          // About
);

extern "C" const char* CartridgeBuildStamp()
{
	return "cartridge " CARTRIDGE_VERSION " source, built " __DATE__ " " __TIME__;
}
