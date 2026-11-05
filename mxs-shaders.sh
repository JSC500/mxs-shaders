#!/bin/bash
# Starts MX Simulator with mxs-shaders. Put this file and libmxs-shaders.so in the game folder,
# next to mx and mxbin, and run ./mxs-shaders.sh instead of ./mx. ./mx still starts the game
# without the mod.
#
# The library goes on mxbin only, not on this script or the programs it runs.
# LD_PRELOAD splits its value at spaces and colons with no way to escape them, so a library in a
# folder like "MX Simulator" can't be named there. It's linked into the data folder (a path without
# spaces) and loaded from that link.
#
# Optional: MX_HOME=/path/to/game (the folder holding mxbin), MXS_SHADERS_LIB=/path/to/libmxs-shaders.so

here="$(dirname "$(readlink -f "$0")")"
export MX_HOME="${MX_HOME:-$here}"
lib="${MXS_SHADERS_LIB:-$here/libmxs-shaders.so}"
xdg="${XDG_DATA_HOME:-}"
case "$xdg" in /*) ;; *) xdg="$HOME/.local/share" ;; esac  # the XDG spec ignores a relative path
data="$xdg/mxs-shaders"
link="$data/libmxs-shaders.so"

if [ ! -x "$MX_HOME/mxbin" ]; then
	echo "mxs-shaders: no mxbin in $MX_HOME." >&2
	echo "Put this script in your MX Simulator folder, or run: MX_HOME=/path/to/your/game $0" >&2
	exit 1
fi
if [ ! -f "$lib" ]; then
	echo "mxs-shaders: $lib not found. Put libmxs-shaders.so next to this script." >&2
	exit 1
fi
case "$link" in
*[[:space:]:]*)
	echo "mxs-shaders: $link contains a space or colon, which LD_PRELOAD can't handle." >&2
	echo "Set XDG_DATA_HOME to a folder without them, for example XDG_DATA_HOME=/tmp/mxs $0" >&2
	exit 1
	;;
esac

mkdir -p "$data" && ln -sf "$(readlink -f "$lib")" "$link" || exit 1
exec env LD_PRELOAD="$link${LD_PRELOAD:+:$LD_PRELOAD}" "$MX_HOME/mxbin" "$@"
