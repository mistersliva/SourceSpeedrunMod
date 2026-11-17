/*
 * race.js — ghost race (HUD page). Keeps the WebSocket to the lobby server
 * (launcher/tools/race-worker.js), sends this player's position ~10 times a
 * second, hands other players' positions to client.dll (ssm_ghost) and draws
 * the race board and the start countdown.
 *
 * The menu page controls it through localStorage:
 *   'race.ctl'    {seq, action: 'create'|'join'|'leave'|'start', code}
 *   'race.status' {state, code, me, host, players:[{id, name, map, tm, fin}], error}
 */
(function (global) {
  'use strict';

  const SEND_INTERVAL = 100;    // ms
  const STATUS_KEY = 'race.status';
  const CTL_KEY = 'race.ctl';
  const $ = (id) => document.getElementById(id);

  let ws = null;
  let code = '';
  let me = '';
  let host = '';
  let state = 'idle';           // idle | connecting | lobby | error
  let error = '';
  let startAt = 0;
  let lastSend = 0;
  let lastCtl = 0;
  let mine = { map: '', tm: null, fin: false };
  const players = new Map();    // id -> {name, map, tm, fin}
  const chat = [];              // {name, text, at, self}

  const cleanName = (n) => String(n || '').replace(/[";\r\n\u0000-\u001f]/g, '').trim().slice(0, 24) || 'Player';
  const server = () => String(Prefs.get('mp.server') || '').trim().replace(/\/+$/, '');

  function publish() {
    const list = [...players.entries()].map(([id, p]) => ({ id, ...p }));
    if (me) list.unshift({ id: me, name: cleanName(Prefs.get('mp.name')), map: mine.map, tm: mine.tm, fin: mine.fin, self: true });
    try { localStorage.setItem(STATUS_KEY, JSON.stringify({ state, code, me, host, players: list, error, startAt, chat: chat.slice(-30), t: Date.now() })); } catch {}
    renderBoard(list);
  }

  function setState(s, err = '') { state = s; error = err; publish(); }

  function close(reason) {
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
    ws = null;
    players.clear();
    chat.length = 0;
    renderChat();
    me = ''; host = ''; code = ''; startAt = 0;
    Source.cmd('ssm_ghost_clear');
    setState(reason ? 'error' : 'idle', reason || '');
  }

  function connect(c) {
    close();
    const base = server();
    if (!base) { setState('error', 'No race server set (Settings → Multiplayer).'); return; }
    code = c.toUpperCase();
    setState('connecting');
    let sock;
    try { sock = new WebSocket(`${base.replace(/^http/, 'ws')}/lobby/${code}`); } catch (e) { close(e.message); return; }
    ws = sock;
    sock.onopen = () => sock.send(JSON.stringify({ t: 'hello', name: cleanName(Prefs.get('mp.name')) }));
    sock.onmessage = (e) => { try { onMessage(JSON.parse(e.data)); } catch {} };
    sock.onclose = () => { if (ws === sock) close(state === 'lobby' ? 'Disconnected from the lobby.' : 'Could not connect to the race server.'); };
    sock.onerror = () => {};
  }

  function onMessage(m) {
    switch (m.t) {
      case 'welcome':
        me = m.id; host = m.host;
        for (const p of m.players || []) if (p.id !== me) players.set(p.id, { name: cleanName(p.name), map: '', tm: null, fin: false });
        setState('lobby');
        break;
      case 'join': players.set(m.id, { name: cleanName(m.name), map: '', tm: null, fin: false }); publish(); break;
      case 'leave': players.delete(m.id); Source.cmd(`ssm_ghost_remove ${m.id}`); publish(); break;
      case 'host': host = m.id || ''; publish(); break;
      case 'error': close(m.msg || 'Lobby error'); break;
      case 'start': startAt = m.at; publish(); break;
      case 'chat':
        chat.push({ name: cleanName(m.name), text: String(m.text || '').slice(0, 200), at: Date.now(), self: m.id === me });
        if (chat.length > 100) chat.splice(0, chat.length - 100);
        Source.sound?.('click');
        renderChat();
        publish();
        break;
      case 'pos': {
        const p = players.get(m.id);
        if (!p) return;
        const changed = p.map !== m.m || p.fin !== !!m.fin || Math.floor((p.tm ?? -1000) / 1000) !== Math.floor((m.tm ?? -1000) / 1000);
        p.map = m.m || ''; p.tm = m.tm ?? null; p.fin = !!m.fin;
        if (Array.isArray(m.p) && p.map) {
          Source.cmd(`ssm_ghost ${m.id} ${p.map} ${m.p[0]} ${m.p[1]} ${m.p[2]} ${m.y || 0} ${m.s || 0} ${m.d ? 1 : 0} ${p.name}`);
        }
        if (changed) publish();
        break;
      }
      default: break;
    }
  }

  // Called with every HUD state (hud.js).
  function onState(s) {
    mine = { map: s.map || '', tm: global.RunTimer ? RunTimer.ms() : null, fin: !!(global.RunTimer && RunTimer.ended) };
    tickCountdown();
    if (!ws || state !== 'lobby' || ws.readyState !== 1) return;
    const now = performance.now();
    if (now - lastSend < SEND_INTERVAL) return;
    lastSend = now;
    const msg = { t: 'pos', m: mine.map, s: s.speed || 0, y: Math.round(s.yaw || 0) };
    if (Array.isArray(s.pos)) msg.p = s.pos.map((v) => Math.round(v));
    if (s.duck) msg.d = 1;
    if (mine.tm != null) msg.tm = mine.tm;
    if (mine.fin) msg.fin = 1;
    ws.send(JSON.stringify(msg));
  }

  // Race start: 3-2-1 on screen, then everybody starts a new game at once.
  function tickCountdown() {
    const el = $('race-countdown');
    if (!startAt) { if (!el.hidden) el.hidden = true; return; }
    const left = startAt - Date.now();
    if (left <= 0) {
      startAt = 0;
      el.hidden = true;
      Source.cmd('ssm_ghost_clear');
      Source.call('newgame', { config: 'chapter1.cfg' }).catch(() => Source.cmd('map d1_trainstation_01'));
      publish();
      return;
    }
    el.hidden = false;
    const n = String(Math.ceil(left / 1000));
    if (el.textContent !== n) el.textContent = n;
  }
  setInterval(tickCountdown, 100);

  function fmt(ms) {
    if (ms == null) return '—';
    const t = Math.max(0, Math.floor(ms / 1000));
    const h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60, s = t % 60;
    return h ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${m}:${String(s).padStart(2, '0')}`;
  }

  function renderBoard(list) {
    const board = $('race-board');
    const show = state === 'lobby' && list.length > 1 && Prefs.get('mp.board') !== false;
    board.hidden = !show;
    if (!show) return;
    // Finished first (by time), then further along (higher time = later), then the rest.
    const rows = [...list].sort((a, b) => (b.fin - a.fin) || (a.fin ? (a.tm ?? 1e12) - (b.tm ?? 1e12) : 0));
    $('race-code').textContent = code;
    $('race-rows').replaceChildren(...rows.map((p) => {
      const row = document.createElement('div');
      row.className = `race__row${p.self ? ' is-self' : ''}${p.fin ? ' is-fin' : ''}`;
      const name = document.createElement('span'); name.className = 'race__name'; name.textContent = p.name;
      const map = document.createElement('span'); map.className = 'race__map'; map.textContent = p.fin ? 'Finished' : (p.map || '');
      const tm = document.createElement('span'); tm.className = 'race__time'; tm.textContent = fmt(p.tm);
      row.append(name, map, tm);
      return row;
    }));
  }
  // Chat feed in the HUD: the last few messages, each fading out after 12 s.
  const CHAT_SHOW = 12000;
  function renderChat() {
    const feed = $('race-chat');
    const now = Date.now();
    const recent = chat.filter((c) => now - c.at < CHAT_SHOW).slice(-6);
    feed.hidden = !recent.length;
    feed.replaceChildren(...recent.map((c) => {
      const row = document.createElement('div');
      row.className = `chat__line${c.self ? ' is-self' : ''}`;
      if (now - c.at > CHAT_SHOW - 1500) row.classList.add('is-fading');
      const n = document.createElement('b'); n.textContent = c.name;
      const t = document.createElement('span'); t.textContent = c.text;
      row.append(n, t);
      return row;
    }));
  }
  setInterval(() => { if (chat.length) renderChat(); }, 500);

  // Own time on the board ticks once a second.
  setInterval(() => { if (state === 'lobby') publish(); }, 1000);

  // Commands from the menu page.
  function onCtl(raw) {
    let c;
    try { c = JSON.parse(raw || 'null'); } catch { return; }
    if (!c || c.seq <= lastCtl) return;
    lastCtl = c.seq;
    if (c.action === 'create' || c.action === 'join') connect(c.code);
    else if (c.action === 'leave') close();
    else if (c.action === 'start' && ws && state === 'lobby' && host === me) ws.send(JSON.stringify({ t: 'start' }));
    else if (c.action === 'chat' && ws && state === 'lobby') {
      const text = String(c.text || '').trim().slice(0, 200);
      if (text) ws.send(JSON.stringify({ t: 'chat', text }));
    } else if (c.action === 'status') publish();
  }
  addEventListener('storage', (e) => { if (e.key === CTL_KEY) onCtl(e.newValue); });
  try { lastCtl = (JSON.parse(localStorage.getItem(CTL_KEY) || 'null') || {}).seq || 0; } catch {}
  publish();

  global.Race = { onState };
})(window);
