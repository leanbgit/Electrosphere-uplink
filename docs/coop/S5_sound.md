# S5: what each player hears

Before this, both computers played the game's one sound chip, which is the
host's sound. The guest heard the host's engine, the host's gunfire and the
host's HUD tones, never its own. The host heard the guest's gunfire and
missile launches as if they were its own.

## How the game makes sound

- **One table of 24 channels** in memory, `0x800C5830` (0x14 bytes each), one
  per voice of the sound chip. Byte +1 holds flags; bit 0 asks for a key-on
  at the next sound tick.
- **Requests** are written into the table during the frame by game code:
  - the player update writes the engine (channels 0-2, `0x800301F4`) and the
    player's gun and missile sounds;
  - the rest of the tick writes everything else: HUD tones, explosions,
    enemies;
  - placed sounds for other aircraft go on channels 6-11. Their volume
    depends on the distance from the camera, which the game passes to
    `0x8002F9AC`. Their state is kept at `0x800C6630`.
- **The sound tick** `0x8002E1F4` runs once per frame from the main loop
  (`0x80026208`), before the flight tick. It sends the changes to the chip:
  `SpuSetVoiceAttr`, `SpuSetKey`, `SpuGetAllKeysStatus` and
  `SpuSetCommonAttr`. In flight these are the only sound-chip calls, apart
  from 3 CD-mix switches in a whole mission.
- **What the tick writes besides the table** (measured with `--coop-wfn`):
  `0x800C5800`..`0x800C5814` (the settings it passes to `SpuSetCommonAttr`),
  `0x800BCE99`, `0x800C6550`..`0x800C6553` and its stack.
- **The game reads the chip back every frame**, so the chip must stay
  identical on both computers (S1).

## The ear: a second sound chip

`spu_ear.h`: the sound-chip code (`spu_core.c`, `psyq_spu*.c`) is compiled a
second time under other names (`spu_ear_core.c`, `spu_ear_psyq.c`,
`spu_ear_names.h`). It has its own voices, mixer and sound memory.

- **The real chip** runs exactly as before. The game reads only the real chip.
- **The speakers** play the ear.
- **Copied from the real chip to the ear:**
  - sample uploads into sound memory;
  - CD audio (music, radio);
  - the volume and reverb settings, each frame.
- **Without a guest aircraft** (menus, briefings, spectating), the ear copies
  the real chip's voices just before every mix, so it plays exactly what the
  real chip plays. Checked: 1,753,257 samples, all identical.

## The table as each player hears it (`coop_sound.c`)

At the sound tick, after the real tick, the tick runs a second time with its
chip calls going to the ear. It runs on the table as this player should hear
it. Afterwards the table, the other memory the tick writes and its stack are
put back.

| Sound | Host's ears | Guest's ears |
|---|---|---|
| Engine (channels 0-2) | the host's | the guest's, from its player update (those writes used to be thrown away) |
| New sound from the host's player update (its gun, missiles) | yes | silenced |
| New sound from the guest's player update | silenced | yes |
| Rest of the frame (HUD tones, world) | the host's frame | the guest's view replay's version of each channel it changed. A new sound that only the host's frame made (the host's HUD) is silenced |
| Placed sounds (channels 6-11) | the host's camera | the guest's camera: the replay places them, and their state is kept for the guest between frames |

"Silenced" means the key-on request is removed from the ear's copy and the
ear's voice is keyed off. Which computer is which:

- `--coop-ears host|guest`;
- by default, the guest's computer (`--coop-view` without `--coop-split`)
  plays the guest's ears;
- on the test page, the **Sound** choice.

## Checked (bench, user's recordings `X` and `B`)

- **Game state unchanged** on every field: RAM, scratchpad, video memory,
  the sound chip and the shared simulation. Checked with the host's ears and
  with the guest's ears, on both kinds of computer.
- **Key-ons per channel over 3,600 sound ticks:**
  - In `B` the host fires: its 13 gun sounds (channel 4) and 50 HUD and
    weapon tones (channels 12-13) are silenced for the guest. The guest's own
    tones play on channels 14-16.
  - In `X` the guest fires: its 180 gun sounds are silenced for the host and
    kept for the guest.
- **Placed sounds for the guest:** 21 key-ons against the host's 4 on
  channels 6-8 (other aircraft passing near the guest). No repeated
  retriggering.
- **Cost:** web build under Node, 45.0 s instead of 43.3 s for 10,024 fields
  (about 4%).

## The guest's own plane sounds (2026-11-02)

**What a plane file holds.** Entry 3 of each plane file (66 + plane) holds
its sounds, and every plane is laid out the same way:

- **Header:** first definition number, count, samples offset/4, where they go
  in sound memory, size. `0x8002D96C` copies the definitions into the sound
  definition table (`0x800C5A10`, 0x14 bytes each) and uploads the samples.
- **Two definitions, 84 and 85:** the engine sounds.
- **Samples:** 9-27 KB, always uploaded at `0x496D0`.
- **Not in the plane file:** gun and missile sounds, which are the same for
  every plane.

**How a definition is used.** A channel stores only the number of the
definition it plays (byte 0, `0x8002E514`). When the sound starts, the sound
tick reads the definition for the sample address (+0x10), volume, pitch and
envelope (`0x8002E0D0`).

**What changes for the guest.**

- **Samples:** they go into the ear's memory past the PS1's 512 KB. The ear's
  sample space is 1 MB (`EAR_SAMPLE_SPACE`); the reverb and everything else
  still use the first 512 KB.
- **Definitions:** a copy of the guest's two, pointing to those samples, is in
  the definition table only while the guest's sounds are worked out:
  - its player update (both computers);
  - its view replay;
  - the ear's sound tick with the guest's ears.

  Then the host's are put back (`coop_sound_bank_swap`).
- **The guest's engine restarts** when the guest's ears take over, so it
  starts with its own sample.

**Checked** (`PH`, guest flying plane 6 next to the host's plane 4):

- the game state is identical on every field, on both computers and in the
  web build;
- the guest's audio changes from the field its plane appears and stays
  different.

Clips: `build-check/PH_guest_plane6_engine_before.mp3` and `..._after.mp3`.

## Not done

- **The other player's plane: not checked.** I haven't checked whether the
  placed sounds include the other player's aircraft (it uses the player's
  layout, in slot 1). Each player's own gun and missiles are silenced for the
  other.
- **The other player's plane is not heard as a nearby aircraft** (the user's
  test, 2026-11-02). Not important for now.
