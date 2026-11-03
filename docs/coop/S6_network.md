# S6: playing online

Two players, each on their own computer with their own discs. The host
creates a room and shares its code, or makes it public. The guest joins, and
both games start together from power-on with the host's memory card and the
host's graphics settings. From then on only the two controllers travel
between the computers, every field.

## Rooms (`server/`)

A Cloudflare Worker serves the site and, at `/api/ws`, the room server
(`server/src/index.js`). It never carries the game: it hands out codes, lists
public rooms, passes join requests and the connection offers along, and tells
each side when the other leaves.

- **Code** `NEMO-XXXX` (4 letters or digits, no 0/O/1/I). Joining with the
  code is immediate: knowing it is the host's permission.
- **Public room:** listed with the host's name, but not its code. Joining
  from the list asks the host (Accept / Decline).
- **One guest per room.** A room with two players, or whose game has
  started, leaves the list.
- **The site version** (hashes of `ac3.wasm`, `worker.js`, `netplay.js`) must
  match, or the join is refused.
- **Hibernation API:** everything about a connection is in its attachment, so
  the Durable Object can sleep between messages.

Checked: `server/test/rooms.test.js`, 17 checks on Cloudflare's local
runtime (`wrangler dev`).

## The connection and the set-up (`web/online.js`)

1. **Direct connection:** after the room server says both are in, the
   browsers connect directly (WebRTC data channel, reliable and ordered). The
   STUN servers are Cloudflare's and Google's. There is no relay yet: if no
   direct path exists, the page says so after 20 s.
2. **Hello** (text messages): each side sends its site version, which discs
   it has, the language, and whether it has WebGPU. The host refuses a guest
   with a different version, no disc 1, a missing disc 2 (when the host has
   it) or a different language.
3. **Set-up:** the host sends its graphics settings (resolution and option
   bits) and its memory card (base64, 128 KB).
   - The guest refuses if the host plays Enhanced with widescreen or
     atmosphere (they change the game itself) and the guest's browser has no
     WebGPU.
   - Otherwise the guest answers "ready".
4. **Start:** the host presses Start. The input delay comes from the median
   of 5 round trips: half of it plus 10 ms, in fields, plus 1, between 3 and
   15. Both games then start (`startGame` in `app.js`) with:
   - `net: {role, delay, inbound}`;
   - the host's card. The guest's game never saves to the guest's browser;
     the host's saves as usual;
   - the host's graphics, locked for the session on both sides.

   The host's game runs with `--coop-guest`, the guest's with `--coop-guest
   --coop-view`: the guest sees its own view and hears its own sound (S3, S5).
5. **During the game:** binary messages carry the game's records (below).
   The page writes them into a shared ring for the game worker.
6. **Leaving:** Stop or Leave sends a goodbye. A closed connection counts as
   one too. The other game then stops waiting and plays on with a neutral
   controller for the player who left.

Checked: `server/test/online.test.js` drives two players on the real page in
headless Chromium against `wrangler dev`:

- private room by code, and public room with the host's approval;
- direct connection and the checks;
- the card arrives byte for byte;
- both games start with the same delay and settings;
- game records go both ways;
- the goodbye reaches the other game.

The game itself can't start there (no discs), so the page's start function is
replaced by a stand-in.

## Who has the controls (on the game screen)

The game reports which phase the session is in (`ac3_coop_phase`, `coop.h`)
whenever it changes; `online.js` dims the game screen and shows a line of
text:

| Phase | Guest's screen | Host's screen |
|---|---|---|
| menus, briefings, the host's hangar turn | dimmed: "<host> has the controls" | normal |
| the guest's hangar turn | "Your turn: choose your plane and weapons" (not dimmed) | dimmed: "<guest> is choosing a plane and weapons" |
| a mission, before the guest's plane joins | dimmed: "You join when <host> gets control of their plane" | normal |
| flying | normal | normal |
| the guest shot down | dimmed: "Shot down: watching <host> until the mission ends" | normal |
| the host shot down, the guest flies on (S7_host_down.md) | "<host> was shot down. The mission is yours now!" for 5 s | dimmed: "Shot down: watching <guest> fly on until the mission ends" |

While a game waits for the other player for over a second, both screens
are dimmed with "Waiting for <name>... N s".

## Lockstep (`web/netplay.js`, game worker)

- **Inputs:** each field the game reads the controllers once
  (`web_input_poll`). This player's controller is sampled, tagged for field
  F + delay, sent, and used at that field on both computers. Controller 1 is
  the host's, controller 2 the guest's. If the other player's input for this
  field hasn't arrived, the game waits for it (the page shows "Waiting for
  ..." after 1 s).
- **Before the first input arrives:** for the first `delay` fields both use
  a neutral controller.
- **Desync check:** every 60 fields (about once a second) both send the
  fingerprint of the shared game state (`coop_sync.c`, "sim"). A difference
  is reported on the page with the field number.
- **Records:** four int32 each: kind (1 input, 2 fingerprint, 3 goodbye),
  field, a, b.

Checked with two runs of the web build under Node, connected by local TCP
(`platform/web/run_worker_node.js --net-role host|guest`, transport in
`net_io_node.js`). Each run takes only its own player's controller from a
recording.

| Recording | Fields | Result |
|---|---|---|
| `PH` (guest's hangar turn, own plane) | 3,336 | both identical, and identical to the native bench's replay of the recording, on every field |
| `X` | 10,024 | the same |
| `PH`, one side started with a different option on purpose | 3,336 | both report the desync at field 1,440 (the games went apart at 1,409) |
| `PH` at real speed | 1,800 | no waits over 1 s |

## Not yet

- **Joining a game in progress:** not possible yet. A guest must be in the
  room before the host starts (savestates, Phase 7).
- **Relay (TURN):** none yet.
- **Enhanced graphics for the guest's view:** not ready. Its sky, fog and
  sun still use the host's camera (the parked HD co-op work).
- **A desync is only reported.** Repairing one needs savestates.
