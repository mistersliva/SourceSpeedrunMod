/*
 * prefs.js — interface and HUD customization, shared by index.html and
 * hud.html. Stored in localStorage (both pages share the gameui.local origin);
 * the other page picks up changes through the "storage" event.
 *
 *   Prefs.get('hud.scale')          value (or default)
 *   Prefs.set('hud.scale', 1.2)     store + notify both pages
 *   Prefs.onChange(fn)              fn(key, value), also for the other page
 *   Prefs.reset(prefix)             back to defaults ('ui.' or 'hud.')
 */
(function (global) {
  'use strict';

  const KEY = 'htmlui.prefs';

  const ACCENTS = [
    { value: '#7cc8f5', label: 'Light blue' },
    { value: '#f2f2f2', label: 'White' },
    { value: '#86dc9a', label: 'Green' },
    { value: '#f2a54a', label: 'Orange' },
    { value: '#f28bb8', label: 'Pink' },
    { value: '#f26d6d', label: 'Red' },
  ];

  const DEFAULTS = {
    // Interface
    'ui.accent': '#7cc8f5',
    'ui.font': 'din',
    'ui.scale': 1,
    'ui.dim': 0.35,
    'ui.animations': true,
    'ui.quick': true,
    'ui.bgBlur': true,
    'ui.blur': 'high',
    // HUD
    'hud.scale': 1,
    'hud.opacity': 1,
    'hud.accent': '#7cc8f5',
    'hud.speed': true,
    'hud.speedPos': 'bottom',
    'hud.timerMode': 'run',   // bottom panel: off | run | map
    // Speedrun timer (js/splits.js)
    'timer.file': '',
    'timer.panel': true,
    'timer.side': 'right',
    'timer.rows': 10,
    'timer.method': 'game',
    'timer.autoStartEnd': true,   // SourceSplit: reset on new game, HL2 start/end
    'timer.autoSplit': true,      // split on level transitions
    'timer.splitInterval': 1,
    'timer.splitExclude': '',
    'timer.countPauses': true,
    // Replays (engine: ssm_replay_record), started and stopped with the timer
    'replay.autoRecord': true,
    'replay.keepResets': false,
    'hud.mode': true,
    // Crosshair (js/crosshair.js)
    'hud.crosshair': true,
    'hud.chStyle': 'cross-dot',
    'hud.chColor': '#f2f2f2',
    'hud.chSize': 6,
    'hud.chGap': 4,
    'hud.chThickness': 2,
    'hud.chOutline': true,
    'hud.chOpacity': 0.9,
    'hud.vitals': true,
    'hud.ammo': true,
    'hud.panels': true,
  };

  let values = {};

  function load() {
    try { values = JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch { values = {}; }
    // Older builds stored the speedometer toggle on its own.
    try {
      const legacy = localStorage.getItem('htmlui.speedometer');
      if (legacy != null && !('hud.speed' in values)) values['hud.speed'] = legacy !== '0';
    } catch {}
    if ('hud.timer' in values) {
      if (values['hud.timer'] === false && !('hud.timerMode' in values)) values['hud.timerMode'] = 'off';
      delete values['hud.timer'];
    }
  }

  function save() {
    try { localStorage.setItem(KEY, JSON.stringify(values)); } catch {}
  }

  const listeners = [];
  function notify(key) { for (const fn of listeners) fn(key, get(key)); }

  function get(key) { return key in values ? values[key] : DEFAULTS[key]; }

  function set(key, value) {
    if (get(key) === value) return;
    values[key] = value;
    save();
    notify(key);
  }

  function reset(prefix) {
    for (const k of Object.keys(DEFAULTS)) if (k.startsWith(prefix)) delete values[k];
    save();
    for (const k of Object.keys(DEFAULTS)) if (k.startsWith(prefix)) notify(k);
  }

  function onChange(fn) { listeners.push(fn); }

  addEventListener('storage', (e) => {
    if (e.key !== KEY) return;
    const before = { ...values };
    load();
    for (const k of Object.keys(DEFAULTS)) if (before[k] !== values[k]) notify(k);
  });

  load();
  global.Prefs = { get, set, reset, onChange, DEFAULTS, ACCENTS };
})(window);
