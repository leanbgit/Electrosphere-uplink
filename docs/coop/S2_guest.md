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

## Known gaps (next steps)

- Confirm guest missiles hit and get credited (find the missile pool; kill credit).
- Guest death: what happens when slot 1 is shot down (mode changes, respawn rules).
- The guest uses the host's aircraft model and loadout (copy of the host).
- Mission end / next mission: the guest is recreated when the host's handle
  changes; check across a whole session and in other mission types.
- Only the flight tick variant `0x80032F3C` has been exercised.
- Player mode is not kept in sync after creation (end-of-mission mode 4,
  scripted sections).
- S3: the guest's own view (camera + HUD).
- **Each player shown to the other as a named ally** (requirement, 2026-10-28):
  the host sees the guest's plane with the guest's name, like the allied NPCs
  (e.g. "F15XA / Keith" with a FRND box and a friendly radar marker), and the
  guest sees the host the same way. First test: giving the guest entity the
  type word of the allied NPC in slot 3 (0x004) does **not** create a label, so
  the labels come from a separate list of named friendly units (mission data
  or a HUD target list), still to find. Plan: find that list and the name
  table, add an entry for the other player with their name (the English font
  is plain ASCII), keep the guest's name in the session settings.

## Bench recipe

```sh
python3 tools/coop/make_p2.py p2.ac3i 30000            # scripted second pad
python3 tools/coop/make_p2.py p2copy.ac3i --copy REC   # port 2 = REC's port 1
AC3_COOP_DEBUG=1 tools/coop/replay.sh G REC 9000 --coop-guest --coop-p2 p2.ac3i \
    --coop-ents 6000-9000:150 ents.txt
```

`AC3_COOP_DEBUG=1` prints the guest's input map, state block and entity every
30 updates; `AC3_COOP_NOUPDATE=1` creates the guest but never updates it.
