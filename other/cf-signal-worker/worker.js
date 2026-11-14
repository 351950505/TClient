// Low-volume public WSS relay. Rooms and uids are discovery hints, not identity proof.
const encoder = new TextEncoder();
const MAX_FRAME_BYTES = 512;
const MAX_PAYLOAD_BYTES = 160;
const MAX_CONNECTIONS = 32;
const MAX_CONNECTIONS_PER_IP = 8;
const MAX_IP_ATTEMPTS_PER_MINUTE = 20;
const MAX_TOTAL_ATTEMPTS_PER_DAY = 2000;
const MAX_ROOMS_PER_DAY = 64;
const MAX_MESSAGES_PER_FIVE_SECONDS = 12;

function reply(status, error, extra = {}) {
  return Response.json({ ok: false, error, ...extra }, { status });
}

function identity(request) {
  const url = new URL(request.url);
  const room = url.searchParams.get("room") || "";
  const uid = url.searchParams.get("uid") || "";
  const session = url.searchParams.get("session") || "";
  if (!/^[0-9A-Za-z.\[\]:]{1,64}$/.test(room)) return null;
  if (!/^[0-9a-f]{16}$/.test(uid) || !/^[0-9a-f]{16}$/.test(session)) return null;
  return { room, uid, session };
}

async function hashIp(ip, day) {
  const bytes = await crypto.subtle.digest("SHA-256", encoder.encode(day + ":" + ip));
  return Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, "0")).join("");
}

// A separate, globally addressed admission object bounds room creation and handshakes.
// This does not authenticate users or promise DoS resistance for a public endpoint.
export class Admission {
  constructor(ctx) {
    this.ctx = ctx;
  }

  async fetch(request) {
    const { room, ip } = await request.json();
    const now = Date.now();
    const day = new Date(now).toISOString().slice(0, 10);
    const result = await this.ctx.storage.transaction(async txn => {
      let state = await txn.get("limits");
      if (!state || state.day !== day) state = { day, total: 0, rooms: [], ips: {} };
      if (state.total >= MAX_TOTAL_ATTEMPTS_PER_DAY) return "daily admission limit";
      if (!state.rooms.includes(room) && state.rooms.length >= MAX_ROOMS_PER_DAY) return "daily room limit";
      const cutoff = now - 60000;
      for (const key of Object.keys(state.ips)) {
        state.ips[key] = state.ips[key].filter(at => at > cutoff);
        if (state.ips[key].length === 0) delete state.ips[key];
      }
      const attempts = state.ips[ip] || [];
      if (attempts.length >= MAX_IP_ATTEMPTS_PER_MINUTE) return "connection rate limit";
      if (!(ip in state.ips) && Object.keys(state.ips).length >= 128) return "admission table full";
      attempts.push(now);
      state.ips[ip] = attempts;
      if (!state.rooms.includes(room)) state.rooms.push(room);
      ++state.total;
      await txn.put("limits", state);
      return null;
    });
    return result ? reply(429, result) : Response.json({ ok: true });
  }
}

export class SignalRoom {
  constructor(ctx) {
    this.ctx = ctx;
    this.boot = crypto.randomUUID();
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("PING", "PONG"));
  }

  async fetch(request) {
    const member = identity(request);
    const ip = request.headers.get("X-Relay-Ip-Hash");
    if (!member || !ip || request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return reply(400, "invalid WebSocket request");
    }
    const existing = this.ctx.getWebSockets().filter(ws => ws.readyState === 1);
    // A public uid cannot be used to force-close someone else's active connection.
    if (existing.some(ws => ws.deserializeAttachment()?.uid === member.uid)) {
      return reply(409, "uid already connected");
    }
    if (existing.length >= MAX_CONNECTIONS) return reply(429, "room full");
    if (existing.filter(ws => ws.deserializeAttachment()?.ip === ip).length >= MAX_CONNECTIONS_PER_IP) {
      return reply(429, "too many connections from address");
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ uid: member.uid, session: member.session, ip, lastSequence: -1, window: 0, messages: 0 });
    server.send(`W|${member.uid}|${this.boot}|${existing.length + 1}`);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    const member = ws.deserializeAttachment();
    if (!member || ws.readyState !== 1) return;
    const now = Date.now();
    if (now - member.window >= 5000) { member.window = now; member.messages = 0; }
    if (++member.messages > MAX_MESSAGES_PER_FIVE_SECONDS) {
      ws.close(1008, "signal rate exceeded");
      return;
    }
    ws.serializeAttachment(member);
    if (typeof message !== "string" || encoder.encode(message).length > MAX_FRAME_BYTES) {
      ws.close(1008, "invalid frame");
      return;
    }
    if (message === "Q") {
      ws.send(`I|${this.boot}|${this.ctx.getWebSockets().filter(peer => peer.readyState === 1).length}`);
      return;
    }
    const match = /^S\|([0-9]{1,16})\|([\s\S]+)$/.exec(message);
    if (!match) { ws.send("E|invalid envelope"); return; }
    const sequence = Number(match[1]);
    const payload = match[2];
    if (!Number.isSafeInteger(sequence) || sequence < 1 || !/^\u27e8B:[A-Za-z0-9#%]{6}$/.test(payload) ||
        encoder.encode(payload).length > MAX_PAYLOAD_BYTES) {
      ws.send("E|invalid signal");
      return;
    }
    if (sequence <= member.lastSequence) { ws.send(`A|${sequence}|0|${this.boot}`); return; }
    member.lastSequence = sequence;
    ws.serializeAttachment(member);
    let accepted = 0;
    const outgoing = `D|${member.uid}|${member.session}|${sequence}|${payload}`;
    for (const peer of this.ctx.getWebSockets()) {
      if (peer === ws || peer.readyState !== 1) continue;
      const identity = peer.deserializeAttachment();
      if (!identity || identity.uid === member.uid) continue;
      try { peer.send(outgoing); ++accepted; }
      catch { try { peer.close(1011, "send failed"); } catch {} }
    }
    // Counts server socket writes, not confirmation from every receiving device.
    ws.send(`A|${sequence}|${accepted}|${this.boot}`);
  }

  webSocketClose() {}
  webSocketError(ws) { try { ws.close(1011, "socket error"); } catch {} }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health") return Response.json({ ok: true, service: "tclient-signal-production", protocol: 1 });
    if (url.pathname !== "/ws") return reply(404, "not found");
    const member = identity(request);
    if (!member) return reply(400, "invalid room or identity");
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return reply(426, "WebSocket upgrade required");
    const ip = request.headers.get("CF-Connecting-IP");
    if (!ip) return reply(400, "missing client address");
    const day = new Date().toISOString().slice(0, 10);
    const ipHash = await hashIp(ip, day);
    const allowed = await env.ADMISSION.get(env.ADMISSION.idFromName("global")).fetch("https://admission/", {
      method: "POST", body: JSON.stringify({ room: member.room, ip: ipHash }),
    });
    if (!allowed.ok) return allowed;
    const headers = new Headers(request.headers);
    headers.set("X-Relay-Ip-Hash", ipHash);
    headers.delete("X-Prototype-Key");
    return env.ROOMS.get(env.ROOMS.idFromName(member.room)).fetch(new Request(request, { headers }));
  }
};
