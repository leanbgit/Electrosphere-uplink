# The website and its room server (Cloudflare)

One Cloudflare Worker serves the website's files (`site/`) and the room server
for online co-op (`/api/ws`, `src/index.js`). The rooms are kept in one
Durable Object (SQLite-backed, which the free plan allows).

The room server only introduces the two browsers. The game itself goes
directly between them over WebRTC (`web/online.js`, `web/netplay.js`,
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
- `node test/online.test.js` drives two players on the real page in headless
  Chromium: room, direct connection, checks, memory card, start, game data
  both ways, goodbye.

## Limits

- **No relay (TURN) yet.** Two browsers that cannot reach each other
  directly (some strict home routers, many mobile and university networks)
  don't connect, and the page says so. A relay can be added later in
  `web/online.js` (`ICE`) without other changes.
- **One lobby** for all rooms. That's plenty for this site.
