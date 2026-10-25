# Putting Electrosphere Uplink on the web

The website is the `site/` folder (built by `platform/web/build.sh public`):
about 630 KB of static files. It contains **no game code and no game data**:
players select their own disc images, which never leave their browser.

## Requirements of the host

- Serves static files over **HTTPS** (any custom domain or the host's own).
- Ideally sends two headers that let the page use shared memory:
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`.
  `site/_headers` declares them for Cloudflare Pages and Netlify.
  Hosts that ignore `_headers` (e.g. GitHub Pages) still work: `coi-sw.js`
  adds the headers from inside the browser, at the cost of one automatic
  reload on the first visit.

## Option A: Cloudflare Pages (recommended)

1. Create a free Cloudflare account and open **Workers & Pages**.
2. Create a new **Pages** project using **direct upload** (no Git needed).
3. Name it (the name becomes `<name>.pages.dev`, e.g. `electrosphere-uplink`).
4. Upload the **contents** of `site/` (the files, not the folder around them).
5. Deploy. To update later, upload a new `site/` as a new deployment.

## Option B: Netlify

Drag the `site/` folder onto Netlify's manual deploy page. `_headers` is used
the same way.

## Option C: GitHub Pages

Put the contents of `site/` in a repository and enable Pages for it. The
headers come from `coi-sw.js` (first visit reloads once).

## After deploying: check

1. Open the site in Chrome or Edge. Open **Diagnostics** at the bottom: there
   must be no red line about cross-origin isolation.
2. Select both discs, press Play, reach the title screen.
3. Reload the page: the discs show as "remembered in this browser".

## Project NEMO's English patches

Included with permission (v0.9b3): `site/patches/CD1.xdelta` and `CD2.xdelta`.
`build.sh` copies them from `web/patches/`, where they are part of the
repository. The page uses them automatically; players only choose their discs.
Keep the credit in the page's About section.

## Before announcing it publicly

- Keep the footer notice (not affiliated; you need your own copy).
