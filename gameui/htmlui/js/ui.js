/*
 * ui.js — small declarative UI layer on top of source.js.
 *
 * Layers:     <section class="layer" data-layer="options">…</section>
 *             UI.open("options"), UI.close("options"), UI.closeTop()
 *             While any layer is open the engine's Esc-to-resume is blocked
 *             and Esc closes the top layer instead.
 * Modals:     await UI.modal({ title, text, buttons: [{ label, value, primary }] })
 * Toasts:     UI.toast("Saved")
 * Bindings (applied by UI.bind(root), automatically for new DOM):
 *   data-loc="#Token"                     text content from localization
 *   data-loc-placeholder="#Token"         placeholder attribute
 *   data-cvar="name" on <input type=range|checkbox|text> / <select>
 *       range:    min/max/step, data-format="percent|int|fixed1|fixed2"
 *       checkbox: data-on="1" data-off="0" (defaults)
 *   data-cmd="console command"            on any clickable element
 *   data-open="layer" / data-close        open / close layers
 *   data-tabs + [data-tab] / [data-panel] tab groups
 */
(function (global) {
  'use strict';
  const { L } = Source;

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v == null || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'text') el.textContent = v;
      else if (k === 'html') el.innerHTML = v;
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
      else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
      else el.setAttribute(k, v === true ? '' : v);
    }
    for (const c of children.flat()) {
      if (c == null || c === false) continue;
      el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return el;
  }

  // ---------------------------------------------------------------------------
  // Layers
  // ---------------------------------------------------------------------------
  const stack = [];
  const hooks = new Map(); // layer -> { onOpen, onClose }

  // Escape interceptors run (newest first) before the top layer is closed;
  // returning true consumes the key.
  const interceptors = [];
  let modalDepth = 0;

  function syncEscape() {
    for (const b of document.querySelectorAll('[data-open]')) b.classList.toggle('is-current', stack.includes(b.dataset.open));
    const prevent = stack.length > 0 || modalDepth > 0 || interceptors.length > 0;
    Source.call('ui.escape', { prevent }).catch(() => {});
    document.body.classList.toggle('has-layer', stack.length > 0);
    document.body.classList.toggle('has-modal', modalDepth > 0);
  }

  function interceptEscape(fn) {
    interceptors.push(fn);
    syncEscape();
    return () => {
      const i = interceptors.indexOf(fn);
      if (i !== -1) { interceptors.splice(i, 1); syncEscape(); }
    };
  }

  function layerEl(name) {
    return $(`[data-layer="${name}"]`);
  }

  function open(name, arg) {
    const el = layerEl(name);
    if (!el) { console.warn('[ui] unknown layer', name); return; }
    const idx = stack.indexOf(name);
    if (idx !== -1) stack.splice(idx, 1);
    stack.push(name);
    clearTimeout(el.__closeTimer);
    el.classList.remove('is-closing');
    el.hidden = false;
    // restart entrance animation
    el.classList.remove('is-open');
    void el.offsetWidth;
    el.classList.add('is-open');
    el.style.zIndex = String(100 + stack.length);
    if (!el.dataset.silent) Source.sound('open');
    hooks.get(name)?.onOpen?.(el, arg);
    syncEscape();
    requestAnimationFrame(() => {
      const f = el.querySelector('[autofocus]') || el.querySelector('.panel__body .btn, .panel__body button, .panel__body input, .panel__body select, .btn--primary');
      f?.focus({ preventScroll: true });
    });
  }

  function close(name) {
    const idx = stack.indexOf(name);
    if (idx === -1) return false;
    const el = layerEl(name);
    if (hooks.get(name)?.canClose?.(el) === false) return false;
    stack.splice(idx, 1);
    el.classList.remove('is-open');
    // Slide out, then hide (unless it was reopened meanwhile).
    const ms = document.documentElement.classList.contains('no-anim') || !document.body.classList.contains('ui-visible') ? 0 : 170;
    if (ms) {
      el.classList.add('is-closing');
      clearTimeout(el.__closeTimer);
      el.__closeTimer = setTimeout(() => { el.classList.remove('is-closing'); el.hidden = true; }, ms);
    } else {
      el.hidden = true;
    }
    hooks.get(name)?.onClose?.(el);
    syncEscape();
    return true;
  }

  function closeTop() {
    if (!stack.length) return false;
    return close(stack[stack.length - 1]);
  }

  function closeAll() {
    while (stack.length) if (!close(stack[stack.length - 1])) break;
  }

  function define(name, h) { hooks.set(name, h); }

  // ---------------------------------------------------------------------------
  // Modal / toast
  // ---------------------------------------------------------------------------
  function modal({ title, text, html, buttons = [{ label: L('#GameUI_OK', 'OK'), value: true, primary: true }], cancelValue = null }) {
    return new Promise((resolve) => {
      const root = $('#modal-root');
      let done = false;
      const finish = (v) => {
        if (done) return;
        done = true;
        off();
        overlay.classList.add('is-leaving');
        setTimeout(() => overlay.remove(), 160);
        modalDepth--;
        syncModalEscape();
        resolve(v);
      };
      const box = h('div', { class: 'modal', role: 'dialog' },
        title && h('h2', { class: 'modal__title', text: title }),
        (text || html) && h('div', { class: 'modal__body', ...(html ? { html } : { text }) }),
        h('div', { class: 'modal__buttons' },
          buttons.map((b) => h('button', {
            class: `btn ${b.primary ? 'btn--primary' : ''} ${b.danger ? 'btn--danger' : ''}`,
            text: b.label,
            onclick: () => finish(b.value),
          }))));
      const overlay = h('div', { class: 'modal-overlay' }, box);
      overlay.addEventListener('mousedown', (e) => { if (e.target === overlay) finish(cancelValue); });
      const off = interceptEscape(() => { finish(cancelValue); return true; });
      root.append(overlay);
      modalDepth++;
      syncModalEscape();
      Source.sound('notify');
      requestAnimationFrame(() => (box.querySelector('.btn--primary') || box.querySelector('.btn'))?.focus());
    });
  }

  function syncModalEscape() { syncEscape(); }

  function toast(text, kind = 'info', ms = 2600) {
    Source.sound(kind === 'error' ? 'error' : kind === 'success' ? 'success' : 'notify');
    const t = h('div', { class: `toast toast--${kind}`, text });
    $('#toast-root').append(t);
    setTimeout(() => t.classList.add('is-leaving'), ms);
    setTimeout(() => t.remove(), ms + 300);
  }

  // ---------------------------------------------------------------------------
  // Declarative bindings
  // ---------------------------------------------------------------------------
  const pendingSets = new Map();
  function setCvarDebounced(name, value, delay = 60) {
    clearTimeout(pendingSets.get(name));
    pendingSets.set(name, setTimeout(() => {
      pendingSets.delete(name);
      Source.cvar.set(name, value).catch((e) => console.warn(e.message));
    }, delay));
  }

  function formatValue(el, v) {
    const n = Number(v);
    switch (el.dataset.format) {
      case 'percent': return `${Math.round(n * 100)}%`;
      case 'int': return String(Math.round(n));
      case 'fixed1': return n.toFixed(1);
      case 'fixed2': return n.toFixed(2);
      default: return String(v);
    }
  }

  function updateRangeVisual(input) {
    const min = Number(input.min || 0), max = Number(input.max || 100);
    const pct = max > min ? ((Number(input.value) - min) / (max - min)) * 100 : 0;
    input.style.setProperty('--fill', `${pct}%`);
    const out = input.id && $(`output[for="${input.id}"]`);
    if (out) out.textContent = formatValue(input, input.value);
  }

  function applyCvarValue(el, value) {
    if (value == null) { el.closest('.field')?.classList.add('is-unavailable'); return; }
    if (el.type === 'checkbox') {
      const on = el.dataset.on ?? '1';
      el.checked = Number.isNaN(Number(on)) ? value === on : Number(value) === Number(on);
    } else if (el.tagName === 'SELECT') {
      const match = Array.from(el.options).find((o) => o.value === value || Number(o.value) === Number(value));
      el.value = match ? match.value : el.options[0]?.value;
    } else {
      el.value = value;
      if (el.type === 'range') updateRangeVisual(el);
    }
  }

  async function refreshCvars(root = document) {
    const els = $$('[data-cvar]', root);
    if (!els.length) return;
    const names = [...new Set(els.map((e) => e.dataset.cvar))];
    const values = await Source.cvar.get(names);
    for (const el of els) {
      const cv = values[el.dataset.cvar];
      applyCvarValue(el, cv ? cv.value : null);
      syncCycler(el);
    }
  }

  // Keep every control bound to the same cvar in step (quick settings and
  // the settings page both show sv_movement_mode, for example).
  document.addEventListener('cvarchange', (e) => {
    const { name, value } = e.detail;
    for (const el of $$(`[data-cvar="${name}"]`)) {
      if (el === e.target) continue;
      applyCvarValue(el, value);
      syncCycler(el);
    }
  });

  // ---------------------------------------------------------------------------
  // Value cyclers: "‹ value ›" in place of <select> and switch checkboxes in
  // settings rows. The native control stays in the DOM (hidden) and keeps its
  // bindings; the cycler only changes it and fires "change".
  // ---------------------------------------------------------------------------
  const CHEVRON = (d) => `<svg viewBox="0 0 16 16"><path d="${d}"/></svg>`;

  function syncCycler(el) {
    const c = el.__cycler;
    if (!c) return;
    const value = c.querySelector('.cycler__value');
    const isCheck = el.type === 'checkbox';
    const opt = isCheck ? null : el.options[el.selectedIndex];
    value.replaceChildren();
    if (opt?.dataset.swatch) value.append(h('i', { class: 'cycler__swatch', style: { background: opt.dataset.swatch } }));
    value.append(isCheck ? (el.checked ? 'Enabled' : 'Disabled') : (opt?.textContent ?? ''));
    c.classList.toggle('is-on', isCheck && el.checked);
    c.classList.toggle('is-off', isCheck && !el.checked);
    for (const b of c.querySelectorAll('.cycler__btn')) b.disabled = el.disabled || (!isCheck && el.options.length < 2);
  }

  function enhanceCycler(el) {
    if (el.__cycler || el.dataset.noCycle != null) return;
    const isCheck = el.type === 'checkbox';
    const host = isCheck ? el.closest('.switch') : el;
    if (!host || !host.parentElement?.closest('.field')) return;
    const prev = h('button', { class: 'cycler__btn', type: 'button', 'aria-label': 'Previous', html: CHEVRON('M10 3 5 8l5 5') });
    const next = h('button', { class: 'cycler__btn', type: 'button', 'aria-label': 'Next', html: CHEVRON('m6 3 5 5-5 5') });
    const c = h('div', { class: 'cycler' }, prev, h('span', { class: 'cycler__value' }), next);
    host.classList.add('is-cycled');
    host.after(c);
    el.__cycler = c;
    const step = (dir) => {
      if (el.disabled) return;
      if (isCheck) {
        el.checked = !el.checked;
      } else {
        const n = el.options.length;
        if (n < 2) return;
        el.selectedIndex = (el.selectedIndex + dir + n) % n;
        Source.sound('click');
      }
      el.dispatchEvent(new Event('change', { bubbles: true }));
    };
    for (const [b, dir] of [[prev, -1], [next, 1]]) {
      b.addEventListener('click', (e) => { e.preventDefault(); e.stopPropagation(); step(dir); });
    }
    el.addEventListener('change', () => syncCycler(el));
    syncCycler(el);
  }

  function syncControls(root = document) {
    for (const el of $$('select, input[type="checkbox"]', root)) syncCycler(el);
  }

  function bindCvar(el) {
    if (el.__cvarBound) return;
    el.__cvarBound = true;
    const name = el.dataset.cvar;
    const handler = () => {
      let v;
      if (el.type === 'checkbox') v = el.checked ? (el.dataset.on ?? '1') : (el.dataset.off ?? '0');
      else v = el.value;
      if (el.type === 'checkbox') Source.sound(el.checked ? 'toggle_on' : 'toggle_off');
      if (el.type === 'range') Source.sound('slider', { rate: 0.85 + 0.3 * ((Number(el.value) - Number(el.min || 0)) / ((Number(el.max || 1) - Number(el.min || 0)) || 1)) });
      if (el.type === 'range') updateRangeVisual(el);
      setCvarDebounced(name, v, el.type === 'range' ? 80 : 0);
      el.dispatchEvent(new CustomEvent('cvarchange', { bubbles: true, detail: { name, value: v } }));
    };
    el.addEventListener(el.type === 'range' || el.type === 'text' ? 'input' : 'change', handler);
  }

  function bind(root = document) {
    for (const el of $$('[data-loc]', root)) el.textContent = L(el.dataset.loc, el.textContent || undefined);
    for (const el of $$('[data-loc-placeholder]', root)) el.placeholder = L(el.dataset.locPlaceholder, el.placeholder);
    for (const el of $$('[data-loc-title]', root)) el.title = L(el.dataset.locTitle, el.title);
    for (const el of $$('[data-cvar]', root)) bindCvar(el);
    for (const el of $$('.field select, .field .switch input[type="checkbox"]', root)) enhanceCycler(el);
    for (const el of $$('input[type=range]', root)) {
      if (!el.__rangeBound) { el.__rangeBound = true; el.addEventListener('input', () => updateRangeVisual(el)); }
      updateRangeVisual(el);
    }
    for (const group of $$('[data-tabs]', root)) bindTabs(group);
  }

  function bindTabs(group) {
    if (group.__tabsBound) return;
    group.__tabsBound = true;
    const tabs = $$('[data-tab]', group);
    const show = (name) => {
      if (group.__tabsShown) Source.sound('tab');
      group.__tabsShown = true;
      for (const t of tabs) t.classList.toggle('is-active', t.dataset.tab === name);
      for (const p of $$('[data-panel]', group)) p.hidden = p.dataset.panel !== name;
      group.dispatchEvent(new CustomEvent('tabchange', { detail: { tab: name } }));
    };
    for (const t of tabs) t.addEventListener('click', () => show(t.dataset.tab));
    group.showTab = show;
    show((tabs.find((t) => t.classList.contains('is-active')) || tabs[0])?.dataset.tab);
  }

  // Global delegated actions.
  document.addEventListener('click', (e) => {
    const cmd = e.target.closest('[data-cmd]');
    if (cmd) Source.cmd(cmd.dataset.cmd);
    const openEl = e.target.closest('[data-open]');
    if (openEl) {
      // Main menu pages replace each other instead of stacking.
      if (openEl.closest('.mainmenu')) {
        for (const name of stack.slice()) if (name !== openEl.dataset.open && name !== 'console') close(name);
      }
      open(openEl.dataset.open);
    }
    const closeEl = e.target.closest('[data-close]');
    if (closeEl) close(closeEl.closest('[data-layer]')?.dataset.layer);
    const snd = e.target.closest('[data-sound]');
    if (snd) Source.sound(snd.dataset.sound);
    else if (e.target.closest('[data-close], .icon-btn')) Source.sound('back');
    else if (e.target.closest('.btn, .menu__item, .chapter, .save, .bind, .segmented button')) Source.sound('click');
    // (tabs play their own sound when switching)
  });

  let lastHover = null;
  document.addEventListener('mouseover', (e) => {
    const b = e.target.closest('.btn:not(:disabled), .menu__item, .chapter:not(:disabled), .save, .bind, .tabs button, .icon-btn');
    if (b && b !== lastHover) Source.sound('hover');
    lastHover = b;
  });

  // Keyboard navigation for vertical menus.
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const menu = document.activeElement?.closest('[data-nav]');
    if (!menu) return;
    const items = $$('.menu__item:not([hidden]):not(:disabled)', menu);
    const i = items.indexOf(document.activeElement);
    const next = items[(i + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length];
    next?.focus();
    e.preventDefault();
  });

  // Escape closes the top-most layer (modals handle their own escape).
  Source.on('escape', () => {
    for (let i = interceptors.length - 1; i >= 0; i--) if (interceptors[i]()) return;
    if (closeTop()) Source.sound('back');
  });

  // ---------------------------------------------------------------------------
  // Smooth wheel scrolling. The engine delivers one 120-unit step per wheel
  // notch; ease towards the target each frame instead of jumping.
  // ---------------------------------------------------------------------------
  const smooth = new Map(); // element -> { target, raf }

  function scrollableParent(el) {
    for (let n = el; n && n !== document.body; n = n.parentElement) {
      if (n.scrollHeight <= n.clientHeight + 1) continue;
      const oy = getComputedStyle(n).overflowY;
      if (oy === 'auto' || oy === 'scroll') return n;
    }
    return null;
  }

  function animateScroll(el) {
    const s = smooth.get(el);
    const max = el.scrollHeight - el.clientHeight;
    s.target = Math.max(0, Math.min(max, s.target));
    const diff = s.target - s.pos;
    if (Math.abs(diff) < 0.5) {
      el.scrollTop = s.pos = s.target;
      smooth.delete(el);
      return;
    }
    // Frame-rate independent exponential easing (~90 ms time constant).
    const now = performance.now();
    const dt = Math.min(50, now - (s.time || now - 16));
    s.time = now;
    s.pos += diff * (1 - Math.exp(-dt / 90));
    el.scrollTop = s.pos;
    s.raf = requestAnimationFrame(() => animateScroll(el));
  }

  document.addEventListener('wheel', (e) => {
    if (e.defaultPrevented || e.ctrlKey) return;
    const el = scrollableParent(e.target);
    if (!el) return;
    e.preventDefault();
    const step = e.deltaMode === 1 ? e.deltaY * 40 : e.deltaMode === 2 ? e.deltaY * el.clientHeight : e.deltaY;
    let s = smooth.get(el);
    if (!s) {
      s = { target: el.scrollTop, pos: el.scrollTop, time: 0 };
      smooth.set(el, s);
      s.target += step;
      animateScroll(el);
    } else {
      s.target += step;
    }
  }, { passive: false });

  new MutationObserver((records) => {
    for (const r of records) for (const n of r.addedNodes) if (n.nodeType === 1) bind(n.parentNode || n);
  }).observe(document.documentElement, { childList: true, subtree: true });

  global.UI = { $, $$, h, interceptEscape, open, close, closeTop, closeAll, define, modal, toast, bind, refreshCvars, syncControls, stack, get isOpen() { return stack.length > 0 || modalDepth > 0; } };
})(window);
