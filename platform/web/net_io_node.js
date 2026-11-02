// net_io_node.js -- the online co-op transport for run_worker_node.js (testing
// web/netplay.js without browsers): a TCP connection between two runs, on its
// own thread, because the game blocks the main thread while it waits for the
// other player's controller. Records from the other run go into the shared
// ring (NetRing); records from the game arrive as messages.
'use strict';
const { workerData, parentPort } = require('worker_threads');
const net = require('net');
const { NetRing } = require(workerData.netplay);
let sock = null, pending = [], rx = Buffer.alloc(0);
function frame(buf) { const h = Buffer.alloc(4); h.writeUInt32LE(buf.length, 0); return Buffer.concat([h, buf]); }
function attach(s) {
  sock = s; s.setNoDelay(true);
  for (const b of pending) s.write(b);
  pending = [];
  s.on('data', d => {
    rx = Buffer.concat([rx, d]);
    while (rx.length >= 4) {
      const n = rx.readUInt32LE(0);
      if (rx.length < 4 + n) break;
      const body = rx.subarray(4, 4 + n);
      NetRing.write(workerData.inbound, new Int32Array(body.buffer.slice(body.byteOffset, body.byteOffset + n)));
      rx = rx.subarray(4 + n);
    }
  });
  s.on('close', () => NetRing.write(workerData.inbound, new Int32Array([3, 0, 0, 0])));   // NET_BYE
  parentPort.postMessage({ type: 'connected' });
}
parentPort.on('message', m => {
  const b = frame(Buffer.from(m));
  if (sock) sock.write(b); else pending.push(b);
});
if (workerData.role === 'host') net.createServer(attach).listen(workerData.port, '127.0.0.1');
else {
  const tryConnect = () => { const s = net.connect(workerData.port, '127.0.0.1', () => attach(s)); s.on('error', () => setTimeout(tryConnect, 200)); };
  tryConnect();
}
