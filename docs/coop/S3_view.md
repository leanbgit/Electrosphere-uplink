# Spike S3: the guest's own view

Prototype in `runtime/hle/coop.c` (`view_mark`, `view_replay`, the GPU
overlay and the display VRAM) and `runtime/hle/psyq_gpu.c` (`draw_ot`).
Switched on in the bench with `--coop-guest --coop-view` (what the guest's
computer will do). Measured on `session_20261023_153603.ac3i`.

**Status: works.** The guest's computer shows the guest's camera and HUD
while its game memory, scratchpad, video memory and sound state stay
byte-identical to the host's, measured every field for 40,000 fields (about
11 minutes of play, several missions, disc 2 included). Cost about 3–5% of
the bench's speed, on the guest's computer only.

## How the flight frame draws

```
start of frame:  PutDispEnv, PutDrawEnv, DrawOTag(previous frame's picture)
flight frame:    ClearOTagR(this frame's buffer)
                 simulation tick, called through a pointer: player update 0x800729DC,
                   then world, AI, radar/target preparation (0x8004658C, 0x8003A23C,
                   0x8007F0F8 ...)
                 back in the flight frame at 0x80033A8C
                 camera   0x8007B438   (follows the player through world+0xEC)
                 view     0x800332E4
                 drawing  0x80033324 ... 0x80033814 (returns to 0x80033DA8)
end of frame:    DrawSync, VSync(2)
```

The 3D picture is built into one of two buffers (ordering table + packets,
0x1F800 bytes each; OT bases at world+0x18/+0x1C, packet bases at
+0x20/+0x24, current OT at +0x10). The HUD keeps its drawing packets
elsewhere and links them into that ordering table. The GPU draws the whole
thing at the start of the next frame (`DrawOTag`).

## How the guest's view is made

1. **Mark** right after the player update (host and guest done): save RAM,
   scratchpad, registers and the return address into the tick.
2. The rest of the frame runs normally for the host.
3. After the host's last drawing call, **replay** for the guest: go back to
   the mark, swap the guest in (its state, slot table entries 0/1, its own
   camera/view state with the follow pointer aimed at its plane), run the rest
   of the tick (it returns to the interpreter's sentinel, because the flight
   frame calls it through a pointer), then the flight frame from 0x80033A8C to
   the end of drawing (`ac3_interpret_until`). The only library calls on the
   way are memory-only (rand, memset, ClearOTagR, SetDrawArea, CatPrim).
4. Keep the guest's picture: its buffer, plus every packet the ordering table
   links to outside it (found by walking the list like the GPU). Put the
   host's memory and registers back.
5. At the next `DrawOTag`, draw twice: first the host's picture into the
   game's VRAM (the game sometimes copies screen pixels back into memory, so
   this VRAM must match the host's), then lay the guest's picture over RAM,
   draw it into a **display copy of VRAM** with the same starting GPU state,
   and take the game's bytes back. The display copy is refreshed from the
   game's VRAM each time except for the picture on screen (the game double
   buffers). Screens show the display copy.

## Wrong turns (kept for the record)

| Attempt | Why it failed |
|---|---|
| Draw only the guest's view, restore the pre-camera memory | the host's own drawing leaves state that later frames use: drifted at the first view |
| Replay from the camera call | camera followed the host (fixed by aiming world+0xEC at the guest), HUD still the host's: the HUD is prepared in the simulation tick |
| Keep the guest's packets in RAM | game memory differs from the host's (HUD packet areas) |
| One VRAM | the game read 16 screen pixels into memory at field 20,266 (StoreImage reached by a tail jump): drift |

## Results

| Check | Result |
|---|---|
| Guest's camera | yes |
| Guest's HUD (altitude, speed, pitch ladder, missiles, radar/map) | yes |
| Shared messages ("BULLS EYE") | shown on both, as they should |
| Game state identical to the host's | yes: RAM, scratchpad, VRAM, SPU, 40,000 fields |
| Co-op off | identical to the original engine (20,000 fields) |
| Cost | ~3–5% (one extra tick tail + drawing + GPU pass, a few memory copies) |

## Still to do

- Enhanced renderer (website "Enhanced" mode): the hooks are already given
  only the guest's pass; the presenter must show the display VRAM
  (`ac3_coop_display_vram`). Not tested yet.
- The website's original-picture presenter (`platform/web/web_platform.c`)
  already shows `ac3_coop_display_vram` (falls back to the game's VRAM when
  co-op is off); not run in a browser yet.
- Things drawn straight into VRAM outside `DrawOTag` during flight (if any)
  appear only in the game's VRAM.
- Sound is the host's (engine, warnings): to do in Phase 3.
