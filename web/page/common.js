// common.js -- shared by the page's modules (web/page/): the test-hooks flag,
// element lookup, the log in Diagnostics, and the run state.
export const DEV = true;    // site/ (build.sh): false (test hooks off in the website)

export const $ = id => document.getElementById(id);

const logEl = $('log');
export function log(t, cls) {
  const line = document.createElement('div');
  if (cls) line.className = cls;
  line.textContent = t;
  logEl.appendChild(line);
  if (logEl.childNodes.length > 2000) logEl.removeChild(logEl.firstChild);
  logEl.scrollTop = logEl.scrollHeight;
}

// The state several modules read and change.
export const app = {
  running: false,            // a game is running (game.js)
  online: null,              // the online co-op session of the running game, or null (game.js, online.js)
  lobby: null,               // in an online room: { canStart, hint, go } (online.js); Play becomes Play Online
  padView: null,             // the controller, shared with the game worker (written by input.js)
  renderWorker: null,        // the enhanced renderer's worker, or null (game.js)
  hdSet: null,               // live graphics settings shared with the game: [0] bits, [1] version
};
