#!/usr/bin/env bash
#
# Everything that can be checked without Resolume.
#
# Three layers, and they fail for different reasons:
#
#   1. cartest  -- the libretro host on the CPU. Pixel formats, the padded
#                  pitch, the triple buffer, input ports, the audio ring,
#                  determinism, and two instances of one core staying separate.
#   2. cargl    -- the real plugin class through the real FFGL sequence in a
#                  headless GL 4.1 core context. The only thing that catches a
#                  shader that will not compile or a uniform that does not
#                  resolve.
#   3. helper   -- the same plugin against a real second process, then the same
#                  process killed with SIGKILL underneath it.
#
# What none of them cover is the host: how the parameter groups land in
# Resolume's inspector, and whether a controller MIDI-maps onto the pad. Those
# are Allan's to confirm -- driving that GUI from a session is unreliable.

set -u

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
BUILD="${ROOT}/build"
CORE="${BUILD}/cartridge_testcore.dylib"

FAILED=0

section() {
	printf '\n\033[1m== %s ==\033[0m\n' "$1"
}

run() {
	local label="$1"; shift
	if "$@" > /tmp/cartridge-verify.log 2>&1; then
		printf '  \033[32mPASS\033[0m  %s\n' "${label}"
	else
		printf '  \033[31mFAIL\033[0m  %s\n' "${label}"
		sed 's/^/        /' /tmp/cartridge-verify.log | tail -25
		FAILED=$(( FAILED + 1 ))
	fi
}

if [ ! -x "${BUILD}/cartest" ]; then
	echo "build first:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 2
fi

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" "$ROOT" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# verify.sh does not cd to the repo root, so it is handed in.
root = pathlib.Path( sys.argv[ 2 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Plugin.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = ( root / f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

section "shaders"
run "every shader compiles" shaders_compile

section "libretro host (cartest)"
run "XRGB8888 path"  env -u CARTRIDGE_TESTCORE_FORMAT "${BUILD}/cartest" --check
run "RGB565 path"    env CARTRIDGE_TESTCORE_FORMAT=565 "${BUILD}/cartest" --check

section "plugin in a real GL context (cargl)"
if [ -x "${BUILD}/cargl" ]; then
	run "in-process, 16:9"  "${BUILD}/cargl" --check
	# A square render is where the two-aspect confusion hides: 4:3 into 1:1
	# letterboxes on the other axis, so a sign error that passes at 16:9 fails
	# here.
	run "in-process, 1:1"   "${BUILD}/cargl" --check --size 720x720
else
	echo "  (skipped: cargl not built -- needs the FFGL submodule)"
fi

section "out of process (helper)"
if [ -x "${BUILD}/cartridge-helper" ] && [ -x "${BUILD}/cargl" ]; then
	CHANNEL="verify$$"

	"${BUILD}/cartridge-helper" --core "${CORE}" --channel "${CHANNEL}" --quiet \
		>/dev/null 2>&1 &
	HELPER_PID=$!
	# Killing it is the point of the test below, and bash would otherwise print
	# its own "Killed: 9" line at reap time, which reads like a failure.
	disown "${HELPER_PID}" 2>/dev/null || true

	# Wait for the channel rather than sleeping a guessed amount: on a loaded
	# machine a fixed sleep is either wasteful or flaky.
	for _ in $(seq 1 50); do
		if "${BUILD}/cargl" --helper "${CHANNEL}" >/dev/null 2>&1; then break; fi
		sleep 0.1
	done

	run "same checks, across a process boundary" \
		"${BUILD}/cargl" --check --helper "${CHANNEL}"

	# The claim the whole out-of-process build exists to make. SIGKILL, not
	# SIGTERM: a core that segfaults gets no chance to tidy up.
	( sleep 3; kill -9 "${HELPER_PID}" 2>/dev/null ) &
	run "picture survives the helper being killed" \
		"${BUILD}/cargl" --helper "${CHANNEL}" --survive 6

	kill -9 "${HELPER_PID}" 2>/dev/null || true
else
	echo "  (skipped: helper or cargl not built)"
fi

section "build hygiene"
BUNDLE="${BUILD}/Cartridge.bundle/Contents/MacOS/Cartridge"
if [ -f "${BUNDLE}" ]; then
	# A bundle can load, export plugMain and still contain no plugins if the
	# registration object gets dropped -- see SourcePlugin.cpp.
	if nm -gU "${BUNDLE}" | grep -q plugMain; then
		printf '  \033[32mPASS\033[0m  bundle exports plugMain\n'
	else
		printf '  \033[31mFAIL\033[0m  bundle does not export plugMain\n'
		FAILED=$(( FAILED + 1 ))
	fi

	# Universal is checked with lipo, never with the build log: setting
	# CMAKE_OSX_ARCHITECTURES after the first target is created is silently
	# ignored and the log still says success.
	ARCHS="$( lipo -archs "${BUNDLE}" 2>/dev/null )"
	case "${ARCHS}" in
		*arm64*x86_64*|*x86_64*arm64*)
			printf '  \033[32mPASS\033[0m  bundle is universal (%s)\n' "${ARCHS}" ;;
		*)
			printf '  \033[33mWARN\033[0m  bundle is %s only -- fine for a dev build, not for release\n' \
				"${ARCHS}" ;;
	esac
fi

printf '\n'
if [ "${FAILED}" -eq 0 ]; then
	printf '\033[32mall verification passed\033[0m\n'
	exit 0
fi
printf '\033[31m%d group(s) failed\033[0m\n' "${FAILED}"
exit 1
