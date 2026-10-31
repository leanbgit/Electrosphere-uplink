# Spike S4: the guest's own plane and weapons

Goal: the guest picks its own plane and weapons for each mission, from the
same list the host gets. Investigated 2026-10-30 with the user's recording
`coop_20261031004730.ac3i` (bench: `runs/P.ac3i`; simulation mode, mission
Awakening, fourth plane in the list, both weapon slots changed).

## What the hangar hands to the mission

Only four values in the scene (`0x800BF2D0`):

| Where | What |
|---|---|
| `+0x38E` (halfword) | the plane (0..36; 1 at the start of the hangar, 4 in the recording) |
| `+0x3A9` | gun choice |
| `+0x3AA`, `+0x3AB` | the two missile slots' choices |

The default is 1,1,1; the recording picked 3,4,4 (cannon, M.I.R.V.).

Mission start uses them:
- `0x80032CB8`: the player's aircraft type (entity `+0x48`) = table
  `0x800234F8`[plane] (identity for 0..30, then 31..36 -> 4, 6, 16, 20, 22,
  23).
- `0x8006CE70` (player setup, called once at `0x80032CF4`): the plane's
  figures record from `0x8006CD6C(type)` goes to `[gp+0x524]+0`; health,
  `+8`, `+0xC`, `+0x10`, `+0x1C` (missiles) come from it; `+4` =
  min(figures `+0x7C`, `0x800B3B28`[missile choice]). It also allocates the
  player's blocks (`0x8006CCE0`: gp+0x514, +0x51C, +0x518, +0x520, +0x524 and
  `0x800BD0A0`, 0x10C bytes in all).
- `0x80035F18`: loads plane file 66 + plane (table `0x800B0B84`; 37 files)
  into the heap (`0x80027410`, a bump allocator with its top at scene `+0x1C`
  and a fixed limit of `0x801FB700`). The file is an archive: sounds go to
  the sound chip (`0x8002D96C`), textures to video memory (one texture page,
  at (704, 256) in Awakening), and the model stays in the heap (about 24 KB).

Every frame: the player update (`0x80072618`) and the HUD (`0x8003EF50`) read
the weapon choices; the effects (`0x80085148`, `0x80085368`) read the plane.

## Step A: the guest's own weapons (done)

The three weapon bytes are a per-player range (`PP_WEAPONS`), swapped in for
the guest's update and view replay like the rest. When the guest is created,
its choice replaces the host's in its copy and `[gp+0x524]+4` is worked out
again with it. Until the guest has its own hangar turn the choice comes from
`--coop-guest-weapons G,M,S` (the test page: `coop.html?guestWeapons=G,M,S`).

Checked on P with the guest at 1,1,1 and the host at 3,4,4: the guest's HUD
shows the standard missile and the guest fires standard missiles while the
host keeps M.I.R.V.s; host's and guest's computers byte-identical. Without
the switch, X and B are byte-identical to the previous build.

## The hangar (simulation mode)

An overlay at about `0x800D0000..0x80110000`; its state is a struct pointed
to by `[0x80112E10]` (`0x801846F0` in P), halfword `+0` the screen:

| state | screen |
|---|---|
| 0 | set up (field 1083) |
| 1 | plane list (cursor at `0x80112EF4`) |
| 2, 3 | plane confirmed (`0x800FA5E0` writes scene `+0x38E` from `0x800FE8D8(mission, ..)`; default weapons from `0x800FEAD4`) |
| 4 | weapon select |
| 5 | weapons confirmed (`0x800F9B3C`.. writes scene `+0x3A9..0x3AB`), then the mission loads |

The weapon screen's cancel goes back to state 1 (`0x800FA9C0`, `0x800FA9C8`).

## Step B: the guest's own plane number (done)

What the player setup does with the plane (`0x8006CF90..0x8006D034`) is done
again for the guest when it is created (`guest_loadout` in `coop_guest.c`):
its aircraft type (entity `+0x48`) from the table at `0x800234F8`, its
plane's figures record from `0x8006CD6C(type)` into its copy of
`[gp+0x524]` (`+0` record, `+8` = record `+0x74`, `+0xC` = `+0x80`, `+0x10` =
`+0x82`, `+0x14` health = `+0x84` (30 when scene `+0x394` >= 2, also entity
`+0x4E`), `+0x1C` missiles = `+0x78`), and `+4` from the missile choice. The
plane number itself (scene `+0x38E`) is a per-player range (`PP_PLANE`); the
effects around the other player swap it in with the other player's block.

Checked on P with the guest on plane 25 and the host on plane 4: the guest's
HUD shows plane 25's icon, it carries plane 25's missiles (32, the host 112)
and flies at different speeds (1,729 against 1,067 at the same moment). It
still looks like the host's plane (step C).

Test switches: `--coop-guest-plane N` (the page: `coop.html?guestPlane=N`).

## The guest's turn in the hangar (done)

`coop_hangar.c`. The hangar's screens run from a table at `0x8010ECF8`
indexed by its state:

| state | handler | |
|---|---|---|
| 0 | `0x800F9E68` | enter the plane list (+2 = 0, state 1, fade in) |
| 1 | `0x800F9EC8` | plane list; leaving sets state 2 with +2 = 1 (confirm) or 2 (cancel, `0x800FA4C8`) |
| 2 | `0x800FA4E4` | leaving the plane list: confirm -> plane set (`0x800FA5E0`), cancel -> out of the hangar |
| 3, 4 | `0x800FA860`, `0x800FA8E0` | weapon select; leaving sets state 5 (+2 = 1 confirm, 2 back) |
| 5 | `0x800FAA48` | after the fade: confirm -> `0x800F9B10` (weapons into the scene), menu request (`[0x80112DC4]+0x84` = 3, 0), free the hangar (`0x80027538`), next screen (`0x800F7524(3, 0)`) |

In co-op, when the host confirms (state 5), the commit runs and the host's
choice is kept; the menu request is put back, the free and next-screen calls
are skipped, and the hangar goes back to state 0 with controller 2 read as
controller 1 (`ac3_pad_swap` in `psyq_pad.c`, the engine's controller
buffers). When the guest confirms, its choice (scene `+0x38E`, `+0x3A9..`)
becomes `ac3_coop_guest_plane` / `ac3_coop_guest_weapons`, the host's is put
back, controller 1 is controller 1 again, and the hangar goes on to the
mission. A cancel in the guest's weapon select brings back its plane list; a
cancel in its plane list hands the turn back to the host (the host's plane
list, with the host's choice put back; the host confirms again, then it is
the guest's turn again). The overlay's addresses are shared with other overlays, so its code is
checked (first instructions of `0x800FAA48` and `0x800F9B10`) before
anything is done.

Bench recordings with a scripted guest turn inserted after the host's
confirm (`runs/PH`, `PX` (with a cancel), `C2H` (campaign)): the guest gets
the plane and weapons it picked, the host keeps its own, both computers
byte-identical. Recordings made without a guest turn replay with
`--coop-no-hangar-turn`.

The test page says whose turn it is (from the `coop: hangar:` log lines).

## Step C: the guest's own plane model (done, original graphics)

`coop_plane.c`, plus two engine additions.

**Extra memory.** 512 KB at `0x80200000`, past the PS1's 2 MB
(`ac3_ext`, `runtime/ac3_cpu.c`): reached through the slow memory path, so
normal accesses are unchanged; the library functions' pointer check
(`guest_ptr`, `hle.h`) knows it too. Only co-op uses it (the game's heap ends
at `0x801FB700` and it never goes further). The bench's memory hash includes
it.

**Second video memory bank** (`gpu_sw.c`, the original-graphics renderer).
1024x512 more, which the game never addresses: a polygon reads its texture
page from it when bit 11 of its texture page is set (the bit Sony's
development units used for their larger video memory) and its colour table
when bit 15 of its colour table is set (unused). The bank bit does not leak
into the drawing mode that sprites use. The bench's video memory hash
includes it.

**Loading.** When the host's plane file is queued (`0x80035F18`) and the
guest picked another plane, the guest's file (66 + plane) is queued too
(`0x8002C7AC`, no callback), into the extra memory; the read queue's "last
request" (`+0x17C`, `+0x180`) is put back so the loader waits for the host's
file as usual. When the guest is created, the file is taken apart as
`0x80035F98` does, with the game's own functions: relocated (`0x80027338`),
its pictures loaded (`0x800272F0`) with LoadImage sent to the second bank
(same places as the host's: page (704, 256), colour tables (336, 312..319)),
entries 0 and 1 decompressed (`0x800AFD08`) into the extra memory, the parts
table relocated. Not loaded: its sounds (the guest's plane sounds like the
host's).

The model's faces are 0x24-byte records with PS1 polygon words: uv0 +
colour table at `+0x0C`, uv1 + texture page at `+0x10`. Every face using the
plane's page and colour tables gets the bank bits (191 faces for the F-22C).

**Drawing.** `0x80067494` (from `0x80068334`, from `0x8003342C`) draws both
players' planes from a list (16-byte items; slot 0 has flag 2, the other
player flag 4). For each it copies pointers out of the player's parts table
(the draw table's entry 3, `[gp+0x2DC]+8`) into a record: the player's
(argument 3) or the other player's (first stack argument). Afterwards the
guest's record gets the matching pointers from the guest's table (host's
frame: the other player's record; guest's view replay: the player's), using
a map built when the guest's plane was loaded (12 parts x 4 pointers). The
model's second object (`[gp+0x2DC]+0xC`) is the guest's during its update and
its view replay.

Checked with a scripted guest turn (host F-15S/MT, guest F-22C Raptor II):
the guest is drawn as the F-22C with its own colours in both views (matching
the F-22C flown by the host on its own); host's and guest's computers
byte-identical, extra memory and second bank included.

## Still to find

- The HD and WebGPU renderers do not have the second bank yet: there the
  guest's plane has its own shape but the host's textures.
- The guest's plane sounds like the host's (its sounds are not loaded).
- The HUD's small plane picture is drawn from the host's file in the guest's
  view too.
