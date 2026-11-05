# The website and its room server (Cloudflare)

One Cloudflare Worker serves the website's files (`site/`) and the room server
for online co-op (`/api/ws`, `src/index.js`). The rooms are kept in one
Durable Object (SQLite-backed, which the free plan allows).

The room server only introduces the two browsers. The game itself goes
directly between them over WebRTC (`web/page/online.js`, `web/netplay.js`,
`docs/coop/S6_network.md`).

## Publish

1. Build the site: `platform/web/build.sh` makes `site/`.
2. Deploy, from this folder:
   ```sh
   npx wrangler login      # once: opens your Cloudflare account in the browser
   npx wrangler deploy
   ```
   The site is then at `https://electrosphere-uplink.<your-subdomain>.workers.dev`.
   You can add your own domain in the Cloudflare dashboard (Workers, then the
   worker, then Settings, then Domains & Routes).

If the site is on Cloudflare Pages today, this Worker replaces it. It serves
the same files with the same `_headers`, and online co-op needs the room
server at the same address as the page.

## Test locally

```sh
platform/web/build.sh
cd server && npx wrangler dev        # http://localhost:8787
```

Open http://localhost:8787 in two browser windows. `web/serve.py` (or
`serve.bat`) has no room server, so online co-op needs `wrangler dev` or the
deployed site.

Automatic checks, against a running `wrangler dev` (they need `npm i ws
playwright`):

- `node test/rooms.test.js` checks the room protocol: codes, private and
  public rooms, the host's approval, full rooms, versions and leaving.
- `node test/limits.test.js` checks the limits: wrong codes, messages per
  second, connections per address, people waiting, other sites refused,
  names cleaned.
- `node test/online.test.js` drives two players on the real page in headless
  Chromium: room, direct connection, checks, memory card, start, game
  records both ways, goodbye.
- `node test/peer.test.js` plays a modified guest and modified hosts on the
  real page: what each side refuses from the other.

`limits.test.js` sends 200 messages in one go on purpose; run it at most
about once a minute, or its per-address checks see the previous run.

## Limits

- **Abuse limits** (`LIMIT` in `src/index.js`):
  - 20 messages a second per connection, bursts of 60;
  - 5 wrong codes a minute;
  - 6 connections per network address;
  - 3 people waiting per room.

  Cloudflare's free plan has a daily allowance for the room server (100,000
  requests, with WebSocket messages counted 20 to 1). Once the site is on
  your own domain, a Cloudflare rate-limiting rule for `/api/ws` adds a second
  layer in front of the Worker (the free plan includes one rule; rules apply
  to domains, not to `workers.dev`).

- **No relay (TURN) yet.** Two browsers that cannot reach each other
  directly (some strict home routers, many mobile and university networks)
  don't connect, and the page says so. A relay can be added later in
  `web/page/online.js` (`ICE`) without other changes.
- **One lobby** for all rooms. That's plenty for this site.
