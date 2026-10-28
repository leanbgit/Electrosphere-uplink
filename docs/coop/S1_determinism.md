# Spike S1: can two players' games stay in step?

Lockstep co-op runs the whole game on both computers and exchanges only
controller input, so both must compute exactly the same thing from the same
input. This page tracks what has been measured.

## Measured (2026-10-28, headless bench)

Recording `session_20261023_153603.ac3i`, first 20,000 fields (menus, several
missions, disc-2 streaming). Each run logs hashes of RAM, scratchpad, VRAM and
SPU state every field; `tools/coop/hashcmp.py` finds the first difference.

| Run | Compared with plain build | Result |
|---|---|---|
| Plain build, run twice | itself | identical, all 20,000 fields |
| Enhanced game side, widescreen **off**, web default effects | plain | **identical** |
| Enhanced game side, widescreen off, effects off | plain | **identical** |
| Enhanced game side, **widescreen on**, web default effects | plain | RAM differs from field 5,986 (first flight), SPU from 14,208 |
| Enhanced, widescreen on, **atmosphere on** (vs off) | widescreen-on run | RAM differs from field 5,988 |

What differs with widescreen on: at field 14,200 about 138 KB of RAM, almost
all in two large double-buffered areas (0x8017B3xx–0x8018Axxx and
0x8019ABxx–0x801AAxxx, likely the drawing packet buffers) and the stack. **The
entity pool (0x8014552C–0x80147324) is identical**, and scene changes happen
on the same fields, so the gameplay itself did not diverge in this recording.
It is still a difference in the game's memory, and the earlier Geofront
recording showed it can reach gameplay values (an altitude reading).

Why: widescreen scales the screen X that the GTE returns to the game (RTPS) and
widens the game's culling (`ReadGeomScreen`), and *atmosphere* turns off the
GTE's depth cueing (`ac3_gte_no_depthcue`). The game sees these values.

## Consequences for co-op

1. **Simulation-affecting settings come from the host.** Widescreen and
   atmosphere (the only two found) are sent to the guest at join and locked
   for the session. All other graphics settings (resolution, clouds, far
   terrain, ocean, fog, sun) are free per player: they did not change the
   game's memory at all.
2. **Audio must not drive the game.** In normal web play the amount of sound
   mixed per field follows the audio device's queue
   (`spu->frames_wanted()`), and the game can read voice status
   (`SpuGetKeyStatus`), which advances with mixing. Replays avoid this by
   mixing a fixed amount per field (`ac3_oracle_deterministic()`). Co-op must
   do the same and absorb device clock drift in the audio output instead.
   *Still to measure:* how often the game reads voice status in flight.
3. The game often runs at 30 frames per second in flight (two fields per game
   frame). Input is still exchanged per field; that's simplest and costs
   nothing extra.

## Still to do

- [ ] Same comparison **between the website (WebAssembly) and the native bench**,
      and between two different PCs and browsers. Needs the Emscripten build;
      the plan is to run the browser files under Node next to the discs (the
      existing `platform/web/run_worker_node.js`) with a hash log.
- [ ] Full 48,469-field recording, and the Geofront recording.
- [ ] Count `SpuGetKeyStatus` / `SpuGetAllKeysStatus` calls in flight.
- [ ] Fixed-rate audio mode for the web build.
