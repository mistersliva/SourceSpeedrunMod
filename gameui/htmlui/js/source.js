/*
 * source.js — engine bridge for the HTML GameUI.
 *
 *   Source.call(method, args)      -> Promise<result>   (POST /api/<method>)
 *   Source.on(event, fn)           -> unsubscribe fn     (engine -> page events)
 *   Source.cmd("map d1_trainstation_01")
 *   Source.cvar.get(["volume"]) / Source.cvar.set("volume", 0.5)
 *   Source.L("#GameUI_NewGame", "New Game")  localized string
 *   Source.sound("ui/buttonclick.wav")
 *
 * Outside the game (any origin other than https://gameui.local) a mock
 * backend is used so the UI can be developed in a regular browser.
 */
(function (global) {
  'use strict';

  const IN_GAME = location.hostname === 'gameui.local';
  const listeners = new Map();
  const strings = Object.create(null);

  function emit(name, payload) {
    const set = listeners.get(name);
    if (set) {
      for (const fn of Array.from(set)) {
        try { fn(payload); } catch (e) { console.error(`[source] handler for '${name}' failed:`, e); }
      }
    }
    const any = listeners.get('*');
    if (any) for (const fn of Array.from(any)) { try { fn(name, payload); } catch (e) { console.error(e); } }
  }

  // Called by the engine (CHtmlUI::Emit).
  global.__sourceEvent = emit;

  async function call(method, args) {
    if (!IN_GAME) return Mock.call(method, args || {});
    const res = await fetch(`/api/${method}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(args || {}),
    });
    let data;
    try { data = await res.json(); } catch { throw new Error(`${method}: invalid response (${res.status})`); }
    if (!data.ok) throw new Error(`${method}: ${data.error}`);
    return data.result;
  }

  function on(name, fn) {
    if (!listeners.has(name)) listeners.set(name, new Set());
    listeners.get(name).add(fn);
    return () => listeners.get(name).delete(fn);
  }

  function L(token, fallback) {
    if (typeof token !== 'string' || token[0] !== '#') return token;
    const s = strings[token] ?? strings[token.toLowerCase()];
    if (s != null) return s;
    return fallback ?? token.replace(/^#[A-Za-z0-9]+_/, '').replace(/_/g, ' ');
  }

  async function loadStrings(prefixes) {
    const table = await call('localize.prefix', { prefixes });
    for (const [k, v] of Object.entries(table)) {
      strings[k] = v;
      strings[k.toLowerCase()] = v;
    }
    return table;
  }

  async function localize(tokens) {
    const missing = tokens.filter((t) => strings[t] == null);
    if (missing.length) Object.assign(strings, await call('localize', { tokens: missing }));
    return tokens.map((t) => L(t));
  }

  const cvar = {
    async get(names) { return call('cvar.get', { names: Array.isArray(names) ? names : [names] }); },
    async value(name) { const r = await cvar.get([name]); return r[name] ? r[name].value : null; },
    async set(name, value) { return call('cvar.set', { name, value: String(value) }); },
  };

  let soundEnabled = true;
  // Short names ("click") use the generated UI sound set (sounds.js, WebAudio);
  // paths ("ui/buttonclick.wav") are played by the engine.
  function sound(name, opts) {
    if (!soundEnabled || !name) return;
    if (!name.includes('/') && !name.includes('.') && global.UISound) {
      global.UISound.play(name, opts);
      return;
    }
    call('sound.play', { name }).catch(() => {});
  }

  // Reports the screen rectangles of visible ".glass" elements so the engine
  // can draw the blurred game behind them (frosted glass). Runs every frame
  // but only calls the engine when something moved.
  function trackGlass(method, selector = '.glass', enabled = () => true) {
    let last = '';
    const tick = () => {
      const rects = [];
      for (const el of enabled() ? document.querySelectorAll(selector) : []) {
        if (!el.checkVisibility({ opacityProperty: true, visibilityProperty: true })) continue;
        const r = el.getBoundingClientRect();
        const x0 = Math.max(0, Math.round(r.left)), y0 = Math.max(0, Math.round(r.top));
        const x1 = Math.min(innerWidth, Math.round(r.right)), y1 = Math.min(innerHeight, Math.round(r.bottom));
        if (x1 <= x0 || y1 <= y0) continue;
        // The glass follows the element's rounded corners and fades with it
        // (opacity of the element and its ancestors, e.g. during page transitions).
        let alpha = 1;
        for (let e = el; e && e !== document.documentElement; e = e.parentElement) alpha *= +getComputedStyle(e).opacity;
        if (alpha < 0.01) continue;
        const radius = parseFloat(getComputedStyle(el).borderTopLeftRadius) || 0;
        rects.push([x0, y0, x1 - x0, y1 - y0, radius, Math.round(alpha * 100) / 100]);
      }
      const key = rects.join(';');
      if (key !== last) {
        last = key;
        if (IN_GAME) call(method, { rects }).catch(() => {});
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  const Source = {
    IN_GAME,
    trackGlass,
    call, on, emit, L, localize, loadStrings, cvar, sound,
    cmd: (command) => call('cmd', { command }),
    setSoundsEnabled(v) { soundEnabled = !!v; },
    strings,
  };

  // ---------------------------------------------------------------------------
  // Mock backend (browser preview)
  // ---------------------------------------------------------------------------
  const Mock = {
    state: { inLevel: false, inBackgroundLevel: false, inGame: false, multiplayer: false, connected: false, paused: false, level: '', maxClients: 1, consoleVisible: false, canSave: false },
    cvars: {
      volume: '0.8', snd_musicvolume: '0.6', snd_surround_speakers: '2', snd_pitchquality: '1', closecaption: '0', cc_subtitles: '0',
      sensitivity: '3', m_pitch: '0.022', m_rawinput: '1', m_filter: '0', zoom_sensitivity_ratio: '1', fov_desired: '75',
      mat_vsync: '0', fps_max: '300', mat_monitorgamma: '2.2', mat_picmip: '0', r_rootlod: '0', mat_forceaniso: '8', mat_antialias: '4',
      mat_hdr_level: '2', mat_motion_blur_enabled: '1', r_shadowrendertotexture: '1', r_waterforceexpensive: '1', mat_reducefillrate: '0',
      skill: '2', hud_quickinfo: '1', crosshair: '1', sk_autoaim_mode: '1', hud_draw_fixed_reticle: '0', con_enable: '1', mat_colorcorrection: '1',
      r_waterforcereflectentities: '1', sv_movement_mode: '0', htmlui_glass: '1', sv_autobunnyhopping: '0', mat_bumpmap: '1', mat_specular: '1', cl_viewbob_enabled: '1',
    },
    async call(method, a) {
      await new Promise((r) => setTimeout(r, 40));
      switch (method) {
        case 'ui.ready': case 'state': return { ...Mock.state };
        case 'game.info': return { title: "HALF-LIFE'", title2: '=', name: 'Half-Life 2', dir: 'hl2', singleplayerOnly: true, noDifficulty: false, x64: true, language: 'english' };
        case 'localize.prefix': return {};
        case 'localize': return Object.fromEntries((a.tokens || []).map((t) => [t, t]));
        case 'cvar.get': return Object.fromEntries((a.names || []).map((n) => [n, n in Mock.cvars ? { value: Mock.cvars[n], default: Mock.cvars[n] } : null]));
        case 'cvar.set': Mock.cvars[a.name] = a.value; return { value: a.value };
        case 'cmd': {
          const m = /^movement_(hl2|oe|hl1)$/.exec(a.command);
          if (m) Mock.cvars.sv_movement_mode = String(['hl2', 'oe', 'hl1'].indexOf(m[1]));
          if (a.command === 'movement_cycle') Mock.cvars.sv_movement_mode = String((Number(Mock.cvars.sv_movement_mode) + 1) % 3);
          return true;
        }
        case 'chapters': return Array.from({ length: 14 }, (_, i) => ({ index: i, id: String(i + 1), config: `chapter${i + 1}.cfg`, title: ['Point Insertion', 'A Red Letter Day', 'Route Kanal', 'Water Hazard', 'Black Mesa East', "We Don't Go To Ravenholm", 'Highway 17', 'Sandtraps', 'Nova Prospekt', 'Entanglement', 'Anticitizen One', 'Follow Freeman!', 'Our Benefactors', 'Dark Energy'][i], image: '', locked: i > 6 }));
        case 'saves.list': return [
          { file: 'quick.sav', name: 'quick', map: 'd1_canals_07', comment: 'Route Kanal', elapsed: '72:15', type: 'quick', time: Date.now() / 1000 - 300, thumb: '' },
          { file: 'autosave.sav', name: 'autosave', map: 'd1_canals_06', comment: 'Route Kanal', elapsed: '70:02', type: 'auto', time: Date.now() / 1000 - 1200, thumb: '' },
          { file: 'half-life-000.sav', name: 'half-life-000', map: 'd1_trainstation_05', comment: 'Point Insertion', elapsed: '21:40', type: 'manual', time: Date.now() / 1000 - 86400, thumb: '' },
        ];
        case 'video.get': return { width: 1920, height: 1080, windowed: true, borderless: false, desktopWidth: 2560, desktopHeight: 1440, modes: [[1280, 720], [1600, 900], [1920, 1080], [2560, 1440]].map(([width, height]) => ({ width, height })) };
        case 'binds.list': return [
          { title: 'Movement', items: [{ command: '+forward', label: 'Move forward', keys: ['W'] }, { command: '+back', label: 'Move back', keys: ['S'] }, { command: '+moveleft', label: 'Move left (strafe)', keys: ['A'] }, { command: '+moveright', label: 'Move right (strafe)', keys: ['D'] }, { command: '+jump', label: 'Jump', keys: ['SPACE'] }, { command: '+duck', label: 'Duck', keys: ['CTRL'] }, { command: '+speed', label: 'Sprint', keys: ['SHIFT'] }] },
          { title: 'Combat', items: [{ command: '+attack', label: 'Primary attack', keys: ['MOUSE1'] }, { command: '+attack2', label: 'Secondary attack', keys: ['MOUSE2'] }, { command: '+reload', label: 'Reload weapon', keys: ['R'] }] },
        ];
        case 'binds.keyname': return a.key || (a.mouse != null ? `MOUSE${a.mouse + 1}` : a.wheel ? (a.wheel > 0 ? 'MWHEELUP' : 'MWHEELDOWN') : String.fromCharCode(a.vk || 63));
        case 'binds.set': return a.key || 'K';
        case 'console.complete': return ['sv_cheats', 'sv_gravity', 'sensitivity', 'snd_restart'].filter((n) => n.startsWith(a.text)).map((name) => ({ name, command: false, value: '0' }));
        case 'console.submit': setTimeout(() => emit('console.print', [{ text: `] ${a.text}\n`, color: [160, 160, 160, 255] }, { text: `Unknown command "${a.text}"\n`, color: [216, 222, 211, 255] }]), 10); return true;
        case 'newgame': case 'saves.load': Mock.simulateLoad(); return true;
        default: return true;
      }
    },
    simulateLoad() {
      emit('loading.start', { dialog: true });
      let p = 0;
      const t = setInterval(() => {
        p += 0.07;
        emit('loading.progress', { progress: Math.min(p, 1), text: p < 0.5 ? 'Loading resources…' : 'Spawning world…' });
        if (p >= 1) {
          clearInterval(t);
          emit('loading.end', { error: false });
          Mock.state = { ...Mock.state, inLevel: true, inGame: true, inBackgroundLevel: false, canSave: true };
          emit('hidden');
          setTimeout(() => emit('activated', { ...Mock.state }), 600);
        }
      }, 120);
    },
  };

  if (!IN_GAME) {
    // Esc in the browser mimics the engine's escape event.
    addEventListener('keydown', (e) => { if (e.key === 'Escape') emit('escape'); });
  }

  global.Source = Source;
})(window);
