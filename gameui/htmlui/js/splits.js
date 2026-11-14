/*
 * splits.js — LiveSplit-style speedrun timer for the HUD.
 *
 * Timing: "game time" = in-game time with loads and pauses removed (the sum of
 * map time across maps, like SourceSplit); "real time" = wall clock.
 * Splits: LiveSplit .lss files from <mod>/splits (settings → Timer). Best
 * segments, attempts and a new personal best are written back to the file,
 * which stays LiveSplit compatible (the original is kept as .lss.bak once).
 *
 * Controls (console commands, bindable in Keyboard → SourceSpeedrunMod):
 *   timer_split  start / split      timer_reset  reset
 *   timer_undo   undo split         timer_skip   skip split
 * Optional: auto start on new game, auto split when entering a new map.
 */
(function (global) {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const pad = (n, w = 2) => String(n).padStart(w, '0');

  // ------------------------------------------------------------------ Formats
  function fmtTimer(ms) {
    const neg = ms < 0;
    ms = Math.abs(Math.floor(ms));
    const cs = Math.floor(ms / 10) % 100, s = Math.floor(ms / 1000) % 60, m = Math.floor(ms / 60000) % 60, h = Math.floor(ms / 3600000);
    return `${neg ? '-' : ''}${h ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`}.${pad(cs)}`;
  }
  function fmtSplit(ms) {
    if (ms == null) return '-';
    ms = Math.max(0, Math.floor(ms));
    const s = Math.floor(ms / 1000) % 60, m = Math.floor(ms / 60000) % 60, h = Math.floor(ms / 3600000);
    if (!h && !m) return `${s}.${pad(Math.floor(ms / 10) % 100)}`;
    return h ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
  }
  function fmtDelta(ms) {
    if (ms == null) return '';
    const sign = ms < 0 ? '−' : '+';
    const a = Math.abs(ms);
    if (a < 60000) return `${sign}${(Math.floor(a / 100) / 10).toFixed(1)}`;
    const s = Math.floor(a / 1000) % 60, m = Math.floor(a / 60000) % 60, h = Math.floor(a / 3600000);
    return `${sign}${h ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`}`;
  }

  // LiveSplit time span: [-][d.]hh:mm:ss[.fffffff]
  function parseSpan(t) {
    if (!t) return null;
    const m = /^(-)?(?:(\d+)\.)?(\d+):(\d+):(\d+(?:\.\d+)?)$/.exec(t.trim());
    if (!m) return null;
    const ms = (((Number(m[2] || 0) * 24 + Number(m[3])) * 60 + Number(m[4])) * 60 + parseFloat(m[5])) * 1000;
    return m[1] ? -ms : ms;
  }
  function toSpan(ms) {
    const neg = ms < 0;
    ms = Math.round(Math.abs(ms));
    const total = Math.floor(ms / 1000);
    return `${neg ? '-' : ''}${pad(Math.floor(total / 3600))}:${pad(Math.floor(total / 60) % 60)}:${pad(total % 60)}.${pad((ms % 1000) * 10000, 7)}`;
  }
  function lssDate(d) {
    return `${pad(d.getUTCMonth() + 1)}/${pad(d.getUTCDate())}/${d.getUTCFullYear()} ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`;
  }

  // ------------------------------------------------------------------ .lss DOM
  let doc = null;      // parsed XML document
  let file = '';       // file name in <mod>/splits
  let hadDecl = true;  // keep the <?xml?> declaration on save
  const run = { game: '', category: '', attempts: 0, offset: 0, segments: [] };

  const kid = (el, tag) => (el ? Array.from(el.children).find((c) => c.tagName === tag) : null);
  function ensure(el, tag) {
    let c = kid(el, tag);
    if (!c) { c = doc.createElement(tag); el.append(c); }
    return c;
  }
  const readTimes = (el) => ({ real: parseSpan(kid(el, 'RealTime')?.textContent), game: parseSpan(kid(el, 'GameTime')?.textContent) });
  function writeTimes(el, t) {
    for (const [tag, v] of [['RealTime', t.real], ['GameTime', t.game]]) {
      if (v == null) kid(el, tag)?.remove();
      else ensure(el, tag).textContent = toSpan(v);
    }
  }
  const pbNode = (seg) => Array.from(kid(seg, 'SplitTimes')?.children || []).find((c) => c.getAttribute('name') === 'Personal Best');

  function parse(text) {
    const d = new DOMParser().parseFromString(text, 'application/xml');
    const root = d.documentElement;
    if (!root || root.tagName !== 'Run' || d.querySelector('parsererror')) throw new Error('not a LiveSplit .lss file');
    doc = d;
    hadDecl = /^\s*<\?xml/.test(text);
    run.game = kid(root, 'GameName')?.textContent || '';
    run.category = kid(root, 'CategoryName')?.textContent || '';
    run.attempts = Number(kid(root, 'AttemptCount')?.textContent) || 0;
    run.offset = parseSpan(kid(root, 'Offset')?.textContent) || 0;
    run.segments = Array.from(kid(root, 'Segments')?.children || []).filter((s) => s.tagName === 'Segment').map((el) => ({
      el,
      name: kid(el, 'Name')?.textContent || '',
      pb: pbNode(el) ? readTimes(pbNode(el)) : { real: null, game: null },
      best: kid(el, 'BestSegmentTime') ? readTimes(kid(el, 'BestSegmentTime')) : { real: null, game: null },
    }));
  }

  function clearRun() {
    doc = null;
    file = '';
    Object.assign(run, { game: '', category: '', attempts: 0, offset: 0, segments: [] });
  }

  async function save() {
    if (!doc || !file || !Source.IN_GAME) return;
    let text = new XMLSerializer().serializeToString(doc);
    if (hadDecl && !/^\s*<\?xml/.test(text)) text = `<?xml version="1.0" encoding="UTF-8"?>\n${text}`;
    try { await Source.call('splits.write', { file, text }); } catch (e) { console.warn('[splits] save failed', e); }
  }

  // ------------------------------------------------------------------ Timer
  const t = {
    phase: 'idle',      // idle | running | ended
    game: 0,            // accumulated game time (ms)
    realStart: 0,       // performance.now() at start
    started: null,      // Date at start
    splits: [],         // per segment { real, game } or null (skipped)
    lastT: null, lastMap: '', maps: new Set(),
    armed: false,       // auto start on the next map after a new game
    pb: false,
  };
  const method = () => (Prefs.get('timer.method') === 'real' ? 'real' : 'game');
  const realNow = () => (t.phase === 'idle' ? 0 : (t.phase === 'ended' ? t.realEnd : performance.now()) - t.realStart);
  const current = () => ({ real: realNow() + run.offset, game: t.game + run.offset });
  const value = (x) => (x ? x[method()] ?? (method() === 'game' ? x.real : null) : null);
  // Comparison times fall back to real time when the file has no game time.
  const cmp = (x) => (x ? (x[method()] ?? x.real) : null);

  function start() {
    Object.assign(t, { phase: 'running', game: 0, realStart: performance.now(), started: new Date(), splits: [], pb: false, armed: false });
    t.maps = new Set(t.lastMap ? [t.lastMap] : []);
    run.attempts++;
    render(true);
  }

  function split() {
    if (t.phase === 'idle') { start(); return; }
    if (t.phase !== 'running') return;
    t.splits.push(current());
    if (!run.segments.length || t.splits.length >= run.segments.length) finish();
    render(true);
  }

  function skip() {
    if (t.phase !== 'running' || t.splits.length >= run.segments.length - 1) return;
    t.splits.push(null);
    render(true);
  }

  function undo() {
    if (t.phase === 'idle' || !t.splits.length) return;
    if (t.phase === 'ended') t.phase = 'running';
    t.splits.pop();
    t.pb = false;
    render(true);
  }

  function segmentTime(i, splits, m) {
    const cur = splits[i];
    if (!cur || cur[m] == null) return null;
    if (i === 0) return cur[m] - run.offset;
    const prev = splits[i - 1];
    return prev && prev[m] != null ? cur[m] - prev[m] : null;
  }

  function finish() {
    t.phase = 'ended';
    t.realEnd = performance.now();
    if (!run.segments.length) return;
    const final = cmp(t.splits[t.splits.length - 1]);
    const pbFinal = cmp(run.segments[run.segments.length - 1].pb);
    t.pb = final != null && (pbFinal == null || final < pbFinal);
  }

  // A finished run that beat the PB becomes the new PB on reset (until then the
  // deltas keep comparing against the old one, as in LiveSplit).
  function applyPb() {
    if (t.phase !== 'ended' || !t.pb) return;
    {
      run.segments.forEach((seg, i) => {
        seg.pb = t.splits[i] ? { ...t.splits[i] } : { real: null, game: null };
        if (doc) {
          const times = ensure(seg.el, 'SplitTimes');
          let node = pbNode(seg.el);
          if (!node) { node = doc.createElement('SplitTime'); node.setAttribute('name', 'Personal Best'); times.append(node); }
          writeTimes(node, seg.pb);
        }
      });
    }
  }

  // Best segments, attempt + segment history, attempt count; then save.
  function commitAttempt() {
    if (!doc || t.phase === 'idle') return;
    const root = doc.documentElement;
    const hist = ensure(root, 'AttemptHistory');
    const id = Math.max(0, ...Array.from(hist.children).map((a) => Number(a.getAttribute('id')) || 0)) + 1;
    const a = doc.createElement('Attempt');
    a.setAttribute('id', id);
    a.setAttribute('started', lssDate(t.started));
    a.setAttribute('isStartedSynced', 'False');
    a.setAttribute('ended', lssDate(new Date()));
    a.setAttribute('isEndedSynced', 'False');
    if (t.phase === 'ended' && t.splits.length) writeTimes(a, t.splits[t.splits.length - 1]);
    hist.append(a);

    run.segments.forEach((seg, i) => {
      const real = segmentTime(i, t.splits, 'real'), game = segmentTime(i, t.splits, 'game');
      if (real == null && game == null) return;
      const time = doc.createElement('Time');
      time.setAttribute('id', id);
      writeTimes(time, { real, game });
      ensure(seg.el, 'SegmentHistory').append(time);
      let changed = false;
      if (real != null && (seg.best.real == null || real < seg.best.real)) { seg.best.real = real; changed = true; }
      if (game != null && (seg.best.game == null || game < seg.best.game)) { seg.best.game = game; changed = true; }
      if (changed) writeTimes(ensure(seg.el, 'BestSegmentTime'), seg.best);
    });
    ensure(root, 'AttemptCount').textContent = String(run.attempts);
    save();
  }

  function reset() {
    if (t.phase === 'idle') return;
    applyPb();
    commitAttempt();
    t.phase = 'idle';
    t.splits = [];
    t.game = 0;
    t.pb = false;
    render(true);
  }

  // Game time comes from the HUD state: map time (curtime) only advances while
  // the game simulates, so summing its increments removes loads and pauses.
  function onState(s) {
    if (s.time == null) return;
    const map = s.map || '';
    const sameMap = map === t.lastMap;
    if (t.armed && map && (map !== t.armedFrom || (t.armedT != null && s.time + 500 < t.armedT))) {
      // New game: the run starts when the first map is up.
      start();
      t.maps = new Set([map]);
      t.lastT = s.time;
      t.lastMap = map;
      render(false);
      return;
    }
    if (t.phase === 'running' && sameMap && t.lastT != null) {
      const d = s.time - t.lastT;
      if (d > 0 && d < 250) t.game += d;
    }
    if (t.phase === 'running' && map && !sameMap && t.lastMap) {
      if (Prefs.get('timer.autoSplit') && !t.maps.has(map)) split();
      t.maps.add(map);
    }
    t.lastT = s.time;
    t.lastMap = map;
    render(false);
  }

  // ------------------------------------------------------------------ Render
  let rowsKey = '';
  const visible = () => Prefs.get('timer.panel') && !!file;

  function deltaClass(i, delta) {
    const seg = segmentTime(i, t.splits, method()) ?? (method() === 'game' ? segmentTime(i, t.splits, 'real') : null);
    const best = cmp(run.segments[i].best);
    if (seg != null && best != null && seg < best) return 'is-gold';
    const pbSeg = i === 0 ? cmp(run.segments[0].pb) - run.offset : (cmp(run.segments[i].pb) != null && cmp(run.segments[i - 1].pb) != null ? cmp(run.segments[i].pb) - cmp(run.segments[i - 1].pb) : null);
    const gained = seg != null && pbSeg != null && seg < pbSeg;
    return delta < 0 ? (gained ? 'is-ahead-gain' : 'is-ahead-lose') : (gained ? 'is-behind-gain' : 'is-behind-lose');
  }

  function windowRows() {
    const n = run.segments.length, max = Math.max(3, Number(Prefs.get('timer.rows')) || 10);
    if (n <= max) return [...Array(n).keys()];
    const cur = Math.min(t.splits.length, n - 1);
    let first = Math.max(0, Math.min(cur - Math.floor((max - 1) / 2), n - max));
    const rows = [...Array(max - 1).keys()].map((k) => first + k).filter((i) => i < n - 1);
    rows.push(n - 1); // the final split is always shown
    return rows;
  }

  function renderRows() {
    const box = $('splits-rows');
    const rows = windowRows();
    const cur = t.phase === 'running' ? t.splits.length : -1;
    box.replaceChildren(...rows.map((i) => {
      const seg = run.segments[i];
      const done = i < t.splits.length;
      const sp = done ? t.splits[i] : null;
      const delta = sp && cmp(seg.pb) != null ? value(sp) - cmp(seg.pb) : null;
      const row = document.createElement('div');
      row.className = `split${i === cur ? ' is-current' : ''}${i === run.segments.length - 1 && rows.length < run.segments.length ? ' is-last' : ''}`;
      row.innerHTML = '<span class="split__name"></span><span class="split__delta"></span><span class="split__time"></span>';
      row.children[0].textContent = seg.name;
      if (delta != null) { row.children[1].textContent = fmtDelta(delta); row.children[1].className = `split__delta ${deltaClass(i, delta)}`; }
      row.children[2].textContent = done ? (sp ? fmtSplit(value(sp)) : '-') : fmtSplit(cmp(seg.pb));
      if (i === cur) row.dataset.live = '1';
      return row;
    }));
  }

  function renderInfo() {
    // Previous segment vs PB segment.
    let prev = '';
    const last = t.splits.length - 1;
    if (last >= 0 && run.segments[last]) {
      const seg = segmentTime(last, t.splits, method());
      const pbs = last === 0 ? cmp(run.segments[0].pb) - run.offset : (cmp(run.segments[last].pb) != null && cmp(run.segments[last - 1].pb) != null ? cmp(run.segments[last].pb) - cmp(run.segments[last - 1].pb) : null);
      if (seg != null && pbs != null) prev = fmtDelta(seg - pbs);
    }
    $('splits-prev').textContent = prev || '-';
    const bests = run.segments.map((s) => cmp(s.best));
    $('splits-sob').textContent = bests.length && bests.every((b) => b != null) ? fmtSplit(bests.reduce((a, b) => a + b, 0)) : '-';
    $('splits-attempts').textContent = run.attempts ? String(run.attempts) : '';
  }

  function render(structural) {
    const panel = $('splits');
    if (!panel) return;
    const show = visible();
    if (panel.hidden === show) panel.hidden = !show;
    document.body.classList.toggle('has-splits', show);
    document.body.classList.toggle('splits-left', Prefs.get('timer.side') === 'left');

    const now = value(current()) ?? 0;
    const time = t.phase === 'idle' ? run.offset : t.phase === 'ended' ? value(t.splits[t.splits.length - 1]) ?? now : now;
    const timerEl = $('splits-timer');
    const cur = t.splits.length;
    let state = '';
    if (t.phase === 'ended') state = t.pb ? 'is-pb' : 'is-behind-lose';
    else if (t.phase === 'running' && run.segments[cur]) {
      const pb = cmp(run.segments[cur].pb);
      const lastDelta = cur > 0 && t.splits[cur - 1] && cmp(run.segments[cur - 1].pb) != null ? value(t.splits[cur - 1]) - cmp(run.segments[cur - 1].pb) : null;
      state = pb != null && now > pb ? 'is-behind-lose' : lastDelta != null && lastDelta > 0 ? 'is-behind-gain' : 'is-ahead-gain';
    }
    timerEl.className = `splits__timer ${state}`;
    timerEl.textContent = fmtTimer(time);

    if (!show) return;
    const key = `${file}|${t.phase}|${t.splits.length}|${Prefs.get('timer.rows')}|${method()}|${run.attempts}`;
    if (structural || key !== rowsKey) {
      rowsKey = key;
      $('splits-game').textContent = run.game || file.replace(/\.lss$/i, '');
      $('splits-category').textContent = run.category;
      renderRows();
      renderInfo();
    }
    // Live delta on the current split once behind the PB split.
    const live = document.querySelector('#splits-rows [data-live]');
    if (live && t.phase === 'running') {
      const pb = cmp(run.segments[cur]?.pb);
      const d = live.children[1];
      if (pb != null && now > pb) { d.textContent = fmtDelta(now - pb); d.className = 'split__delta is-behind-lose'; }
      else if (d.textContent) { d.textContent = ''; d.className = 'split__delta'; }
    }
  }

  // ------------------------------------------------------------------ Files
  async function loadFile(name) {
    if (t.phase !== 'idle') reset();
    if (!name) { clearRun(); render(true); return; }
    try {
      const text = await Source.call('splits.read', { file: name });
      parse(text);
      file = name;
    } catch (e) {
      console.warn('[splits] load failed', e);
      clearRun();
    }
    render(true);
  }

  Prefs.onChange((key) => {
    if (key === 'timer.file') loadFile(Prefs.get('timer.file'));
    else if (key.startsWith('timer.')) render(true);
  });

  // Commands from the engine (console / binds) and signals from the menu page.
  Source.on('timer', ({ action }) => ({ split, reset, undo, skip }[action]?.()));
  addEventListener('storage', (e) => {
    if (e.key !== 'htmlui.timer.signal' || !e.newValue) return;
    let sig = null;
    try { sig = JSON.parse(e.newValue); } catch {}
    if (sig?.action === 'newgame' && Prefs.get('timer.autoStart')) {
      reset();
      // Start once the new game's map is up: a different map, or the same map
      // with its clock restarted.
      t.armed = true;
      t.armedFrom = t.lastMap;
      t.armedT = t.lastT;
    }
  });

  if (Source.IN_GAME) loadFile(Prefs.get('timer.file'));

  global.RunTimer = {
    onState,
    split, reset, undo, skip,
    get running() { return t.phase !== 'idle'; },
    get panelVisible() { return visible(); },
    display() { const v = t.phase === 'ended' ? value(t.splits[t.splits.length - 1]) : value(current()); return fmtTimer(t.phase === 'idle' ? run.offset : v ?? 0); },
    // Browser preview.
    _demo(text) { parse(text); file = 'demo.lss'; render(true); },
  };
})(window);
