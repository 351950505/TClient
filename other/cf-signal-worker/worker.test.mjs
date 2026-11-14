import assert from 'node:assert/strict';
import fs from 'node:fs/promises';

const NativeResponse = globalThis.Response;
class Socket {
  readyState = 1;
  messages = [];
  send(message) { if (this.readyState !== 1) throw Error('closed'); this.messages.push(message); }
  serializeAttachment(value) { this.attachment = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.attachment); }
  close(code, reason) { this.readyState = 3; this.closeCode = code; }
}
globalThis.WebSocketPair = class { constructor() { this[0] = new Socket(); this[1] = new Socket(); } };
globalThis.WebSocketRequestResponsePair = class { constructor(request, response) { this.request = request; this.response = response; } };
globalThis.Response = class {
  constructor(body, init = {}) { this.body = body; this.status = init.status ?? 200; this.webSocket = init.webSocket; }
  get ok() { return this.status >= 200 && this.status < 300; }
  static json(value, init) { return new this(JSON.stringify(value), init); }
};
const source = await fs.readFile(new URL('worker.js', import.meta.url), 'utf8');
const { default: worker, Admission, SignalRoom } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
class Context {
  sockets = [];
  values = new Map();
  storage = {
    transaction: async fn => fn({ get: async key => structuredClone(this.values.get(key)), put: async (key, value) => this.values.set(key, structuredClone(value)) })
  };
  getWebSockets() { return this.sockets.filter(ws => ws.readyState === 1); }
  acceptWebSocket(ws) { this.sockets.push(ws); }
  setWebSocketAutoResponse(pair) { this.auto = pair; }
}
function request(uid, ip = 'ip1', room = 'test.room:8303') {
  return new Request(`https://signal.example/ws?room=${room}&uid=${uid}&session=0123456789abcdef`, {
    headers: { Upgrade: 'websocket', 'X-Relay-Ip-Hash': ip, 'CF-Connecting-IP': '192.0.2.1' }
  });
}
let passed = 0;
async function test(name, fn) { await fn(); ++passed; console.log('PASS', name); }
const ctx = new Context();
let room = new SignalRoom(ctx);
let a, b;
await test('no secret required; valid welcome and broadcast', async () => {
  assert.equal((await room.fetch(request('aaaaaaaaaaaaaaaa'))).status, 101); a = ctx.sockets.at(-1);
  assert.equal((await room.fetch(request('bbbbbbbbbbbbbbbb'))).status, 101); b = ctx.sockets.at(-1);
  room.webSocketMessage(a, 'S|1|\u27e8B:AAAAAA');
  assert.equal(b.messages.at(-1), 'D|aaaaaaaaaaaaaaaa|0123456789abcdef|1|\u27e8B:AAAAAA');
  assert.match(a.messages.at(-1), /^A\|1\|1\|/);
  assert(!a.messages.some(m => m.startsWith('D|')));
});
await test('duplicates are not broadcast', () => {
  const count = b.messages.length;
  room.webSocketMessage(a, 'S|1|\u27e8B:AAAAAA');
  assert.equal(b.messages.length, count);
});
await test('hibernation reconstruction restores sequence and identity', () => {
  const boot = room.boot; room = new SignalRoom(ctx);
  assert.notEqual(room.boot, boot);
  room.webSocketMessage(a, 'S|2|\u27e8B:AAAAAA');
  assert.equal(b.messages.at(-1), 'D|aaaaaaaaaaaaaaaa|0123456789abcdef|2|\u27e8B:AAAAAA');
  assert.equal(a.deserializeAttachment().lastSequence, 2);
});
await test('uid cannot evict active user', async () => {
  const reply = await room.fetch(request('aaaaaaaaaaaaaaaa', 'ip2'));
  assert.equal(reply.status, 409);
  assert.equal(a.readyState, 1);
});
await test('eight same-address sockets allowed, ninth rejected', async () => {
  const c = new Context(), r = new SignalRoom(c);
  for (let i = 1; i <= 8; ++i) assert.equal((await r.fetch(request(i.toString(16).padStart(16, '0')))).status, 101);
  assert.equal((await r.fetch(request('ffffffffffffffff'))).status, 429);
});
await test('twenty peers supported across address groups, room cap32', async () => {
  const c = new Context(), r = new SignalRoom(c);
  for (let i = 1; i <= 32; ++i) assert.equal((await r.fetch(request(i.toString(16).padStart(16, '0'), 'ip' + Math.floor((i-1)/8)))).status, 101);
  assert.equal((await r.fetch(request('ffffffffffffffff', 'ip9'))).status, 429);
});
await test('bad signal and binary frames rejected', async () => {
  const c = new Context(), r = new SignalRoom(c);
  await r.fetch(request('aaaaaaaaaaaaaaaa')); const s = c.sockets.at(-1);
  for (const signal of ['S|0|\u27e8B:AAAAAA', 'S|3|bad', 'S|9999999999999999|\u27e8B:AAAAAA', 'S|3|\u27e8B:A|AAAA']) {
    r.webSocketMessage(s, signal); assert.match(s.messages.at(-1), /^E\|/);
  }
  r.webSocketMessage(s, new ArrayBuffer(20)); assert.equal(s.closeCode, 1008);
});
await test('message rate persists through constructor restart', async () => {
  const c = new Context(); let r = new SignalRoom(c);
  await r.fetch(request('aaaaaaaaaaaaaaaa')); const s = c.sockets.at(-1);
  for (let i = 1; i <= 6; ++i) r.webSocketMessage(s, `S|${i}|\u27e8B:AAAAAA`);
  r = new SignalRoom(c);
  for (let i = 7; i <= 12; ++i) r.webSocketMessage(s, `S|${i}|\u27e8B:AAAAAA`);
  assert.equal(s.readyState, 1);
  r.webSocketMessage(s, 'S|13|\u27e8B:AAAAAA'); assert.equal(s.closeCode, 1008);
});
function admit(ip, game = 'test.room:8303') { return new Request('https://admission/', { method: 'POST', body: JSON.stringify({ room: game, ip }) }); }
await test('per-address connection attempts bounded', async () => {
  const c = new Context(), gate = new Admission(c);
  for (let i = 0; i < 20; ++i) assert.equal((await gate.fetch(admit('ip1'))).status, 200);
  assert.equal((await gate.fetch(admit('ip1'))).status, 429);
  assert.equal((await gate.fetch(admit('ip2'))).status, 200);
});
await test('admission quotas persist across object reconstruction', async () => {
  const c = new Context(); let gate = new Admission(c);
  for (let i = 0; i < 10; ++i) await gate.fetch(admit('ip1'));
  gate = new Admission(c);
  for (let i = 0; i < 10; ++i) await gate.fetch(admit('ip1'));
  assert.equal((await gate.fetch(admit('ip1'))).status, 429);
});
await test('daily room and handshake limits, next day reset', async () => {
  const c = new Context(), gate = new Admission(c);
  await gate.fetch(admit('seed'));
  let state = c.values.get('limits'); state.total = 2000;
  assert.equal((await gate.fetch(admit('next'))).status, 429);
  state.total = 1; state.rooms = Array.from({length:64}, (_,i) => 'room'+i);
  assert.equal((await gate.fetch(admit('new', 'notexisting'))).status, 429);
  state.day = '2000-01-01';
  assert.equal((await gate.fetch(admit('new', 'notexisting'))).status, 200);
});
await test('outer gate strips client-provided internal identity and old test secret', async () => {
  let forwarded;
  const env = {
    ADMISSION: { idFromName: n => n, get: n => ({ fetch: async request => Response.json({ok:true}) }) },
    ROOMS: { idFromName: n => n, get: n => ({ fetch: async request => {forwarded=request; return Response.json({ok:true});} }) }
  };
  const input = request('aaaaaaaaaaaaaaaa', 'forged');
  input.headers.set('X-Prototype-Key', 'old-secret');
  assert.equal((await worker.fetch(input,env)).status, 200);
  assert.notEqual(forwarded.headers.get('X-Relay-Ip-Hash'), 'forged');
  assert.equal(forwarded.headers.get('X-Prototype-Key'), null);
  assert.equal((await worker.fetch(new Request('https://signal.example/health'),env)).status, 200);
});
globalThis.Response = NativeResponse;
console.log(`RESULT ${passed}/${passed}; mock unit tests, not a load/security certification`);
