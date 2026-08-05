#pragma once

#include <string>

/**
	Logging for code that runs inside somebody else's process.

	A member of the fleet's `diag` family, and the same shape as orrery's: log
	file only, no crash handler, no diagnostics bundle. A plugin loaded into
	Resolume must not install a process-wide signal handler -- it would intercept
	faults that are not ours, and a plugin has no business deciding what happens
	when the host dies.

	What it covers here is different from a shader plugin, because the failures
	are different. Every one of these looks identical from the operator's side
	("the layer is black"):

	- **The core would not load** -- missing symbol, wrong architecture, or an
	  ABI version this build refuses.
	- **The core rejected the content** -- wrong system, bad dump, missing BIOS
	  in the system directory.
	- **The core asked for hardware rendering** and was refused, so it is running
	  but drawing nothing.
	- **The core's own log**, forwarded through `GET_LOG_INTERFACE`. This is
	  usually the only thing that says *which* BIOS file is missing.

	`setConsoleEcho` exists for the harness, which is a terminal program and
	should not make anyone tail a file to see why a test failed.
*/
namespace cartridge::diag
{

/// Open the log file and record the build, once per process.
void init();

/// Also write every line to stderr. For the harness and the helper, not the
/// plugin -- Resolume's stderr goes somewhere no operator will ever look.
void setConsoleEcho( bool echo );

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace cartridge::diag
