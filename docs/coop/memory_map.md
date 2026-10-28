# What the game knows about "the player" (Phase 0 findings)

Measured with the headless bench on `session_20261023_153603.ac3i` (English,
random missions from the 100% save), mainly at fields 9,600 and 14,200 (in
flight). Addresses are for the Rev 1 `ACE3.BIN`. **Confidence:** *measured* =
seen in RAM or in a trace; *read* = read from the code; *guess* = still to confirm.

## Globals (gp = 0x800BCB74)

| Address | Holds | Confidence |
|---|---|---|
| gp+0x010 → 0x800C0418 | controller buffers (libpad direct mode): port 1 at +0x00, port 2 at +0x22 (34 bytes each) | measured |
| gp+0x2E0 → 0x80145170 | world / view state (camera rotation at +76, view pointer at +40, sun at +108) | read (`hw_world.c`) |
| gp+0x2E8 → 0x8014542C | **entity slot table**, 64 pointers | measured |
| gp+0x2EC → 0x800BF2D0 | scene / game state (group +44, phase +48) | measured |
| gp+0x514 → 0x801771E0 | **player aircraft state block**, five pointers into it: | measured |
| gp+0x518 → 0x801771F8 | | |
| gp+0x51C → 0x801771E8 | | |
| gp+0x520 → 0x80177208 | | |
| gp+0x524 → 0x80177278 | | |
| 0x800CBF58 | random seed | read (oracle) |

The player state block spans at least **0x801771E0–0x801772B4** (all writes in
that page during a flight field fall inside it).

## Entities

The slot table points into one fixed pool at **0x8014552C–0x80147324**:

| Slots | Size | Seen holding |
|---|---|---|
| 0 | 0x58 | **the player's aircraft** |
| 1 | 0x58 | nothing live so far (candidate: unused second "player" slot, **to check in other missions**) |
| 2–31 | 0x5C | aircraft and missiles |
| 32–63 | 0x94 | ground objects, turrets, structures |

Slot layout (words, measured):

| Word | Meaning | Confidence |
|---|---|---|
| 0 | handle: bits 0–5 slot index, bits 6–7 generation, **bit 16 = live**, other flags above | measured |
| 1 | object type (player 0x900; others 0x002–0x805, 0x0AAA040E for some structures) | measured |
| 2–4 | position x, y, z (s32, 20.12 fixed point) | measured |
| 6 | packed orientation | guess |
| 20 | **target handle**: the handle of the object this one is attacking or tracking | measured |

**Targeting is by handle, not by "the player".** At field 14,200: four turrets
and two aircraft have target = the player's handle (0x1E240), one aircraft
targets another aircraft (slot 3), a structure targets slot 2, and the player's
own lock-on (slot 0 word 20) is a structure in slot 39. So the AI already
attacks non-player objects. A second player aircraft with its own handle should
be targetable without changing the AI's data model; what remains to find is
the code that *chooses* targets, which may still prefer slot 0.

## Controllers

1. `0x80026E84` reads both ports every frame and decodes each into a
   controller record (`0x80026BA4`): records at `0x800C045C + 0x10·n`, with
   their state at **0x800BF320** (port 1) and **0x800BF388** (port 2).
   **The game polls port 2 every frame and keeps a record for it** (measured;
   no second controller was connected in the recording, so the decoding of a
   live port 2 is still to confirm with a two-controller recording).
2. `0x80028C24(controller, map, …)` turns port 1's state into the game's
   **input map at 0x800BF3F0** (applies the button configuration). It is the
   only reader of the controller state in flight. (measured)
3. The input map is read by the player update, the HUD (`0x80033EF0`), the
   camera (`0x8007B438`) and about ten other functions. (measured)

## The frame in flight

- The flight frame function (copied in the native oracle as
  `flight_frame_stepped`) clears the ordering table, **calls the simulation
  tick through a function pointer**, sets up the camera (`0x8007B438`,
  `0x800332E4`), then **draws** (`0x80033324` … `0x80033814`, return addresses
  0x80033CEC–0x80033DA8). The native oracle marks the drawing part as
  checkpoints 18–19. (read)
- Simulation ticks: `0x80032F3C` (and variants `0x8003307C`, `0x800330CC`,
  `0x80033104`, `0x800331E0`, `0x8003329C`). `0x80032F3C` calls, in order:
  `0x800729DC` **(player)**, `0x80045898`, `0x80038A3C`, `0x800517F4`,
  `0x8004658C`, `0x8006C5B4`, `0x8007DAC8`, `0x800850D8`. (read)

**`0x800729DC` is the player aircraft update** (3.9 KB). It reaches the player
only through gp+0x2E8 (slot table), gp+0x514…0x524 (state block) and gp+0x2E0
/ 0x2EC. It calls `0x8006EF30(entity = slot 0, state = 0x801771F8, spec =
0x800B3898)` (the flight model, measured) and the weapon / lock-on code
(`0x80072178`, `0x80070A34`, `0x80072938`, …).

## What this means for spike S2 (second plane)

The "identity swap" in the roadmap looks practical:

1. Give the guest its own copy of the player state block and its own entity
   slot (slot 1 if it really is spare, otherwise a free aircraft slot).
2. Each simulation tick, after the host's `0x800729DC`:
   - build the guest's input map with `0x80028C24(port-2 controller, guest map)`;
   - swap gp+0x514…0x524, slot table entry 0 and the input map to the guest's;
   - call `0x800729DC` again;
   - swap everything back.
3. Then check what else the player update writes outside these (HUD state,
   weapon counters, camera, sound); each one either gets swapped too or is
   kept host-only.

**Still to find:** where the mission creates the player (slot 0, state block)
so the guest can be created at the same moment; the target-selection code;
kill credit; what slot 1 is for.
