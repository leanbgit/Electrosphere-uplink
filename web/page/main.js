// main.js -- the page (index.html). Each part is a module in web/page/:
//
//   common.js    element lookup, the log, the run state shared by the modules
//   storage.js   what is kept in this browser (memory cards, English data)
//   gamedata.js  the game data streamed from the site, the English patches,
//                and whether the game can start
//   saves.js     the memory card panel
//   graphics.js  the graphics settings (some change live while playing)
//   display.js   the screen
//   input.js     keyboard (changeable keys) and game controllers
//   loader.js    the loading screen
//   game.js      starting and stopping the game and its workers
//   online.js    online co-op: rooms, the connection, starting together
//   tabs.js      the Settings / Play online tabs
import { log, app } from './common.js';
import { cardGet, cardPut } from './storage.js';
import { initGameData, loadRemembered, renderReady } from './gamedata.js';
import { initSaves } from './saves.js';
import { initGraphics } from './graphics.js';
import { initDisplay } from './display.js';
import { initInput } from './input.js';
import { game, initGame } from './game.js';
import { initOnline } from './online.js';
import { initTabs } from './tabs.js';

if (!self.crossOriginIsolated)
  log('This page is not cross-origin isolated, so the game cannot start. Open it through a web server ' +
      '(serve.bat locally, or a host that sends the COOP/COEP headers); opening the file directly does not work.', 'bad');

initGameData();
initSaves();
renderReady();
loadRemembered().then(renderReady);
initGraphics();
initDisplay();
initInput();
initGame();
initOnline();
initTabs();

// for the browser console and the online test (server/test/online.test.js)
self.uplink = { app, game, cardGet, cardPut };
