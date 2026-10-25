#!/usr/bin/env bash
# Builds the browser version.
#
#   platform/web/build.sh            interpreter-only  -> web/dist-public/ and site/
#   platform/web/build.sh public     (the same)
#                                    no game code: the player's own ACE3.BIN is
#                                    interpreted at run time
#   platform/web/build.sh recomp     recompiled build  -> web/dist/
#                                    needs recomp/main/, generated from your ACE3.BIN
#                                    by the native project's recompiler (not part of
#                                    the public repository); contains translated game
#                                    code: private use only, never publish it
#
# Needs emcc on PATH.
set -euo pipefail
cd "$(dirname "$0")/../.."
MODE=${1:-public}
INC="-Iruntime -Iruntime/hle"

if [ "$MODE" = public ]; then
  OUT=web/dist-public; OBJ=build-web/obj-public
  CFLAGS="-O2 -DNDEBUG -DAC3_INTERP_ONLY"
else
  OUT=web/dist; OBJ=build-web/obj
  CFLAGS="-O2 -DNDEBUG"
  [ -f recomp/main/ac3_table.c ] || { echo "recomp/main/ missing: the recompiled build needs the game code translated by the native project's recompiler"; exit 1; }
fi
# The enhanced renderer (renderer/hw on WebGPU, drawn by web/render-worker.js)
# is part of the build: the page offers "Original" or "Enhanced" graphics and
# falls back to the original picture without WebGPU. HD=0 builds without it,
# into <dir>-sd (the game side then has no PGXP bookkeeping, ~3% faster).
HD=${HD:-1}
LINK_HD=()
if [ "$HD" = 0 ]; then
  OUT=$OUT-sd; OBJ=$OBJ-sd
fi
if [ "$HD" = 1 ]; then
  CFLAGS="$CFLAGS -DAC3_ENHANCED -DAC3_HW_WEBGPU"
  INC="$INC -Irenderer -Irenderer/hw -Irenderer/webgpu"
  LINK_HD=()
fi
mkdir -p "$OUT" "$OBJ"
# objects built with other flags must not be reused (timestamps alone cannot tell)
if [ "$(cat "$OBJ/.flags" 2>/dev/null)" != "$CFLAGS $INC" ]; then rm -f "$OBJ"/*.o; echo "$CFLAGS $INC" > "$OBJ/.flags"; fi

compile() {  # src extra-flags...
  local src=$1; shift
  local o="$OBJ/$(echo "$src" | tr '/' '_').o"
  if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find runtime renderer platform/web -name "*.h" -newer "$o" -print -quit)" ]; then
    emcc $CFLAGS $INC "$@" -c "$src" -o "$o"
  fi
  echo "$o"
}

objs=()
HD_SRC=()
[ "$HD" = 1 ] && HD_SRC=(renderer/hw/hw.c renderer/hw/hw_glue.c renderer/hw/hw_world.c renderer/hw/hw_settings.c renderer/webgpu/hw_fx_game.c renderer/webgpu/hw_packet.c renderer/webgpu/hw_ocean_web.c)
for f in runtime/*.c runtime/hle/*.c platform/web/*.c host/*.c "${HD_SRC[@]}"; do
  # the debug oracle's frame stepper calls recompiled functions by name
  if [ "$MODE" = public ] && [ "$f" = runtime/hle/ac3_oracle_frame.c ]; then continue; fi
  objs+=("$(compile "$f" -Wall -Wno-unused-parameter)")
done

if [ "$MODE" = public ]; then
  python3 tools/gen_hle_table.py "$OBJ/hle_table.c"
  objs+=("$(compile "$OBJ/hle_table.c")")
else
  # The generated game code: built in parallel, warnings off (as in CMakeLists.txt)
  pids=()
  for f in recomp/main/*.c; do compile "$f" -w >/dev/null & pids+=($!); done
  for p in "${pids[@]}"; do wait "$p"; done
  for f in recomp/main/*.c; do objs+=("$OBJ/$(echo "$f" | tr '/' '_').o"); done
fi

emcc -O2 "${objs[@]}" -o "$OUT/ac3.js" \
  -sMODULARIZE -sEXPORT_NAME=createAC3 -sENVIRONMENT=worker \
  -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=4GB \
  -sSTACK_SIZE=4MB -sFORCE_FILESYSTEM -lworkerfs.js \
  -sEXPORTED_RUNTIME_METHODS=FS -sEXIT_RUNTIME=1 "${LINK_HD[@]}"

# HD=1: the render worker's program (the GPU half: hw_webgpu.c), see web/render-worker.js
if [ "$HD" = 1 ]; then
  robjs=()
  for f in renderer/webgpu/hw_webgpu.c renderer/webgpu/hw_fx_webgpu.c renderer/webgpu/hw_ocean_webgpu.c renderer/webgpu/hw_ocean_web.c renderer/webgpu/render_main.c; do
    robjs+=("$(compile "$f" -Wall -Wno-unused-parameter)")
  done
  emcc -O2 "${robjs[@]}" -o "$OUT/render.js" \
    -sMODULARIZE -sEXPORT_NAME=createRender -sENVIRONMENT=worker -sUSE_WEBGPU \
    -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=32MB --no-entry \
    -sEXPORTED_FUNCTIONS=_render_init,_render_buffer,_render_frame
  cp web/render-worker.js "$OUT/"
  cp web/test/bench.html "$OUT/"     # development: speed test on your own GPU (not part of site/)
fi

cp web/index.html web/app.js web/worker.js web/audio-worklet.js web/serve.py web/coi-sw.js web/_headers "$OUT/"
mkdir -p "$OUT/fonts" && cp web/fonts/* "$OUT/fonts/"
# Project NEMO's English patches (included with their permission)
if [ -f web/patches/CD1.xdelta ]; then mkdir -p "$OUT/patches" && cp web/patches/CD?.xdelta "$OUT/patches/"; fi
printf '@echo off\r\ncd /d "%%~dp0"\r\npython serve.py\r\npause\r\n' > "$OUT/serve.bat"
if [ "$MODE" = public ] && strings "$OUT/ac3.wasm" | grep -qE '^func_8[0-9A-F]{7}$'; then
  echo "ERROR: recompiled game code found in the public build"; exit 1
fi
# The website = the public build. site/ is what gets uploaded to the host
# (no serve scripts; includes Project NEMO's patches when web/patches/ has them).
if [ "$MODE" = public ] && [ "$HD" = 1 ]; then
  rm -rf site && mkdir -p site/fonts
  cp "$OUT"/{index.html,app.js,worker.js,audio-worklet.js,coi-sw.js,_headers,ac3.js,ac3.wasm,render-worker.js,render.js,render.wasm} site/
  cp web/fonts/* site/fonts/
  # Project NEMO's English patches (permission granted): web/patches/CD1.xdelta, CD2.xdelta
  if [ -f web/patches/CD1.xdelta ]; then mkdir -p site/patches && cp web/patches/CD?.xdelta site/patches/;
  else echo "note: web/patches/ is empty; add CD1.xdelta and CD2.xdelta to site/patches/ before uploading"; fi
  echo "site/ ready: $(du -sh site | cut -f1)"
fi
ls -la "$OUT"
