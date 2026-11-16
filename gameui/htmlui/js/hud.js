/*
 * hud.js — HTML HUD. State comes from the client DLL bridge
 * (game/client/hl2/hud_htmlbridge.cpp) as "hud.state"; one-shot events are
 * "damage" and "pickup". DOM writes are diffed so only changed regions are
 * repainted (and uploaded to the GPU).
 */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const cache = new Map();

  function setText(el, value) {
    const v = String(value);
    if (cache.get(el) === v) return;
    cache.set(el, v);
    el.textContent = v;
  }

  function setHidden(el, hidden) {
    if (el.hidden !== hidden) el.hidden = hidden;
  }

  function setClass(el, cls, on) {
    if (el.classList.contains(cls) !== on) el.classList.toggle(cls, on);
  }

  function setScale(el, fraction) {
    const v = `scaleX(${Math.max(0, Math.min(1, fraction)).toFixed(3)})`;
    if (el.style.transform !== v) el.style.transform = v;
  }

  function pulse(el, cls) {
    el.classList.remove(cls);
    void el.offsetWidth;
    el.classList.add(cls);
  }

  let prev = null;

  const MODES = [
    { short: 'HL2', name: 'Half-Life 2 — ABH' },
    { short: 'HL2 OE', name: 'Half-Life 2 Old Engine — AFH' },
    { short: 'HL1', name: 'Half-Life 1' },
  ];

  // Customization from the settings page (js/prefs.js, shared localStorage).
  function applyPrefs() {
    const root = document.documentElement.style;
    const b = document.body.classList;
    root.setProperty('--scale', Prefs.get('hud.scale'));
    root.setProperty('--hud-opacity', Prefs.get('hud.opacity'));
    root.setProperty('--accent', Prefs.get('hud.accent'));
    b.toggle('no-speed', !Prefs.get('hud.speed'));
    b.toggle('no-mode', !Prefs.get('hud.mode'));
    b.toggle('no-vitals', !Prefs.get('hud.vitals'));
    b.toggle('no-ammo', !Prefs.get('hud.ammo'));
    b.toggle('no-panels', !Prefs.get('hud.panels'));
    b.toggle('speed-center', Prefs.get('hud.speedPos') === 'center');
    // Engine glass only behind the panel when it is actually drawn.
    $('runbox').classList.toggle('glass', Prefs.get('hud.panels') && Prefs.get('hud.speedPos') !== 'center');
    $('splits').classList.toggle('glass', !!Prefs.get('hud.panels'));
    b.toggle('no-crosshair', !Prefs.get('hud.crosshair'));
    $('crosshair').innerHTML = Crosshair.svg(Crosshair.fromPrefs(Prefs));
  }
  applyPrefs();
  Prefs.onChange(applyPrefs);

  function formatTime(ms) {
    const t = Math.max(0, Math.floor(ms));
    const m = Math.floor(t / 60000), s = Math.floor(t / 1000) % 60, f = t % 1000;
    return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.${String(f).padStart(3, '0')}`;
  }

  function onMovement({ mode }) {
    const t = $('mode-toast');
    t.innerHTML = `<b>Movement</b><span>${escapeHtml(MODES[mode]?.name || '')}</span>`;
    t.hidden = false;
    t.style.animation = 'none';
    void t.offsetWidth;
    t.style.animation = '';
    clearTimeout(onMovement.timer);
    onMovement.timer = setTimeout(() => { t.hidden = true; }, 2300);
  }

  // ---------------------------------------------------------------- State
  function render(s) {
    const body = document.body;
    setClass(body, 'is-hidden', false);
    setClass(body, 'hide-all', !!s.hidden?.all);
    setClass(body, 'hide-health', !!s.hidden?.health);
    setClass(body, 'hide-weapons', !!s.hidden?.weapons);
    setClass(body, 'is-dead', !s.alive);

    // Health / suit
    const hc = $('health-card');
    setText($('health'), s.health);
    setScale($('health-fill'), s.health / 100);
    setClass(hc, 'is-low', s.alive && s.health <= 20);
    setHidden($('suit-card'), !s.suit);
    setText($('armor'), s.armor);
    setScale($('armor-fill'), s.armor / 100);
    if (prev) {
      if (s.health < prev.health) pulse(hc, 'is-hit');
      else if (s.health > prev.health) pulse(hc, 'is-gain');
      if (s.armor > prev.armor) pulse($('suit-card'), 'is-gain');
    }

    // Aux power: visible while draining / recharging or a device is on.
    const aux = s.aux || {};
    const anyDevice = aux.sprint || aux.flashlight || aux.breather;
    setHidden($('aux'), !s.suit || (!anyDevice && aux.power >= 99.9));
    setScale($('aux-fill'), (aux.power || 0) / 100);
    setClass($('aux'), 'is-low', aux.power < 25);
    setClass($('aux-sprint'), 'is-on', !!aux.sprint);
    setClass($('aux-flashlight'), 'is-on', !!aux.flashlight);
    setClass($('aux-breather'), 'is-on', !!aux.breather);

    // Squad
    const sq = s.squad || {};
    setHidden($('squad'), !sq.members);
    if (sq.members) {
      const key = `${sq.members}/${sq.medics}`;
      if (cache.get('squad') !== key) {
        cache.set('squad', key);
        const icons = [];
        for (let i = 0; i < sq.members; i++) icons.push(`<span class="${i < sq.medics ? 'medic' : ''}">${i < sq.medics ? 'M' : 'C'}</span>`);
        $('squad-members').innerHTML = icons.join('');
        $('squad-members').className = 'squad__members icons';
      }
      setText($('squad-state'), sq.following ? 'FOLLOWING' : 'HOLDING POSITION');
    }

    // Timer (run timer / map time) + speed + movement mode
    RunTimer.onState(s);
    const mode = Prefs.get('hud.timerMode');
    // The splits panel has its own big timer; don't show the run time twice.
    const showTimer = mode === 'map' ? s.time != null : mode === 'run' && !RunTimer.panelVisible;
    setClass(document.body, 'no-timer', !showTimer);
    if (showTimer) setText($('timer'), mode === 'map' ? formatTime(s.time) : RunTimer.display());
    const speed = s.speed || 0;
    setText($('speed'), speed);
    setClass($('speedo'), 'is-fast', speed > 320);
    setText($('movement-chip'), MODES[s.movement]?.short || 'HL2');
    setHidden($('autobhop-chip'), !s.autobhop);

    renderWeapon(s.weapon);
    renderSelection(s.selection);
    prev = s;
  }

  function renderWeapon(w) {
    const box = $('ammo');
    setHidden(box, !w);
    if (!w) return;
    setText($('weapon-icon'), w.icon?.char || '');
    setText($('weapon-name'), w.label || '');

    const usesAmmo = w.usesAmmo;
    setHidden($('clip'), !usesAmmo);
    setHidden($('reserve'), !usesAmmo);
    if (usesAmmo) {
      if (w.clip >= 0) {
        setText($('clip'), w.clip);
        setText($('reserve'), w.reserve);
        setClass($('reserve'), 'no-slash', false);
      } else {
        // No clips (e.g. RPG, grenades): reserve is the count.
        setText($('clip'), w.reserve);
        setText($('reserve'), '');
        setHidden($('reserve'), true);
      }
      const current = w.clip >= 0 ? w.clip : w.reserve;
      setClass(box, 'is-empty', current === 0);
      setClass(box, 'is-low', w.clip >= 0 && w.maxClip > 0 && w.clip > 0 && w.clip <= Math.ceil(w.maxClip / 4));
    } else {
      setClass(box, 'is-empty', false);
      setClass(box, 'is-low', false);
    }

    setHidden($('secondary-wrap'), !(w.secondary >= 0));
    if (w.secondary >= 0) setText($('secondary'), w.secondary);

    // Clip bar: one segment per round for small magazines.
    const bar = $('clipbar');
    const max = w.maxClip > 0 && w.maxClip <= 45 ? w.maxClip : 0;
    if (cache.get('clipbar-max') !== max) {
      cache.set('clipbar-max', max);
      bar.innerHTML = '<i></i>'.repeat(max);
    }
    if (max) {
      const segs = bar.children;
      for (let i = 0; i < segs.length; i++) setClass(segs[i], 'is-spent', i >= w.clip);
    }
  }

  function renderSelection(sel) {
    const root = $('selection');
    setHidden(root, !sel);
    if (!sel) { cache.delete('selection'); return; }
    const key = JSON.stringify(sel);
    if (cache.get('selection') === key) return;
    cache.set('selection', key);

    root.innerHTML = sel.slots.map((weapons, i) => {
      const items = weapons.map((w) =>
        `<div class="wpn${w.selected ? ' is-selected' : ''}${w.empty ? ' is-empty' : ''}">` +
        `<span class="wpn__icon icons">${escapeHtml(w.icon?.char || '')}</span>` +
        `<span class="wpn__label">${escapeHtml(w.label)}</span></div>`).join('');
      return `<div class="slot${weapons.length ? '' : ' is-empty'}"><div class="slot__num">${i + 1}</div>${items}</div>`;
    }).join('');
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  // ---------------------------------------------------------------- Events
  function onDamage(d) {
    if (d.angle != null && (d.damage > 0 || d.armor > 0)) {
      const arc = document.createElement('div');
      arc.className = `dmg-arc${d.damage > 25 ? ' is-heavy' : ''}`;
      arc.style.transform = `rotate(${d.angle}deg)`;
      $('damage').append(arc);
      arc.addEventListener('animationend', () => arc.remove());
    }
    // Edge flash is drawn by the engine (a full-screen CSS overlay would make
    // Chromium repaint and re-upload the whole frame for its whole animation).
    const [r, g, b, alpha] = d.drown ? [0, 60, 160, 0.4] : d.poison ? [90, 200, 30, 0.35] : d.burn ? [255, 120, 0, 0.38] : [200, 0, 0, 0.4];
    if (Source.IN_GAME) Source.call('hud.flash', { r, g, b, alpha, seconds: 0.8 }).catch(() => {});
  }

  function onPickup(p) {
    const list = $('history');
    const li = document.createElement('li');
    if (p.type === 'denied') li.classList.add('is-denied');
    const glyph = p.icon?.char ? `<span class="h-icon">${escapeHtml(p.icon.char)}</span>` : '';
    const label = p.type === 'weapon' ? p.label : (p.label || '').replace(/^item_/, '').replace(/_/g, ' ');
    const count = p.type === 'ammo' && p.count > 0 ? `<span class="h-count">+${p.count}</span>` : '';
    li.innerHTML = `${glyph}<span class="h-label">${escapeHtml(label)}</span>${count}`;
    list.prepend(li);
    while (list.children.length > 6) list.lastElementChild.remove();
    setTimeout(() => li.classList.add('is-leaving'), p.type === 'denied' ? 1600 : 3600);
    setTimeout(() => li.remove(), p.type === 'denied' ? 2300 : 4300);
  }

  Source.on('hud.state', render);
  Source.on('damage', onDamage);
  Source.on('pickup', onPickup);
  Source.on('movement', onMovement);

  if (Source.IN_GAME) {
    Source.trackGlass('hud.glass', '.glass, #selection .wpn, #selection .slot__num');
    Source.call('hud.ready').catch((e) => console.error('[hud] ready failed', e));
    Source.call('game.info').then((i) => I18N.start(i.language)).catch(() => I18N.start(''));
    return;
  }

  // ---------------------------------------------------------------- Preview
  document.documentElement.style.background = '#6b6f73 linear-gradient(160deg,#a36a35,#6d7077 60%,#5d6064)';
  let t = 0;
  const demo = {
    alive: true, health: 100, suit: true, armor: 65,
    hidden: {}, aux: { power: 100 }, squad: { members: 3, medics: 1, following: true }, zoom: false,
    weapon: { label: 'SMG', icon: { char: 'a' }, usesAmmo: true, clip: 45, maxClip: 45, reserve: 135, secondary: 2 },
    selection: null,
  };
  setInterval(() => {
    t++;
    demo.weapon.clip = demo.weapon.clip > 0 ? demo.weapon.clip - 1 : 45;
    demo.speed = Math.round(330 + 160 * Math.sin(t / 6));
    demo.time = t * 250;
    demo.movement = Math.floor(t / 30) % 3;
    if (t % 30 === 0) onMovement({ mode: demo.movement });
    demo.aux = { power: 100 - (t % 50) * 2, sprint: t % 50 < 40 };
    if (t % 15 === 0) { demo.health = Math.max(5, demo.health - 13); onDamage({ angle: (t * 47) % 360, damage: 13 }); }
    if (t % 23 === 0) onPickup({ type: 'ammo', label: 'SMG1', count: 45, icon: { char: 'r' } });
    demo.selection = t % 40 < 10 ? { slots: [[{ label: 'Crowbar', icon: { char: 'c' } }], [{ label: '9mm Pistol', icon: { char: 'd' } }], [{ label: 'SMG', icon: { char: 'a' }, selected: true }, { label: 'Pulse rifle', icon: { char: 'l' } }], [], [], []] } : null;
    render(JSON.parse(JSON.stringify(demo)));
  }, 250);
})();
