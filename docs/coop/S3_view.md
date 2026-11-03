# Spike S3: the guest's own view

Prototype in `runtime/hle/coop.c` (`view_mark`, `view_replay`, the GPU
overlay and the display VRAM) and `runtime/hle/psyq_gpu.c` (`draw_ot`).
Switched on in the bench with `--coop-guest --coop-view` (what the guest's
computer will do). Measured on `session_20261023_153603.ac3i`.

**Status: works.** Since the lock-on work (S2_guest.md) the replay runs on
**both** computers, because the HUD code decides lock-on while it draws; only
the guest's computer keeps the picture. The guest's computer shows the guest's camera and HUD
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

## New targets in the guest's HUD (2026-11-02)

Each slot has a HUD record (`HUD_RECS`, 0x34 bytes) that decides its box and
label; the guest has its own copies (`G.hud_rec`), taken from the host's when
the guest is created. Objects that appear later (the enemies after a mission
update, in slots that were free) get their record set up in the host's copy
only, so the guest's copy kept an old object's or nothing: in the guest's
view they had the wrong label (an earlier object's name, or none, where the
host saw "TGT") or no box at all, and the guest could not lock on to them.
The same stale records explain the words "ALERT" / "DAMAGE", and once the
whole word sheet as one huge "box", seen in place of a box: a stale record had
a class (`+2`) of 0x11 where only 1 (aircraft) and 2 (ground) exist, and the
box and label come from tables indexed by it.

`coop_hud_new_targets` (each frame before the guest's update):
- copies the host's record into the guest's when a slot comes into use, or
  when the host's record has the object's kind (`+3`) and the guest's
  another;
- and every frame copies the parts that describe the object rather than the
  player's view of it: `+2..+5` and `+0x20..+0x23`. Over the whole of `TG`
  they never differed otherwise, so this changes nothing in the normal
  case.

## Audit: what the host's side alone changes (2026-11-03)

Logged every write into the per-player memory around a mission update
(`TG`, fields 7,560..7,700: the HUD's arrays and radar blips, targeting and
alert state, the HUD records, the player's block), and looked for writers
that run only on the host's side (spawning, mission checker) rather than in
each player's update or HUD:

| Writer | What | Done |
|---|---|---|
| `0x8004E300` (an object appears) | HUD record `+2..+5` (class, kind) | copied to the guest's (above) |
| same | radar blip colour (`BLIP_KIND`) | copied every frame: the new enemies were grey (unknown) on the guest's radar, now red |
| `0x80042524` from the mission checker | clears the warning on screen | now clears the guest's too |
| scene 0x2C's rule | PULL DOWN from the host's height | no longer shown to the guest (it gets its own) |
| `0x8003809C` (AI pass) | clears the lock-on alert bit | already handled (`clear_guest_alert`) |
| `0x800762D0` from the mission checker, at the update | the player's mode word (`block +0x28`) = 0x800, 4 | left alone: the host flies on normally with it; no effect on the guest seen. To keep in mind if a mission update ever behaves oddly for the guest |

Everything else written there was each player's own update or HUD, or
shared state (AI tables, slot flags) that both players use alike.
Both computers identical on `TG` and `AL`. Checked on the
user's recording `TG` (field 9,000: "B1C" without a box before, "R201 TGT"
with its box after); both computers identical.

## Enhanced (HD) pictures (2026-11-03)

On the guest's computer each frame is drawn twice at DrawOTag
(`psyq_gpu.c`, `draw_ot`): the host's picture into the game's VRAM with the
enhanced renderer's hooks off, then the guest's picture (laid over the
game's memory: `apply_overlay`) with them on. So the HD picture is the
guest's, and the second VRAM bank (the guest's plane's textures) reaches the
WebGPU renderer (`PKT_BANK`; checked in a browser by the user).

The renderer also reads the camera from the world while it draws
(`hw_world_camera`: the view setup's rotation and the camera object's
position; sky, fog, sun, far terrain, ocean and clouds are placed with it),
and the world held the host's. Now the guest's camera (`G.cam`, the view
setup and camera object as the replay left them) is laid over the world
together with the picture and taken off afterwards. Checked in the HD bench
on `TG`: the renderer now gets the guest's camera for the guest's pass (field
8,896: host at x -2,635, guest at -19,191), and both computers stay identical.

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
