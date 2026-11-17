// Ghost race lobbies for SourceSpeedrunMod, as a Cloudflare Worker with one
// Durable Object per lobby (works on the free plan).
//
// Deploy (same Cloudflare account as the ownership worker is fine):
//   1. npm create cloudflare@latest ssm-race   (choose "Hello World" Worker)
//   2. replace src/index.js with this file and wrangler.jsonc with
//      tools/race-wrangler.jsonc
//   3. npx wrangler deploy                      → https://ssm-race.<you>.workers.dev
//   4. the game uses wss://ssm-race.<you>.workers.dev (Ghost race → Race server).
//      Live: wss://ssm-race.sourcespeedrunmod.workers.dev
//
// GET /lobby/<CODE> with "Upgrade: websocket" joins lobby CODE (4–8 letters or
// digits). Messages are JSON text:
//   client → server  {t:'hello', name}            first message
//                    {t:'pos', m, p:[x,y,z], y, s, d, tm, fin}   ~10 per second
//                    {t:'start'}                  host only: start a race
//                    {t:'chat', text}             at most 1/s, 200 characters
//   server → client  {t:'welcome', id, host, players:[{id, name}]}
//                    {t:'join', id, name} / {t:'leave', id} / {t:'host', id}
//                    {t:'pos', id, ...}           another player's state
//                    {t:'start', at}              race starts at epoch ms "at"
//                    {t:'chat', id, name, text}   to everyone, sender included
//                    {t:'error', msg}
// Nothing is stored: a lobby lives as long as somebody is connected.

const MAX_PLAYERS = 8;
const MAX_MESSAGE = 512;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const m = /^\/lobby\/([A-Za-z0-9]{4,8})$/.exec(url.pathname);
    if (!m) return new Response('SourceSpeedrunMod race server', { status: url.pathname === '/' ? 200 : 404 });
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('expected websocket', { status: 426 });
    const code = m[1].toUpperCase();
    return env.LOBBY.get(env.LOBBY.idFromName(code)).fetch(request);
  },
};

export class Lobby {
  constructor(state) {
    this.state = state;
  }

  sockets() { return this.state.getWebSockets(); }
  info(ws) { return ws.deserializeAttachment() || {}; }

  send(ws, msg) { try { ws.send(typeof msg === 'string' ? msg : JSON.stringify(msg)); } catch {} }
  broadcast(msg, except) {
    const text = JSON.stringify(msg);
    for (const ws of this.sockets()) if (ws !== except && this.info(ws).id) this.send(ws, text);
  }

  hostId() {
    // The longest-connected player is the host.
    let host = null;
    for (const ws of this.sockets()) {
      const i = this.info(ws);
      if (i.id && (!host || i.since < host.since)) host = i;
    }
    return host?.id || null;
  }

  async fetch(request) {
    const players = this.sockets().filter((ws) => this.info(ws).id);
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    if (players.length >= MAX_PLAYERS) {
      this.send(server, { t: 'error', msg: 'Lobby is full' });
      server.close(1008, 'full');
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, data) {
    if (typeof data !== 'string' || data.length > MAX_MESSAGE) return;
    let msg;
    try { msg = JSON.parse(data); } catch { return; }
    const me = this.info(ws);

    if (msg.t === 'hello' && !me.id) {
      const id = Math.random().toString(36).slice(2, 8);
      const name = String(msg.name || 'Player').replace(/[\u0000-\u001f]/g, '').slice(0, 24) || 'Player';
      ws.serializeAttachment({ id, name, since: Date.now() });
      const list = this.sockets().map((s) => this.info(s)).filter((i) => i.id).map(({ id: pid, name: pname }) => ({ id: pid, name: pname }));
      this.send(ws, { t: 'welcome', id, host: this.hostId(), players: list });
      this.broadcast({ t: 'join', id, name }, ws);
      return;
    }
    if (!me.id) return;

    if (msg.t === 'pos') {
      // Pass through only the known fields.
      const out = { t: 'pos', id: me.id, m: String(msg.m || '').slice(0, 64) };
      if (Array.isArray(msg.p) && msg.p.length === 3) out.p = msg.p.map((v) => Math.round(Number(v) || 0));
      for (const k of ['y', 's', 'tm']) if (Number.isFinite(msg[k])) out[k] = Math.round(msg[k]);
      if (msg.d) out.d = 1;
      if (msg.fin) out.fin = 1;
      this.broadcast(out, ws);
    } else if (msg.t === 'chat') {
      // At most one message a second per player; plain text only.
      const now = Date.now();
      if (now - (me.lastChat || 0) < 1000) return;
      const text = String(msg.text || '').replace(/[\u0000-\u001f]/g, ' ').trim().slice(0, 200);
      if (!text) return;
      ws.serializeAttachment({ ...me, lastChat: now });
      this.broadcast({ t: 'chat', id: me.id, name: me.name, text });
    } else if (msg.t === 'start' && this.hostId() === me.id) {
      this.broadcast({ t: 'start', at: Date.now() + 5000 });
    }
  }

  async webSocketClose(ws) { this.left(ws); }
  async webSocketError(ws) { this.left(ws); }

  left(ws) {
    const me = this.info(ws);
    if (!me.id) return;
    const wasHost = this.hostId() === me.id;
    ws.serializeAttachment({});
    try { ws.close(1000, 'bye'); } catch {}
    this.broadcast({ t: 'leave', id: me.id });
    if (wasHost) this.broadcast({ t: 'host', id: this.hostId() });
  }
}
