#!/usr/bin/env bash
# Builds the headless native bench (co-op research, determinism checks):
#   platform/headless/build.sh [cc]        -> build-headless/ac3_headless
# Interpreter-only like the website (the player's own ACE3.BIN is run), with
# the co-op probes compiled in (runtime/hle/coop_probe.h). No window, no sound
# device; screenshots are PPM files. Linux or any POSIX system with a C compiler.
set -euo pipefail
cd "$(dirname "$0")/../.."
#   HD=1 platform/headless/build.sh     -> build-headless-hd/ac3_headless
#     adds the enhanced renderer's game side (widescreen, effects switches),
#     as in the website's Enhanced mode, without a GPU
CC=${1:-${CC:-cc}}
HD=${HD:-0}
OUT=build-headless; OBJ=$OUT/obj
CFLAGS="-O2 -DNDEBUG -DAC3_INTERP_ONLY -DAC3_HEADLESS -DAC3_COOP_PROBE -Iruntime -Iruntime/hle -w"
HD_SRC=()
if [ "$HD" = 1 ]; then
  OUT=build-headless-hd; OBJ=$OUT/obj
  # the renderer headers name WebGPU types; Emscripten's webgpu.h (3.1.52) provides them
  if [ ! -f "$OUT/include/webgpu/webgpu.h" ]; then
    mkdir -p "$OUT/include/webgpu"
    curl -sSfL -o "$OUT/include/webgpu/webgpu.h" \
      https://raw.githubusercontent.com/emscripten-core/emscripten/3.1.52/system/include/webgpu/webgpu.h
  fi
  CFLAGS="$CFLAGS -DAC3_ENHANCED -DAC3_HW_WEBGPU -I$OUT/include -Irenderer -Irenderer/hw -Irenderer/webgpu"
  HD_SRC=(renderer/hw/hw.c renderer/hw/hw_glue.c renderer/hw/hw_world.c renderer/hw/hw_settings.c
          renderer/webgpu/hw_fx_game.c renderer/webgpu/hw_packet.c renderer/webgpu/hw_ocean_web.c)
fi
mkdir -p "$OBJ"
if [ "$(cat "$OBJ/.flags" 2>/dev/null)" != "$CC $CFLAGS" ]; then rm -f "$OBJ"/*.o; echo "$CC $CFLAGS" > "$OBJ/.flags"; fi
python3 tools/gen_hle_table.py "$OBJ/hle_table.c" >/dev/null
srcs=(runtime/*.c runtime/hle/*.c host/*.c platform/headless/*.c platform/web/interp_only_stubs.c "$OBJ/hle_table.c" "${HD_SRC[@]}")
objs=(); pids=()
for f in "${srcs[@]}"; do
  [ "$f" = runtime/hle/ac3_oracle_frame.c ] && continue   # calls recompiled functions by name
  o="$OBJ/$(echo "$f" | tr '/' '_').o"
  objs+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find runtime host platform/headless renderer -name '*.h' -newer "$o" -print -quit)" ]; then
    $CC $CFLAGS -c "$f" -o "$o" & pids+=($!)
  fi
done
for p in "${pids[@]}"; do wait "$p"; done
$CC "${objs[@]}" -lm -o "$OUT/ac3_headless"
echo "built $OUT/ac3_headless"
