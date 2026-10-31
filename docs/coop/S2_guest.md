# Spike S2: a second, guest-controlled aircraft

Prototype: `runtime/hle/coop.c`, switched on in the bench with `--coop-guest`
(the website never switches it on yet). Measured on
`session_20261023_153603.ac3i`, first mission (fields 6,000–9,000).

## How it works

After every call of the player update (`0x800729DC`), the guest's update runs:

1. **Swap in the guest** (the game always works on "the player"):
   - exchange the contents of three per-player memory ranges with the guest's copies:
     the player state block `0x801771E0` (0xE0 bytes), the input map `0x800BF3F0`
     (0x40 bytes) and `0x800C7BE8` (0x10 bytes, a position copy);
   - swap entity slot table entries 0 and 1, so "slot 0" is the guest's aircraft;
   - remember two host-only ranges the player update also writes (engine sound
     channels `0x800C5830`, one word at `0x800BD0C0`).
2. Build the guest's input map from **controller port 2**:
   `0x80028C24(0x800BF388, 0x800BF3F0, 0x800BF42C)`.
3. Call the player update again.
4. Swap everything back, restore the host-only ranges, and restore the CPU
   registers so the host's results stand.

The guest aircraft is created in **slot 1** as a copy of the host's (150 units
to the side, handle = host's with slot index 1) the first time the host has
control (**player mode 1**, the word at `[gp+0x520]+4`). Creating it earlier
fails: the mission moves the host's mode 2 → 4 → 1 from outside the player
update, and a guest copied in mode 2 never leaves it (it drifts in a straight
line with the stick ignored).

## Results

| Check | Result |
|---|---|
| Guest created | yes, at the host's first field with control (field 6,109) |
| Guest flies from port 2 | **yes**: with the scripted pad (`tools/coop/make_p2.py`) it rolls, climbs and turns on its own; with port 2 = port 1 (`--copy`) it flies in exact formation with the host |
| Guest is drawn | **yes**, the game draws slot 1 like any aircraft (picture: formation flight, guest on the left) |
| Host unaffected | **yes**: host position and orientation identical to a run without guest at every sampled field, and the host's state block byte-identical at fields 6,200 and 9,000 |
| Guest weapons | **probably**: with port 2 = port 1, the guest's own missile counter (state block +0xB4) drops in step with the host's (168 → 163). The missiles are not in the entity slot table, so seeing them needs the missile pool (to find) or a picture |
| World reacts to the guest | yes: from field 8,740 the two runs differ in one enemy aircraft, i.e. the AI noticed the guest |
| Deterministic | yes (same input → same hashes) |

## Teammate rules (2026-10-28, later)

| Topic | How it works now | Evidence |
|---|---|---|
| Creation | slot 1, when the host first has control (player mode 1) | field 6,109 |
| Destroyed | the game's own destroy routine (`0x800747E0`) clears its live bit; the guest then **watches the host until the mission is over** (no respawn) | crash at 8,449 |
| Mission over | slot table moves, or no player update for 2 s while the host's mode is not 1, or the host's mode returns to 2 (next intro); the guest's aircraft is removed and it may fly again next mission | 16,864 |
| Missiles | the launch code (`0x800685C0`, owner handle at +0x134 of the missile) runs inside the guest's update: **real missiles owned by the guest** | owner 0x1E241 in the pool |
| Targeting | the tick's targeting step `0x8003A23C` also runs in the guest's turn; its state (0x800C6D60, 0x800C6EC0) is per player | guest selects slot 47 like the host |
| Lock-on | **decided by the HUD code while it draws** (`0x8003F2CC` writes the locked target to 0x800C6D72, plus 0x800C6D68–71). So the guest's camera + HUD replay (S3) now runs on **both** computers, and those bytes become the guest's state | guest launches with targets 0x30, 0x32, 0x06, 0x22 |
| Kill credit | the mission's destroyed-object log (`0x80048758`, count at 0x800BF692) **does not check who fired**: it counts objects flagged 0x800 in the per-object table at 0x800C6FD0, set at mission start | with the host unable to fire, the log gains the guest's kills at 7,155, 8,157, 12,445 |
| Sync | host's and guest's computers byte-identical (RAM, scratchpad, VRAM, SPU) | 40,000 fields; the kill scenario 14,000 fields |

Rejected: running `0x8007F0F8` (lock-on/radar lists) a second time for the guest.
It links entries into lists the world update shares; with two runs per frame a
list became circular and the world update looped forever at field 11,260.

Test recipe for kills (`tools/coop/strip_buttons.py`): `nofire.ac3i` is the session recording with Circle
(missile) removed from port 1 between fields 6,110 and 14,000, and the guest is
`make_p2.py --copy` of the original, so only the guest fires.

## Known gaps (next steps)

- Per-player results (kill count, score) in the results screen: not looked at.
- Guest damage display and missile warnings on the guest's own HUD: likely
  already the guest's (they come from its state), not checked.
- ~~The guest's targeting uses per-object tables computed relative to the
  host~~ **Measured, not a problem** (2026-10-29): with the scripted guest
  thousands of units from the host, the per-object tables (0x800C6FD0,
  0x800C6ED0) computed with the guest as "the player" (in the replay) equal the
  host's on all but 5 frames out of ~8,000, always one bit (0x01000000) on the
  object the host had just selected. Range and lock checks use the player's
  position inside the targeting step and the HUD, which already run for the
  guest. To check again in play with a guest that hunts its own targets.
- The guest uses the host's aircraft model and loadout.
- Only the flight tick variant `0x80032F3C` has been exercised; other mission
  types still to check.
- Testing past the first mission needs recordings made with a guest present:
  the guest changes the world, so a solo recording goes off script.

## Bench recipe

```sh
python3 tools/coop/make_p2.py p2.ac3i 30000            # scripted second pad
python3 tools/coop/make_p2.py p2copy.ac3i --copy REC   # port 2 = REC's port 1
AC3_COOP_DEBUG=1 tools/coop/replay.sh G REC 9000 --coop-guest --coop-p2 p2.ac3i \
    --coop-ents 6000-9000:150 ents.txt
```

`AC3_COOP_DEBUG=1` prints the guest's input map, state block and entity every
30 updates; `AC3_COOP_NOUPDATE=1` creates the guest but never updates it.

## Fixed 2026-10-29: the state block moves between missions

The first browser test (a different random mission) showed both planes with the
same speed, missiles, damage and warnings, and linked, sluggish controls. In
that mission the player's state block is at 0x80171484, not 0x801771E0: the
guest was sharing the host's real block (and swapping 224 unrelated bytes every
frame). `coop.c` now takes the block from the game's pointer gp+0x514 every
time. Checked with the user's recording (native bench and browser build):
separate speed, missiles and altitude.

## Fixed 2026-10-29 (second browser test): death, camera, lock-on

Found with the user's recording `coop_20261029195939` (a night mission).

- **Guest shot down → the host's screen went into the "shot down" sequence**
  (explosion close-up, then no HUD and a wreck camera for ~25 s; the host was
  in fact still flying). The guest's own update runs the game's death branch
  (`0x80073880`..`0x8007390C`): it bumps the shot-down counter (scene+0x958),
  stops engine voices (`0x800301A8`, gp+0x32C..0x32F), switches the camera to
  follow the explosion (`0x80079564` → `0x800793C4`, which writes the camera
  object world+0xB8..0x19C) and hides the HUD (`0x80035740(0)` sets bit 0 of
  world+0). Now:
  - the guest's update works on **the guest's own camera** (the view-private
    ranges are exchanged around it, not only around the view replay), and the
    camera range is the whole camera object, world+0xB8..0x1A0;
  - the shot-down counter, the voice bytes, world+0x28C and world+0 are
    host-only (the guest's writes are discarded).
  Checked: the host keeps flying with its HUD and mission timer, as in the solo
  replay of the same recording; the guest's view shows its own explosion.
- **The guest could never complete a lock**, and its lock box jumped around.
  Locking takes several frames: the HUD keeps a state and timer in the first
  two bytes of each target's record (pointers at 0x800C67A0 + 4·slot) and the
  lock in progress at gp+0x354..0x35B (state, slot, lock-box animation;
  `0x800409D4`). The guest's HUD runs in the view replay, whose memory is thrown
  away, so its progress restarted every frame. The guest now keeps its own copy
  (exchanged around its update and its replay). Checked: the guest locks
  (red lock box) and the two computers stay byte-identical (11,900 fields).
- **"Shared controls until Engage"**: there is no guest plane before the host
  gets control (around the "Engage" banner); the test page showed the host's
  picture on the guest's side. The page now dims that picture and says so.

## Fixed 2026-10-29 (third browser test): target box when far from the host

Recording `coop_20261029205939`: with the host 30,000–40,000 units away, enemy
planes right in front of the guest got no target box (it flickered and jumped)
and could not be locked. Each target's record (0x34 bytes, pointers at
0x800C67A0 + 4·slot) is filled relative to "the player": the tick
(`0x80045278`) keeps the target's distance at +0x10 and **recomputes it only
every 16 frames for targets over 0x84D0 away**, and the in-range/visible flags
at +0x0C come from it. The guest's replay started each frame from the host's
records, so a target near the guest kept the host's "far" distance except on
its one frame in 16. The guest now keeps its own copy of the whole records
(with the lock state), exchanged around its update and its replay. Checked:
boxes and a lock (red box, 1,876 units) at 40,000 units from the host; host's
and guest's computers byte-identical over the whole recording (7,680 fields).

## Fixed 2026-10-29 (fourth browser test): the guest's gun reticle

Recording `coop_20261029211358`. Closing in with the gun, the guest never got
the green gun circle, and now and then a circle flashed up somewhere else.
The reticle code (`0x800662B4`, from the HUD module `0x80063E4C`) keeps a ring of
32 positions of the player's bullets at 0x800C79C8 (16 bytes each, index at
gp+0x4FC) and draws the circle where the bullets fired some frames ago have got
to. Only the host's positions ever stayed in the ring (the guest's replay
memory is thrown away). The ring and its index are now part of the guest's own
HUD state (with the per-target HUD arrays at 0x800C68E0..0x800C6D60). Checked:
the circle sits in front of the guest and travels to the bomber as in single
player; host's and guest's computers byte-identical (10,020 fields).

## Far objects froze unless the host was near (found and fixed 2026-10-29)

Same recording. The bombers (slots 5–8) have speed exactly 0 whenever they are
more than about 50,000 units from the host, in a solo replay and in co-op alike,
even with the guest flying 3,600–14,000 units from them (field 7,200–9,000:
bomber 6 still, guest beside it, host 140,000–200,000 away). The game only runs
distant objects near "the player", and that is the host.

**Fixed the same day.** The check is `0x80038100` (called from `0x80038A3C` in the
tick): each frame it clears the "active" bit 0x8000 of aircraft slots 2..31 and
sets it again by type. Fighters (0x10 in the per-object table 0x800C6FD0) are
always active; others (the bombers) only when their target record has bit 0x80
at +0x0C, "near the player", which `0x80045278` sets from the distance to the
player (under 0x84CF, about 34,000 in record units). The movement code
(`0x800383AC`) skips aircraft without the active bit, so they stand still.
`coop.c` now intercepts `0x80038100` and, while it runs, also marks as near
every target that is near the other player (their records are in the guest's
copy, `G.hud_rec`; during the guest's replay that copy holds the host's), then
takes the extra bits off again. Checked: the bombers keep flying while the guest
is within ~45,000 units of them, however far the host is; they stop only when
both players are far; host's and guest's computers byte-identical (10,020
fields).


## Teammate audit (2026-10-29): what still treated only the host as "the player"

A sweep for code that reads slot 0 as "the player" found four things, all
fixed below (the warnings last, after a browser test confirmed them).

### Enemies never targeted the guest (fixed)

The aircraft AI runs once per frame over slots 2..31: `0x80046854`,
`0x800469EC` (decisions), `0x80046DFC` (manoeuvres), `0x8004701C` (attacks).
It takes "the player" to be slot 0 in many places: the target choice
`0x80046124` takes slot 0 when the player has a free attacker place in the
table at `0x800C6D80` (4 bytes per slot: +1 its target, +2/+3 its attackers),
otherwise the nearest opponent from slot 2 up, never slot 1; scripted "attack
the player" orders; manoeuvres that fly to slot 0; aiming (`0x8004AD98`).
Targets are also sticky: the hunters get the host at mission start, before the
guest exists.

`coop.c` now splits the aircraft each frame by "their" player (the one they
attack, otherwise the nearer one) and runs each of the four loops twice: once
for the host's group as usual, once for the guest's group with the two players
exchanged everywhere the AI looks (slot table entries 0/1, the attacker-table
words 0/1, every aircraft's 0/1 target bytes in the table and in its AI record
at `[gp+0x34C] + 0xB8 * slot`, +0x60 target and +0x3F last target). Aircraft of
the other group are marked inactive (0x8000 cleared) for that run. An enemy
attacking one player switches to the other when that one is under 60% of the
distance and has a free attacker place. Checked: enemies with the guest as
target, NF recording 18 (host 9), X recording 54 (host 6); host's and guest's
computers byte-identical (X 10,100 fields, NF 12,000).

### Mission rules checked only the host's position (fixed: both players must meet them)

`0x800540E0` (once per frame from `0x800371C4`) has, per mission scene
(scene+0x38C), checks on slot 0's position that move the mission on or fail it
(radio messages via `0x8004287C`, mission scripts via `0x80054EBC`, mission
flags at gp+0x340):

| Scene | Condition on "the player" |
|---|---|
| 0x0E, 0x35 | height (+0xC) > 0xFB1E0000 (below that altitude); in 0x0E, while mission flag 2 is set, the same check only tints the screen |
| 0x06 | height < 0xFF600000 |
| 0x2C | height < 0xFFA00000, or x and z inside a box |
| 0x17 | \|x\|, \|z\| <= 0x0946FFFF and height > 0xFC400000 |
| 0x37 | z past 0x11D28000 (0x343F0000 when scene+0x394 is set) |
| 0x2A | low altitude costs health (`0x80076FA4`, every 32/16/8 frames by depth) |

Now both players must meet the condition (climb to the height, reach the
area, ...): when the host meets the scene's condition and the guest, still
flying, does not, the rules run with the guest's position written into the
host's entity for that call, so they see the condition unmet; the host's
position is put back afterwards (unless the rules moved the host themselves).
A guest that was shot down no longer counts. This applies to failure checks as
well (for example "below this altitude" in 0x0E/0x35 needs both players low).
Everything else the rules do stays the host's. The tint phase of 0x0E
is left to each player's own view. Scene 0x2A's damage is per player: the same
check runs for the guest during its own update (with its own state swapped in)
and takes the point off the guest. Not covered by the recordings so far
(scenes 1 and 21): to test in those missions.

### Effects around the player only existed for the host (fixed)

`0x800850D8` (every frame after the player update) makes the effects around
"the player": spray/wake when low (`0x80082E80`), the scene's weather around it
at speed (`0x80084F6C`, reads the speed in the aircraft block), scene 0x2A's
effect (`0x80084E6C`) and a flash (`0x800831D0`, from `0x80090148`; now one
per player, see "the weapon-fire flash" below). Each call
from there is now run a second time for the other player: slots 0 and 1
exchanged, the other player's aircraft block swapped in, the entity arguments
pointed at the other entity. In the guest's view replay the other player is
the host (its block comes from the saved RAM), so each picture shows both
planes' effects. The two recordings do not reach these effects (no changes to
the simulation in them); host's and guest's computers byte-identical.

### Warnings followed the host on both screens (fixed)

Browser test: the host's warnings (off course, missile, the blue autopilot
text) showed on the guest's screen too, and the guest never got its own lock-on
or PULL UP warning. The warning banner is picked each frame by `0x80042544`,
called from the HUD preparation `0x8003A0B4` (the drawing part of the frame, so
it already runs in the guest's view replay with the guest's data): in order,
missile close / locked on (bits 1 and 0 of `0x800C6ECC`), off course
(`0x800476F0` on slot 0), altitude ceiling, PULL UP (30 frames too low, counted
at gp+0x44C), and states read from the input map (scene+0x128/0x12C, per
player), such as the autopilot. `0x8004287C` keeps the chosen warning at
`0x800C7180` (number, time left, two animation records) until it runs out,
replaced only by a more urgent one. That record was shared, so the host's
warning, decided in the main pass, stayed up in the guest's replay, and the
guest's own was thrown away with the replay's memory.

- `0x800C7180` (0x10 bytes) and gp+0x44C are now per-player HUD state
  (`hud_static`, kept across frames like the lock-on progress).
- Warnings the mission rules raise themselves (`0x8004287C` called from outside
  `0x80042544`) are given to both players.
- The lock-on alert: an enemy weapon (`0x800535D4`, its target slot at +0x5B)
  sets bit 0 of `0x800C6ECC` half-way through its lock whoever it aims at, and
  the AI dispatcher `0x8003809C` clears it each frame. That word is per-player
  memory; the bit now goes to the player aimed at. Bit 1 (missile close) is
  computed by the targeting step, which already runs for each player.

Checked: in the NF recording the host's WARNING and red missile HUD (fields
10,600-10,640) no longer show on the guest; with a scripted guest diving into
the ground (`gdive.ac3i`, port 2 stick forward for fields 7,000-7,100) the
guest gets CAUTION and BODY DAMAGE on its own screen only. Host's and guest's
computers byte-identical (X, NF and the dive). Not reproduced on the bench: the
autopilot text and off course (same mechanism), and a lock-on aimed at the guest.

### The guest never got MSSL ALERT (fixed 2026-10-30)

Next browser test: autopilot text and the other warnings were right, but the
guest never got the missile alert. The missile manager (`[gp+0x50C]`) records
incoming missiles per target slot: word +0 bit n for slot n, byte +8+n the
latest missile at slot n, +0x48+n missiles fired by n, +0x88+n missiles homing
on n (filled in by `0x800685C0` when a missile is launched at a handle).
Everything that warns "the player" reads slot 0's: the targeting step
(`0x8003ADB4`, sets bit 1 of `0x800C6ECC` when the missile is close, i.e. MSSL
ALERT and the red HUD) and the HUD (`0x8003DCF0`, `0x80067144`). Missiles fired
at the guest were recorded under slot 1 and never looked at. `coop.c` now
exchanges the slot 0 and slot 1 entries while the guest is "the player" (its
update and its view replay). Checked on the X recording, where enemies fire 6
missiles at the guest: the guest gets MSSL ALERT and the red HUD (fields
3,243-3,355 and the other launches), the host does not; in NF the host's
WARNING and red HUD stay on the host's screen only. Host's and guest's
computers byte-identical (X, NF).

The "WARNING" lock-on banner comes from ships' and ground weapons (`0x800535D4`,
see above); enemy aircraft do not raise it in single player either. A lock by
such a weapon on the guest has not been reproduced on the bench.

### Ground and ship weapons never fired at the guest (fixed 2026-10-30)

User report and recording (Megafloat, `coop_20261030043137.ac3i`): the guest
flies over the SAM and missile sites, which never fire at it; the host gets
fired upon there. Three things were in the way:

1. **The sites were asleep.** Each frame `0x80051498` clears the active bit
   (0x8000) of the ground and ship objects (slots 0x20..0x3F, table at
   gp+0x2D8) and sets it again when their target record says "near the player"
   (bit 0x80 at record+0xC), like `0x80038100` does for aircraft. The update
   (`0x80051414` -> `0x80051A54` -> `0x80052FA8`) skips inactive objects, so
   with the host far away the sites near the guest only ran in the guest's
   view replay (whose memory is thrown away). The activation intercept now
   covers `0x80051498` too: a site near either player is active.
2. **Target choice.** With no target (+0x5B = 0xFF) a weapon asks
   `0x80046124`, which only offers slot 0 ("the player", if it has a free
   attacker place) or a non-player opponent.
3. **Range check.** For "the player" the weapon compares its range with the
   distance in its target record (`0x800C6ED0` + 4 * slot points at
   record+0xC, distance at +4); for any other target it adds up
   |dx|+|dy|+|dz|, up to 1.7 times larger. A SAM with range 6,000 dropped the
   guest again in the same frame it picked it.

For 2 and 3, a weapon with no target and the guest nearer than the host, or
already aiming at the guest, now runs as if the guest were the player: the two
players exchanged (slot table, attacker table), its target 0 instead of 1, the
guest's copy of its target record in place; afterwards its target is turned
back into the real slot (1). The lock-on alert from such a weapon goes to the
guest. A weapon aiming at one player switches to the other when that one is
under 60% of the distance and has a free attacker place (`ai_switch_core`, as
for aircraft). The attacker-place limit (one or two attackers per player) still
applies to each player, as in single player.

Checked with the user's recording: sites 0x20 and 0x26 pick the guest (fields
4,452 and 4,482), site 0x26 locks on (lock-on alert to the guest at 4,496) and
fires at 4,512, site 0x25 at 5,480; the guest gets MSSL ALERT, the red HUD and
damage, the host's screen stays clean. Host's and guest's computers
byte-identical (this recording, X).

### Missiles fired at the guest flew at the host (fixed 2026-10-30)

Next browser test: the SAMs fire and the guest gets the warning, but the
missile tracks a point well away from the guest. The launch (`0x800685C0`,
a3 = target handle) looks the target up in the slot table by the handle's slot
and keeps pointers to its position and velocity (missile record +0x74/+0x78),
which the missile homes on. Handles are real (the guest's is slot 1), but the
weapon's turn runs with slots 0 and 1 exchanged, so the lookup found the
host's entity: the missile counted as the guest's (warnings) and flew at the
host's position. `coop.c` now puts the slot table straight for any launch made
while the players are exchanged (ground weapons and the guest's group of the
aircraft AI). Checked on the Megafloat recording: the missile launched at the
guest at field 4,512 points at the guest's position (before: the host's).
Host's and guest's computers byte-identical (Megafloat, X).

### Audit: other enemy code that runs with the players exchanged (2026-10-30)

After the missile fix, a debug build compared RAM before and after every
stretch of code that runs with slots 0 and 1 exchanged (the guest's group of
each aircraft AI loop, ground and ship weapons aiming at the guest, the guest's
own update) and reported any new pointer into either player's entity left
behind, i.e. any other place that, like the missile launch, would keep the
wrong player. Megafloat (user recording), X (bombers, fighters firing at the
guest) and nfcopy (ships firing at both planes): nothing in the AI or weapon
runs; in the guest's update only its own missiles' launcher pointers, which
are correct. Also checked: the SAM missile fired at the guest at field 4,512
closes on the guest steadily (10,700 to under 600 units in 160 fields); the
guest takes gun/flak damage from the sites (health 30 -> 22).

Not exchanged, so unaffected: the camera-distance pass `0x8004833C`, the
radio/voice pass `0x80049438`, and the destroyed-enemy pass `0x80046600`
(kill credit by +0x50, last attacker).

## The other player as an ally on screen (2026-10-30)

Neither player showed on the other's radar or had a label: the radar and the
target labels only cover slots 2..63, and the players (slots 0 and 1) have no
target record.

- **Radar.** The blips are 12-byte entries per slot at `0x800C6A60` (+0 flags,
  bit 0 shown; +2 kind: 1 enemy red, 2 ally blue, 3 yellow; +4 heading; +5
  above/level/below; +6/+7 position; +8 distance), filled by `0x8003DA08`
  (from the radar view in the scratchpad at [gp+0x43C] and the range at
  `0x800C6D7C`) and drawn one by one by `0x8003DFF8` from `0x8003DE5C`. After
  the radar is drawn, `coop.c` fills slot 1's unused entry for the other player
  the same way and draws it, in yellow so it stands apart from the mission's
  allies.
- **FRND label.** `0x8003B34C` projects targets onto the screen and
  `0x8003F520` labels them (allies: pilot name from the mission's name table,
  `0x8004084C`, then FRND under it, `0x80040740(3, left, bottom)`; kind 1 is
  TGT, 2 N.TGT). Right after `0x8003B34C`, while the GTE still holds its camera
  matrix, the other player is projected the same way (`0x800A7358`
  RotTransPers, `0x8003B758` size), and after `0x8003F520` a FRND label is
  drawn under it when it is on screen. No name: the name table only holds the
  mission's characters.

Both are drawn in each player's own view (the host's in the frame, the
guest's in its replay). Checked: the yellow blip on both radars (X recording)
and the FRND label on the host seen from the guest (`gloop.ac3i`, fields
7,819 and 7,841); host's and guest's computers byte-identical (Megafloat, X).

## Fourth browser test (2026-10-30): kill text, screen shake, weapon flash

Found in the user's recording `coop_20261030201238.ac3i` (convoy/city
mission; on the bench as `runs/B.ac3i`, run with `--coop-p2` on itself).

### BINGO / BULLS EYE showed for the host's kills only, on both screens (fixed)

The big text in the middle of the screen is `FN_SHOW_MESSAGE` (`0x8004CEB0`,
message n of the table at `0x800B1A10`: 6 BINGO, 7 BULLS EYE, 0x2E TARGETED,
0 MISSION ACCOMPLISHED, 4 ENGAGE, 0xE HOLD YOUR FIRE, ...). Its state
(`gp+0x478` -> 0x120 bytes: letters, timer, `+0x11C` the message number) is
ticked and drawn in the flight frame's drawing (`0x800337BC` ->
`0x8004C998`), so the guest's view replay drew the host's message.

A kill message is shown when an aircraft (`FN_AI_FIRST`, call at
`0x800467E8`) or ground object (`0x800520C8`, call at `0x80052230`) goes down
with `SLOT_FLAGS` (`0x800C6FD0` + 4 x slot) bits 1 and 0x800 set, and its last
attacker (entity `+0x50`, a handle) in slot 0. The guest's kills have slot 1
there, so nobody got a message. (In the recording: slots 5 and 8 shot down by
the guest at fields 4,519 and 5,405, slots 9 and 10 by an ally, slot 7 by the
host at 10,991.)

Now the message state is part of each player's HUD state (`coop_hud_swap`):
- the mission's messages are raised on both copies (the second time without
  the sound), and the guest's view replay leaves them alone;
- HOLD YOUR FIRE (from the HUD's own code) goes to whoever's HUD raised it;
- the host's kills stay on the host's copy (the replay skips them);
- the guest's kills are found in the view replay with the game's own checks
  (attacker in slot 1) and shown on the guest's copy.

`AC3_COOP_DEBUG=1` logs each copy's message number as it changes: on B, the
guest's 7 at 4,519 and 5,405, the host's 7 at 10,991.

### The speed screen shake followed the host (fixed)

The shake (`world+0x28C`, 8 bytes) is cleared by the flight frame before the
tick and added to by `0x8006CB44` from the player update (speed, damage). It
was host-only memory, so the guest's own shake was thrown away and the
guest's view showed the host's. Now the guest's update starts from the value
before the host's update, its result is kept (`G.shake`) and put into the
guest's view replay. On B: the host's shake at 3,945..4,185 (guest's none),
the guest's at 6,237.. (host's none).

### The weapon-fire flash appeared on both planes (fixed)

Firing some weapons (the pulse laser, kind 5) makes the player update call
`0x80090110`, which starts a flash: one state for the whole game at
`[gp+0x41C]+0x3948` (frame count, kind | duration << 16). `0x80090148`, from
`0x800850D8`, ticks it and draws it at slot 0's aircraft (`0x800831D0`). The
effects change above ran the drawing for the other player too, with the same
state, so the host's laser flashed on the guest's plane, and the guest's
firing would have started the one shared flash.

Now each player has their own: the guest's is swapped in during its update,
and `0x80090148` runs a second time for the other player with that player's
flash (the view replay uses the copies kept at the snapshot). On B the host's
laser (field 7,679, kind 5) leaves the guest's flash at 0.

Host's and guest's computers byte-identical on B, X, the Megafloat recording
and `gloop`.

### A box round the other player (2026-10-30)

The FRND label had no box. The game draws a target's box from its HUD record
(`FN_DRAW_BOX`, `0x8003FA80`, called by the label loop `0x8003F520` for box
style 1, in the label's colour): an ally's record has byte 0 = 1, the box's
half size and centre at `+6`, `+8`, `+0xA`, and flag `0x100` at `+0xC` (a whole
box instead of four corners); blue is colour `0xFF4000`. Slot 1 has no
record (`0x800C67A0 + 4` is 0), so after the label loop the other player is
given one for the call (on the stack, from the same projection as the
label), the box is drawn, and the record pointers are put back. Checked in
the guest's view (`gloop.ac3i`, 7,819 and 7,841); host's and guest's
computers byte-identical (B, gloop).
