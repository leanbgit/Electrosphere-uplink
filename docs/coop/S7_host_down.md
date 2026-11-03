# S7: the host shot down, the guest flies on

Before this, when the host was shot down the mission ended, even with the
guest still flying. Now the guest takes over as "the player" and the mission
goes on. If the guest finishes it, it's a success; if the guest also goes
down, the mission fails as usual.

## How the game ends a mission when the player goes down

Traced with `runs/XHD.ac3i`: session `X`, with the host's stick held forward
from field 3,600. The host hits the ground at field 3,881.

1. **The player update's "shot down" branch** (`0x80073880`..`0x8007390C`)
   runs in the host's own update. It:
   - sets the death bits `0x7000` in the player's status word (the word the
     mode pointer `gp+0x520` points to, which is in the player's block; also
     read through `0x8006CB30`);
   - bumps the shot-down counter;
   - switches the camera to the explosion (`0x80079564`) and hides the HUD;
   - stops the engine voices (`0x800301A8`).
2. **The same frame, the mission checker** (`0x800371C8`, at `0x80037424`)
   reads the status word. On the death bits it asks for the end of the
   mission: `0x80032F18(8, 1)`, plus flags in the world and the scene. The
   mission is over about 9 s later.
3. **Other readers** of the status word: enemies, radio, mission scripts.
   There are eight call sites of `0x8006CB30`.

## The take-over (`coop_guest.c`, `take_over`)

**The trigger:** in the frame the host's update newly sets `MODE_DOWN`,
while the guest's aircraft is alive. Then, before the rest of the frame:

- **Undo the host's death on the shared world.** The host-only memory
  (engine sound channels, shot-down counter, shot-down screen effect, shake,
  world flags: the HUD) goes back to how it was before the host's update.
  It is saved every frame while a guest flies.
- **Swap the guest in for good,** exactly as for its update every frame:
  - its copy of the player's memory, including the status word, which is
    alive;
  - slot 0, and the slot bits of the two handles (below);
  - its camera, HUD state, incoming-missile records and weapon flash;
  - its plane's second object.
- **Make the host the one shot down:** `G.dead`, with the host's state now
  in `G.mine`. `G.live` is off, so there is no guest update, no view replay
  and no second enemy pass. The game is now an ordinary one-player game with
  the guest as its player.
- **Controls:** controller 2 is read as controller 1 (`ac3_pad_swap`), so
  the guest steers, pauses, and so on.
- **Drawing:** the plane drawn as "the player" gets the guest's model
  (`coop_plane.c`), and the HUD's small plane picture comes from the second
  bank (`coop_view.c`, after the frame's last drawing call).

The game now sees a living player. The mission checker, enemies and
scripts carry on, and both computers show the guest's plane with its HUD
and play its sounds.

**The handles.** The game finds an object from its handle (low 6 bits: its
slot), for instance the camera's target and the owner of each shot. At first
only the slot table was swapped, so the guest's handle still said slot 1. The
user's recording `MU` showed what followed at the mission update ("UPDATE
TARGET", field 10,560): the camera was reset to follow its target by handle
(`0x800793C4`, camera `+0xC8` = 1), which was the host's wreck. Then:
- the minimap stopped moving (apparently it follows the camera's target);
- the plane list for drawing (`0x80067144`) gives flag 2 ("the player") only
  to the camera's target, so the guest's plane went to the other record and
  was drawn with the host's model.

Now the two handles exchange their slot bits as well: the guest's says
slot 0. Checked on `MU`: after the update the camera stays on slot 0, the
minimap moves and the plane keeps the guest's model.

**When the mission ends** (`ac3_coop_field`), the player's memory goes back
to the host's (including the hangar's plane and weapon choice) and
controller 1 is the host's again.

**Screens** (`ac3_coop_phase` = 5, `web/online.js`):
- the host's: dimmed, "Shot down: watching <guest> fly on until the mission
  ends";
- the guest's: "<host> was shot down. The mission is yours now!" for 5 s.

## Checked

| Recording | What happens | Both computers |
|---|---|---|
| `XHD` (host dives at 3,600) | host down at 3,881; the guest flies on with its own plane, HUD and missiles; the mission runs to the end of the recording (10,024) | identical on every field |
| `XBD` (host dives, then the guest dives at 4,600) | host down at 3,881, guest down later, mission over at 5,768 | identical; also two web runs in lockstep |
| `X` (nobody down) | as before | identical to the earlier log on every field |

## Not checked yet

- **A mission completed by the guest after the host went down:** the results
  screen and what comes after. This needs a recording.
- **The engine sound after the take-over** stays the host's plane's. The
  guest's plane sounds only live in the second sound chip, which follows the
  real chip once the guest is the player.
