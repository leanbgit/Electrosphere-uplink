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
- The guest's targeting uses the per-object tables the world update computes
  relative to the host (0x800C6FD0 etc.): targets far from the host may not be
  selectable by the guest.
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
