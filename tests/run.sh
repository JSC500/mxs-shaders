#!/bin/bash
# Tests the Linux build without the game: the ini code, then a stand-in mxbin on Xvfb with Mesa's
# software GL, launched through mxs-shaders.sh from a folder with a space in its name, with real X
# input from XTest and a window manager-style frame around the game's window.
# Needs: Xvfb, Mesa GLX, libXtst, ImageMagick's import (for a screenshot of the panel).
# Optional: MXS_GAME_SHADERS=<probe's shaders folder> also compiles the game's own shaders, so the
# default pack replaces them and Mesa has to compile and link it. They aren't in the repo.
set -u
cd "$(dirname "$0")/.."
root="$PWD"
out="$root/build/tests"
game="$out/MX Simulator"
xdg="$out/xdg"
data="$xdg/mxs-shaders"
lib="$root/build/libmxs-shaders.so"
L=/usr/lib/x86_64-linux-gnu
shaders="${MXS_GAME_SHADERS:-}"
fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1"; fail=1; fi; }

rm -rf "$out" && mkdir -p "$game" "$data"
gcc -std=gnu11 -Wall -Wextra -Isrc -o "$out/ini_test" tests/ini_test.c src/platform_linux.c -ldl || exit 1
gcc -O1 -Wall -o "$game/mxbin" tests/fake_mxbin.c $L/libGL.so.1 -lX11 || exit 1
gcc -O1 -Wall -o "$out/driver" tests/driver.c $L/libXtst.so.6 -lX11 || exit 1
cp "$game/mxbin" "$out/notmx"
cp "$lib" mxs-shaders.sh "$game/"
# Small shadow maps keep the software renderer quick. Everything else starts from the defaults.
printf '[mxs-shaders]\nshadow_size=1024\nmsaa=4\n' > "$data/mxs-shaders.ini"

echo "1. ini reader and writer"
"$out/ini_test" "$out/t.ini" | sed 's/^/  /'

echo "2. stand-in game through mxs-shaders.sh"
Xvfb :77 -screen 0 1280x1024x24 -nolisten tcp >/dev/null 2>&1 &
xpid=$!
sleep 1
export DISPLAY=:77 LIBGL_ALWAYS_SOFTWARE=1
(cd "$game" && XDG_DATA_HOME="$xdg" ./mxs-shaders.sh "$out/id.txt" $shaders >"$out/game.txt" 2>&1) &
gpid=$!
"$out/driver" "$out/id.txt" "$out/panel.png"
wait $gpid
log="$data/mxs-shaders.log"
sed 's/^/  game: /' "$out/game.txt"
grep -vE "^glXGetProcAddress|^  role " "$log" | sed 's/^/  log: /'
check "the library is loaded through the link in the data folder" "[ -L '$data/libmxs-shaders.so' ]"
check "the mod attached and read its settings" "grep -q '^MXS Shaders (Linux) attached' '$log' && grep -q '^config: ' '$log'"
check "the swap hook ran" "grep -q '^glXSwapBuffers reached the library' '$log'"
check "the context was set up (anisotropy, MSAA, shadow maps)" "grep -q '^anisotropy ' '$log'"
check "settings the user didn't set were added to the ini, and theirs kept" "grep -q '^shadow_size=1024' '$data/mxs-shaders.ini' && grep -q '^pack=default' '$data/mxs-shaders.ini' && grep -q '^ui_key=119' '$data/mxs-shaders.ini'"
check "the panel font loaded" "grep -q '^ui: font /' '$log'"
check "the panel opened twice and closed twice" "[ \$(grep -c '^panel open:' '$log') = 2 ] && [ \$(grep -c '^panel closed:' '$log') = 2 ]"
check "6 pointer events held back the first time (3 presses, 3 releases)" "grep -q '^panel closed: 6 pointer' '$log'"
check "the game got only the 3 clicks made while the panel was closed, as events" "grep -q 'received 3 button presses, 3 releases' '$out/game.txt'"
check "and by polling XQueryPointer" "grep -q 'saw 3 clicks by polling' '$out/game.txt'"
check "the panel closed by itself when focus left the game" "[ \$(grep -c '^panel open:' '$log') = 2 ]"
check "glXGetProcAddress can't bypass a static hook" "grep -q 'glDrawArrays) is the hook' '$out/game.txt'"
if [ -n "$shaders" ]; then
	check "every game shader got a role" "! grep -q 'unknown shader role' '$log'"
	check "the default pack replaced model, reflection, sprite and terrain" "for r in model.fsh reflection.fsh sprite.fsh terrain.fsh; do grep -q \"role \$r, using pack shader\" '$log' || exit 1; done"
	check "and all programs linked on Mesa" "grep -q 'programs linked, 0 failed' '$out/game.txt'"
fi
check "no failures or GL errors logged" "! grep -qiE 'failed|^GL error|^glGetError' '$log'"

echo "3. inert outside mxbin"
cp "$log" "$out/game.log"	# kept for reading after a failure
rm -f "$log"
LD_PRELOAD="$data/libmxs-shaders.so" bash -c 'ls / >/dev/null; true'
check "bash with the library preloaded writes no log" "[ ! -e '$log' ]"
LD_PRELOAD="$data/libmxs-shaders.so" XDG_DATA_HOME="$xdg" timeout 30 "$out/notmx" "$out/id2.txt" >"$out/notmx.txt" 2>&1 &
npid=$!
"$out/driver" "$out/id2.txt" >/dev/null 2>&1
wait $npid
check "a program not called mxbin runs normally and gets every click" "grep -q 'received 6 button presses' '$out/notmx.txt'"
check "and writes no log" "[ ! -e '$log' ]"

echo "4. launcher errors"
out4=$(cd "$game" && XDG_DATA_HOME="$out/with space" ./mxs-shaders.sh 2>&1); r=$?
check "a data path with a space is refused with a clear message" "[ $r != 0 ] && echo \"\$out4\" | grep -q 'space or colon'"
out5=$(cd "$out" && MX_HOME=/nonexistent "$game/mxs-shaders.sh" 2>&1); r=$?
check "a missing game folder asks for MX_HOME" "[ $r != 0 ] && echo \"\$out5\" | grep -q MX_HOME="

kill $xpid 2>/dev/null
[ $fail = 0 ] && echo "all tests passed" || echo "SOME TESTS FAILED"
exit $fail
