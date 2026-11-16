/*
 * app.js — Half-Life 2 menus built on source.js + ui.js.
 */
(function () {
  'use strict';
  const { L } = Source;
  const { $, $$, h } = UI;

  let info = {};
  let state = {};
  let loading = false;

  // ===========================================================================
  // State / main menu
  // ===========================================================================
  function applyState(next) {
    if (next) state = { ...state, ...next };
    const b = document.body.classList;
    b.toggle('in-game', !!state.inGame);
    b.toggle('in-background-map', !!state.inBackgroundLevel);
    b.toggle('no-map', !state.inLevel && !state.inBackgroundLevel);

    for (const el of $$('[data-when]')) {
      const cond = el.dataset.when;
      el.hidden = !(
        (cond === 'ingame' && state.inGame) ||
        (cond === 'cansave' && state.canSave) ||
        (cond === 'multiplayer' && state.multiplayer)
      );
    }
    $('#status-level').textContent = state.inGame && state.level ? state.level.replace(/^maps\//, '').replace(/\.bsp$/, '') : '';
  }

  async function confirmQuit() {
    const ok = await UI.modal({
      title: L('#GameUI_QuitConfirmationTitle', 'Quit game'),
      text: L('#GameUI_QuitConfirmationText', 'Are you sure you want to quit?'),
      buttons: [
        { label: L('#GameUI_Cancel', 'Cancel'), value: false },
        { label: L('#GameUI_Quit', 'Quit'), value: true, primary: true, danger: true },
      ],
    });
    if (ok) {
      Source.sound('quit');
      setTimeout(() => Source.call('quit'), 450);
    }
  }

  async function confirmDisconnect() {
    const ok = await UI.modal({
      title: L('#GameUI_Disconnect', 'Disconnect'),
      text: L('#GameUI_DisconnectConfirmationText', 'Are you sure you want to disconnect from the game?'),
      buttons: [
        { label: L('#GameUI_Cancel', 'Cancel'), value: false },
        { label: L('#GameUI_Disconnect', 'Disconnect'), value: true, primary: true },
      ],
    });
    if (ok) Source.cmd('disconnect');
  }

  const actions = {
    resume: () => Source.cmd('gameui_hide'),
    console: () => Source.cmd('showconsole'),
    quit: confirmQuit,
    disconnect: confirmDisconnect,
    'movement-cycle': () => { Source.cmd('movement_cycle'); setTimeout(refreshMovement, 200); },
  };

  document.addEventListener('click', (e) => {
    const a = e.target.closest('[data-action]');
    if (a && actions[a.dataset.action]) actions[a.dataset.action]();
  });

  const menuCommands = {
    OpenNewGameDialog: () => UI.open('newgame'),
    OpenLoadGameDialog: () => UI.open('loadgame'),
    OpenSaveGameDialog: () => state.canSave && UI.open('savegame'),
    OpenOptionsDialog: () => UI.open('options'),
    OpenConsole: () => Source.cmd('showconsole'),
    Quit: confirmQuit,
    Disconnect: confirmDisconnect,
  };

  // ===========================================================================
  // New game
  // ===========================================================================
  let selectedChapter = null;
  let skill = 2;

  function setSkill(v) {
    skill = Number(v) || 2;
    for (const b of $$('#difficulty [data-skill]')) b.classList.toggle('is-active', Number(b.dataset.skill) === skill);
  }

  $('#difficulty').addEventListener('click', (e) => {
    const b = e.target.closest('[data-skill]');
    if (b) { e.preventDefault(); setSkill(b.dataset.skill); }
  });

  function selectChapter(card, ch) {
    for (const c of $$('.chapter', $('#chapter-grid'))) c.classList.remove('is-selected');
    card.classList.add('is-selected');
    selectedChapter = ch;
    $('#start-game').disabled = false;
  }

  async function startNewGame() {
    if (!selectedChapter) return;
    if (state.inGame) {
      const ok = await UI.modal({
        title: L('#GameUI_NewGame', 'New Game'),
        text: L('#GameUI_NewGameWarning', 'Starting a new game will lose any unsaved progress. Continue?'),
        buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: false }, { label: L('#GameUI_OK', 'OK'), value: true, primary: true }],
      });
      if (!ok) return;
    }
    const args = { config: selectedChapter.config, commentary: $('#commentary').checked };
    if (!info.noDifficulty) args.skill = skill;
    Source.sound('start');
    UI.closeAll();
    await Source.call('newgame', args);
  }
  $('#start-game').addEventListener('click', startNewGame);

  UI.define('newgame', {
    async onOpen(el, chapterHint) {
      const grid = $('#chapter-grid');
      grid.replaceChildren(h('div', { class: 'placeholder', text: L('#GameUI_Loading', 'Loading…') }));
      selectedChapter = null;
      $('#start-game').disabled = true;
      $('#difficulty-field').hidden = !!info.noDifficulty;

      const [chapters, cv] = await Promise.all([Source.call('chapters'), Source.cvar.get(['skill'])]);
      setSkill(cv.skill ? cv.skill.value : 2);

      const chapterWord = L('#GameUI_Chapter', 'Chapter');
      grid.replaceChildren(...chapters.map((ch) => {
        const card = h('button', { class: `chapter ${ch.locked ? 'is-locked' : ''}`, disabled: ch.locked },
          h('div', { class: 'chapter__image', style: ch.image ? { backgroundImage: `url("${ch.image}")` } : null }),
          h('div', { class: 'chapter__meta' },
            h('div', { class: 'chapter__number', text: `${chapterWord} ${ch.id}` }),
            h('div', { class: 'chapter__title', text: ch.title })),
          ch.locked && h('div', { class: 'chapter__lock', text: '🔒' }));
        card.addEventListener('click', () => selectChapter(card, ch));
        card.addEventListener('dblclick', () => { selectChapter(card, ch); startNewGame(); });
        return card;
      }));
      const first = typeof chapterHint === 'number' ? grid.children[chapterHint - 1] : grid.querySelector('.chapter:not(.is-locked)');
      if (first && !first.disabled) { first.click(); first.focus(); }
    },
  });

  // ===========================================================================
  // Load / save
  // ===========================================================================
  function formatTime(seconds) {
    if (!seconds) return '';
    const d = new Date(seconds * 1000);
    return d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
  }

  function saveItem(save, onSelect, onActivate) {
    const typeLabel = save.type === 'quick' ? L('#GameUI_QuickSave', 'Quick Save') : save.type === 'auto' ? L('#GameUI_AutoSave', 'Autosave') : '';
    const li = h('li', { class: 'save', tabindex: 0 },
      h('div', { class: 'save__thumb', style: save.thumb ? { backgroundImage: `url("${save.thumb}?t=${save.time}")` } : null }),
      h('div', { class: 'save__info' },
        h('div', { class: 'save__title' }, save.comment || save.map, typeLabel && h('span', { class: 'badge', text: typeLabel })),
        h('div', { class: 'save__detail', text: `${save.map} · ${save.elapsed}` }),
        h('div', { class: 'save__time', text: formatTime(save.time) })));
    li.addEventListener('click', () => onSelect(li, save));
    li.addEventListener('dblclick', () => onActivate(save));
    li.addEventListener('keydown', (e) => { if (e.key === 'Enter') onActivate(save); });
    return li;
  }

  function selectIn(list, li) {
    for (const x of $$('.save', list)) x.classList.remove('is-selected');
    li.classList.add('is-selected');
  }

  async function confirmDelete(save) {
    return UI.modal({
      title: L('#GameUI_ConfirmDeleteSaveGame_Title', 'Delete saved game'),
      text: L('#GameUI_ConfirmDeleteSaveGame_Info', 'Are you sure you want to delete this saved game?'),
      buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: false }, { label: L('#GameUI_Delete', 'Delete'), value: true, primary: true, danger: true }],
    });
  }

  // Load
  let loadSelected = null;
  async function refreshLoad() {
    const list = $('#load-list');
    loadSelected = null;
    $('#load-confirm').disabled = $('#load-delete').disabled = true;
    const saves = await Source.call('saves.list');
    if (!saves.length) {
      list.replaceChildren(h('li', { class: 'placeholder', text: L('#GameUI_NoSaveGamesToDisplay', 'No saved games to display') }));
      return;
    }
    list.replaceChildren(...saves.map((s) => saveItem(s, (li, save) => {
      selectIn(list, li);
      loadSelected = save;
      $('#load-confirm').disabled = $('#load-delete').disabled = false;
    }, loadGame)));
    list.querySelector('.save')?.click();
  }

  async function loadGame(save) {
    if (!save) return;
    Source.sound('start');
    UI.closeAll();
    await Source.call('saves.load', { file: save.file });
  }

  $('#load-confirm').addEventListener('click', () => loadGame(loadSelected));
  $('#load-delete').addEventListener('click', async () => {
    if (loadSelected && await confirmDelete(loadSelected)) {
      await Source.call('saves.delete', { file: loadSelected.file });
      refreshLoad();
    }
  });
  UI.define('loadgame', { onOpen: refreshLoad });

  // Replays (engine: ssm_replay_play / ssm_replay_render)
  let replaySelected = null;
  let replayFfmpeg = false;
  const replayMap = (name) => name.replace(/^\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d_?/, '') || name;
  const replayDate = (name) => {
    const m = /^(\d{4})-(\d\d)-(\d\d)_(\d\d)-(\d\d)/.exec(name);
    return m ? `${m[3]}.${m[2]}.${m[1]} ${m[4]}:${m[5]}` : '';
  };
  const formatSize = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(b / 1024))} KB`);

  function updateReplayButtons() {
    const has = !!replaySelected;
    $('#replay-play').disabled = $('#replay-delete').disabled = !has;
    $('#replay-render').disabled = !has || !replayFfmpeg;
    $('#replay-render').title = replayFfmpeg ? '' : 'Install ffmpeg in the launcher (Settings) to render videos';
  }

  async function refreshReplays() {
    const list = $('#replay-list');
    replaySelected = null;
    const res = await Source.call('replays.list');
    replayFfmpeg = !!res.ffmpeg;
    updateReplayButtons();
    if (!res.replays.length) {
      list.replaceChildren(h('li', { class: 'placeholder', text: 'No replays yet. Runs are recorded while the timer runs.' }));
      return;
    }
    list.replaceChildren(...res.replays.map((r) => {
      const parts = r.parts === 1 ? '1 part' : `${r.parts} parts`;
      const li = h('li', { class: 'save save--nothumb', tabindex: 0 },
        h('div', { class: 'save__info' },
          h('div', { class: 'save__title' }, replayMap(r.name), r.mp4 && h('span', { class: 'badge', text: 'MP4' })),
          h('div', { class: 'save__detail', text: `${parts} · ${formatSize(r.size)}` }),
          h('div', { class: 'save__time', text: replayDate(r.name) || formatTime(r.time) })));
      li.addEventListener('click', () => { selectIn(list, li); replaySelected = r; updateReplayButtons(); });
      li.addEventListener('dblclick', () => playReplay(r));
      li.addEventListener('keydown', (e) => { if (e.key === 'Enter') playReplay(r); });
      return li;
    }));
    list.querySelector('.save')?.click();
  }

  function playReplay(r) {
    if (!r) return;
    Source.sound('start');
    UI.closeAll();
    Source.cmd(`ssm_replay_play ${r.name}`);
  }

  $('#replay-play').addEventListener('click', () => playReplay(replaySelected));
  $('#replay-render').addEventListener('click', async () => {
    const r = replaySelected;
    if (!r) return;
    const fps = await UI.modal({
      title: 'Render MP4',
      text: `The replay plays back while frames are saved to replays/${r.name}.mp4. The game is silent while it renders; the video is ready when the replay ends.`,
      buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: 0 }, { label: '30 fps', value: 30 }, { label: '60 fps', value: 60, primary: true }],
    });
    if (!fps) return;
    UI.closeAll();
    Source.cmd(`ssm_replay_render ${r.name} ${fps}`);
  });
  $('#replay-delete').addEventListener('click', async () => {
    const r = replaySelected;
    if (!r) return;
    const ok = await UI.modal({
      title: 'Delete replay',
      text: r.mp4 ? 'Delete this replay? Its rendered MP4 is deleted too.' : 'Delete this replay?',
      buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: false }, { label: L('#GameUI_Delete', 'Delete'), value: true, primary: true, danger: true }],
    });
    if (!ok) return;
    await Source.call('replays.delete', { name: r.name, video: true });
    refreshReplays();
  });
  $('#replay-folder').addEventListener('click', () => Source.call('replays.folder', replaySelected?.mp4 ? { video: replaySelected.name } : {}));
  UI.define('replays', { onOpen: refreshReplays });

  // Save
  let saveSelected = null; // null = new slot
  async function refreshSave() {
    const list = $('#save-list');
    const saves = await Source.call('saves.list');
    const newItem = h('li', { class: 'save save--new', tabindex: 0 },
      h('div', { class: 'save__thumb save__thumb--new', text: '+' }),
      h('div', { class: 'save__info' }, h('div', { class: 'save__title', text: L('#GameUI_NewSaveGame', 'New Saved Game') }),
        h('div', { class: 'save__detail', text: state.level || '' })));
    newItem.addEventListener('click', () => { selectIn(list, newItem); saveSelected = null; $('#save-delete').disabled = true; });
    newItem.addEventListener('dblclick', doSave);
    list.replaceChildren(newItem, ...saves.filter((s) => s.type === 'manual').map((s) => saveItem(s, (li, save) => {
      selectIn(list, li);
      saveSelected = save;
      $('#save-delete').disabled = false;
    }, doSave)));
    newItem.click();
  }

  async function doSave() {
    if (saveSelected) {
      const ok = await UI.modal({
        title: L('#GameUI_ConfirmOverwriteSaveGame_Title', 'Overwrite saved game'),
        text: L('#GameUI_ConfirmOverwriteSaveGame_Info', 'Are you sure you want to overwrite this saved game?'),
        buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: false }, { label: L('#GameUI_ConfirmOverwriteSaveGame_OK', 'Overwrite'), value: true, primary: true }],
      });
      if (!ok) return;
    }
    try {
      await Source.call('saves.save', saveSelected ? { file: saveSelected.file } : {});
      UI.closeAll();
      UI.toast(L('#GameUI_SavedGame', 'Game saved'), 'success');
      setTimeout(() => Source.cmd('gameui_hide'), 250);
    } catch (e) {
      UI.toast(e.message, 'error');
    }
  }

  $('#save-confirm').addEventListener('click', doSave);
  $('#save-delete').addEventListener('click', async () => {
    if (saveSelected && await confirmDelete(saveSelected)) {
      await Source.call('saves.delete', { file: saveSelected.file });
      refreshSave();
    }
  });
  UI.define('savegame', { onOpen: refreshSave });

  // ===========================================================================
  // Options
  // ===========================================================================
  let video = null;

  async function refreshVideo() {
    video = await Source.call('video.get');
    const sel = $('#video-resolution');
    const modes = video.modes.slice().sort((a, b) => a.width - b.width || a.height - b.height);
    if (!modes.some((m) => m.width === video.width && m.height === video.height)) modes.push({ width: video.width, height: video.height });
    sel.replaceChildren(...modes.map((m) => {
      const o = h('option', { value: `${m.width}x${m.height}`, text: `${m.width} × ${m.height}` });
      if (m.width === video.desktopWidth && m.height === video.desktopHeight) o.textContent += '  (native)';
      return o;
    }));
    sel.value = `${video.width}x${video.height}`;
    $('#video-mode').value = video.borderless ? 'borderless' : video.windowed ? 'windowed' : 'fullscreen';
    $('#video-apply').disabled = true;
    UI.syncControls($('[data-panel="video"]'));
  }

  function videoDirty() {
    if (!video) return;
    const [w, hgt] = $('#video-resolution').value.split('x').map(Number);
    const mode = $('#video-mode').value;
    const current = video.borderless ? 'borderless' : video.windowed ? 'windowed' : 'fullscreen';
    $('#video-apply').disabled = w === video.width && hgt === video.height && mode === current;
  }
  $('#video-resolution').addEventListener('change', videoDirty);
  $('#video-mode').addEventListener('change', videoDirty);
  $('#video-apply').addEventListener('click', async () => {
    const [width, height] = $('#video-resolution').value.split('x').map(Number);
    await Source.call('video.set', { width, height, mode: $('#video-mode').value });
    setTimeout(refreshVideo, 1500);
  });

  // Captions: closecaption + cc_subtitles combined.
  async function refreshCaptions() {
    const cv = await Source.cvar.get(['closecaption', 'cc_subtitles']);
    const cc = Number(cv.closecaption?.value || 0), sub = Number(cv.cc_subtitles?.value || 0);
    $('#captions').value = !cc ? '0' : sub ? '2' : '1';
    UI.syncControls($('[data-panel="audio"]'));
  }
  $('#captions').addEventListener('change', async (e) => {
    const v = e.target.value;
    await Source.cvar.set('closecaption', v === '0' ? 0 : 1);
    await Source.cvar.set('cc_subtitles', v === '2' ? 1 : 0);
  });

  // Key bindings
  let capture = null; // { command, row }

  async function refreshBinds() {
    const root = $('#binds');
    const sections = await Source.call('binds.list');
    root.replaceChildren(...sections.map((sec) => h('div', { class: 'binds__section' },
      sec.title && h('h3', { class: 'form-section', text: sec.title }),
      ...sec.items.map((item) => {
        const row = h('button', { class: 'bind', 'data-command': item.command },
          h('span', { class: 'bind__label', text: item.label }),
          h('span', { class: 'bind__keys' }, item.keys.length ? item.keys.map((k) => h('kbd', { text: k })) : h('span', { class: 'bind__none', text: '—' })));
        row.addEventListener('click', () => beginCapture(item, row));
        row.addEventListener('contextmenu', (e) => { e.preventDefault(); clearBinding(item); });
        return row;
      }))));
  }

  function beginCapture(item, row) {
    endCapture();
    capture = { item, row, releaseEscape: UI.interceptEscape(() => { endCapture(); refreshBinds(); return true; }) };
    row.classList.add('is-capturing');
    row.querySelector('.bind__keys').replaceChildren(h('span', { class: 'bind__prompt', text: L('#GameUI_SetNewKey', 'Press a key…') }));
    // Swallow the click that started capture.
    setTimeout(() => { if (capture) capture.armed = true; }, 0);
  }

  function endCapture() {
    if (!capture) return;
    capture.releaseEscape();
    capture.row.classList.remove('is-capturing');
    capture = null;
  }

  async function finishCapture(keyArgs) {
    const c = capture;
    endCapture();
    try {
      await Source.call('binds.set', { command: c.item.command, ...keyArgs });
    } catch (e) {
      UI.toast(e.message, 'error');
    }
    refreshBinds();
  }

  async function clearBinding(item) {
    for (const key of item.keys) await Source.call('binds.unbind', { key }).catch(() => {});
    refreshBinds();
  }

  addEventListener('keydown', (e) => {
    if (!capture) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.key === 'Escape') { endCapture(); refreshBinds(); return; }
    finishCapture({ vk: e.keyCode });
  }, true);
  addEventListener('mousedown', (e) => {
    if (!capture || !capture.armed) return;
    e.preventDefault();
    e.stopPropagation();
    finishCapture({ mouse: e.button });
  }, true);
  addEventListener('wheel', (e) => {
    if (!capture) return;
    e.preventDefault();
    finishCapture({ wheel: e.deltaY < 0 ? 1 : -1 });
  }, { capture: true, passive: false });

  $('#binds-defaults').addEventListener('click', async () => {
    const ok = await UI.modal({
      title: L('#GameUI_KeyboardSettings', 'Keyboard'),
      text: L('#GameUI_KeyboardSettingsText', 'Restore all key bindings to their default values?'),
      buttons: [{ label: L('#GameUI_Cancel', 'Cancel'), value: false }, { label: L('#GameUI_OK', 'OK'), value: true, primary: true }],
    });
    if (!ok) return;
    await Source.call('binds.defaults');
    setTimeout(refreshBinds, 100);
  });

  $('#options-tabs').addEventListener('tabchange', (e) => {
    $('#binds-defaults').hidden = e.detail.tab !== 'keyboard';
    if (e.detail.tab === 'keyboard') refreshBinds();
  });

  // Speedrun timer: .lss files in <mod>/splits.
  async function refreshSplits() {
    const sel = $('#splits-file');
    const files = await Source.call('splits.list').catch(() => []);
    const current = Prefs.get('timer.file');
    if (current && !files.includes(current)) files.unshift(current);
    sel.replaceChildren(h('option', { value: '', text: 'None' }), ...files.map((f) => h('option', { value: f, text: f.replace(/\.lss$/i, '') })));
    sel.value = current;
    UI.syncControls(sel.closest('.field'));
  }
  $('#splits-refresh').addEventListener('click', refreshSplits);
  $('#splits-folder').addEventListener('click', () => Source.call('splits.folder').catch(() => {}));

  UI.define('options', {
    onOpen(el) {
      refreshSplits();
      $('#skill-field').hidden = !!info.noDifficulty;
      for (const x of $$('.cutsceneless-only')) x.hidden = !info.cutsceneless;
      UI.refreshCvars(el);
      refreshVideo();
      refreshCaptions();
      if (!$('[data-panel="keyboard"]').hidden) refreshBinds();
    },
    onClose() {
      endCapture();
      Source.cmd('mat_savechanges\nhost_writeconfig');
    },
  });

  // ===========================================================================
  // Console
  // ===========================================================================
  const out = $('#console-output');
  const input = $('#console-input');
  const suggest = $('#console-suggest');
  const history = [];
  let historyPos = -1;
  let suggestions = [];
  let suggestIndex = -1;
  const MAX_LINES = 4000;
  let partialLine = null;

  function appendConsole(entries) {
    const stick = out.scrollTop + out.clientHeight >= out.scrollHeight - 24;
    const frag = document.createDocumentFragment();
    for (const { text, color, dev } of entries) {
      const parts = text.split('\n');
      parts.forEach((part, i) => {
        if (!partialLine) {
          partialLine = h('div', { class: `console__line${dev ? ' is-dev' : ''}` });
          frag.append(partialLine);
        }
        if (part) {
          const span = h('span', { text: part });
          if (color) span.style.color = `rgba(${color[0]},${color[1]},${color[2]},${(color[3] ?? 255) / 255})`;
          partialLine.append(span);
        }
        if (i < parts.length - 1) partialLine = null;
      });
    }
    out.append(frag);
    while (out.childElementCount > MAX_LINES) out.firstElementChild.remove();
    if (stick) out.scrollTop = out.scrollHeight;
  }

  function renderSuggestions() {
    suggest.hidden = !suggestions.length;
    suggest.replaceChildren(...suggestions.map((s, i) => h('li', {
      class: i === suggestIndex ? 'is-active' : '',
      onmousedown: (e) => { e.preventDefault(); acceptSuggestion(i); },
    }, h('span', { class: 'name', text: s.name }), s.value != null && h('span', { class: 'value', text: s.value }), s.help && h('span', { class: 'help', text: s.help }))));
  }

  function acceptSuggestion(i) {
    const s = suggestions[i];
    if (!s) return;
    input.value = s.name + ' ';
    suggestions = [];
    renderSuggestions();
    input.focus();
  }

  let completeTimer = 0;
  input.addEventListener('input', () => {
    clearTimeout(completeTimer);
    historyPos = -1;
    const text = input.value;
    if (!text || text.includes(' ')) { suggestions = []; renderSuggestions(); return; }
    completeTimer = setTimeout(async () => {
      suggestions = await Source.call('console.complete', { text, limit: 12 }).catch(() => []);
      suggestIndex = -1;
      renderSuggestions();
    }, 60);
  });

  function submitConsole() {
    const text = input.value.trim();
    input.value = '';
    suggestions = [];
    renderSuggestions();
    if (text && history[history.length - 1] !== text) history.push(text);
    historyPos = -1;
    Source.call('console.submit', { text });
  }

  input.addEventListener('keydown', (e) => {
    if (e.code === 'Backquote') { e.preventDefault(); return; }
    if (e.key === 'Enter') { e.preventDefault(); if (suggestIndex >= 0) acceptSuggestion(suggestIndex); else submitConsole(); return; }
    if (e.key === 'Tab') { e.preventDefault(); acceptSuggestion(Math.max(suggestIndex, 0)); return; }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const dir = e.key === 'ArrowDown' ? 1 : -1;
      if (suggestions.length) {
        suggestIndex = (suggestIndex + dir + suggestions.length) % suggestions.length;
        renderSuggestions();
      } else if (history.length) {
        historyPos = historyPos === -1 ? (dir < 0 ? history.length - 1 : -1) : historyPos + dir;
        if (historyPos >= history.length) historyPos = -1;
        if (historyPos < -1) historyPos = 0;
        input.value = historyPos === -1 ? '' : history[historyPos];
      }
    }
  });
  // Drop the backquote character produced by the toggle key.
  input.addEventListener('beforeinput', (e) => { if (e.data === '`' || e.data === '~') e.preventDefault(); });

  $('#console-submit').addEventListener('click', submitConsole);
  $('#console-clear').addEventListener('click', () => { out.replaceChildren(); partialLine = null; });

  UI.define('console', {
    onOpen() { out.scrollTop = out.scrollHeight; setTimeout(() => input.focus(), 30); },
    onClose() { Source.call('console.close').catch(() => {}); },
  });

  Source.on('console.print', appendConsole);
  Source.on('console.show', () => UI.open('console'));
  Source.on('console.hide', () => UI.close('console'));
  Source.on('console.clear', () => { out.replaceChildren(); partialLine = null; });

  // ===========================================================================
  // Loading screen
  // ===========================================================================
  function showLoading(on) {
    loading = on;
    $('#loading').hidden = !on;
    document.body.classList.toggle('is-loading', on);
  }

  let loadingHideTimer = 0;
  function endLoadingScreen() {
    clearTimeout(loadingHideTimer);
    showLoading(false);
    document.body.classList.remove('is-quickload');
  }
  Source.on('loading.start', ({ dialog }) => {
    clearTimeout(loadingHideTimer);
    UI.closeAll();
    $('#loading-bar').style.width = '0%';
    $('#loading-text').textContent = '';
    if (dialog) showLoading(true);
    else document.body.classList.add('is-quickload');
  });
  Source.on('loading.progress', ({ progress, text }) => {
    $('#loading-bar').style.width = `${Math.round(Math.min(Math.max(progress, 0), 1) * 100)}%`;
    if (text) $('#loading-text').textContent = text;
  });
  Source.on('loading.end', ({ error, reason, extended }) => {
    $('#loading-bar').style.width = '100%';
    // The engine keeps the UI (with the last game frame) up until the world is
    // drawn and then hides it; take the loading screen down then (fallback
    // timer for loads that end in the menu).
    clearTimeout(loadingHideTimer);
    if (error) endLoadingScreen();
    else loadingHideTimer = setTimeout(endLoadingScreen, 1500);
    if (error && (reason || extended)) {
      UI.modal({ title: L('#GameUI_Disconnected', 'Disconnected'), text: [reason, extended].filter(Boolean).join('\n\n') });
    }
  });

  // ===========================================================================
  // Interface / HUD customization (js/prefs.js)
  // ===========================================================================
  function applyUiPrefs() {
    const root = document.documentElement;
    root.style.setProperty('--accent', Prefs.get('ui.accent'));
    root.style.setProperty('--ui-scale', Prefs.get('ui.scale'));
    root.style.setProperty('--dim', Prefs.get('ui.dim'));
    root.dataset.font = Prefs.get('ui.font');
    root.classList.toggle('no-anim', !Prefs.get('ui.animations'));
    root.classList.toggle('no-quick', !Prefs.get('ui.quick'));
    applyBlurPrefs();
  }

  // Blur intensity -> engine glass settings (resolution levels + Gaussian
  // passes) and the CSS blur used when no map is running.
  const BLUR_LEVELS = {
    off: { glass: 0, levels: 3, passes: 1, css: 0 },
    low: { glass: 1, levels: 2, passes: 1, css: 8 },
    medium: { glass: 1, levels: 3, passes: 1, css: 14 },
    high: { glass: 1, levels: 3, passes: 2, css: 22 },
    max: { glass: 1, levels: 4, passes: 2, css: 32 },
  };
  let appliedBlur = null;
  function applyBlurPrefs() {
    const b = BLUR_LEVELS[Prefs.get('ui.blur')] || BLUR_LEVELS.high;
    const root = document.documentElement;
    root.style.setProperty('--blur', `${b.css}px`);
    root.classList.toggle('no-bgblur', !Prefs.get('ui.bgBlur') || !b.glass);
    const key = `${b.glass}/${b.levels}/${b.passes}`;
    if (key === appliedBlur) return;
    appliedBlur = key;
    Source.cmd(`htmlui_glass ${b.glass}; htmlui_glass_levels ${b.levels}; htmlui_glass_passes ${b.passes}`).catch(() => {});
  }

  function showPref(el) {
    const v = Prefs.get(el.dataset.pref);
    if (el.type === 'checkbox') el.checked = !!v;
    else el.value = String(v);
    if (el.type === 'range') el.dispatchEvent(new Event('input')); // refresh fill + output
  }

  function bindPrefs() {
    for (const sel of $$('select[data-accents]')) {
      sel.replaceChildren(...Prefs.ACCENTS.map((a) => h('option', { value: a.value, 'data-swatch': a.value, text: a.label })));
    }
    for (const el of $$('[data-pref]')) {
      showPref(el);
      const key = el.dataset.pref;
      el.addEventListener(el.type === 'range' ? 'input' : 'change', (e) => {
        if (!e.isTrusted && el.type === 'range') return; // our own refresh above
        const v = el.type === 'checkbox' ? el.checked : el.type === 'range' ? Number(el.value) : el.value;
        if (el.type === 'checkbox') Source.sound(el.checked ? 'toggle_on' : 'toggle_off');
        Prefs.set(key, v);
      });
    }
    for (const b of $$('[data-pref-reset]')) {
      b.addEventListener('click', () => {
        Prefs.reset(b.dataset.prefReset);
        for (const el of $$(`[data-pref^="${b.dataset.prefReset}"]`)) showPref(el);
        UI.syncControls();
      });
    }
    Prefs.onChange(() => { applyUiPrefs(); for (const el of $$('[data-pref]')) showPref(el); UI.syncControls(); });
    applyUiPrefs();
    UI.syncControls();
  }
  // Before anything else binds the settings controls (and before first paint).
  bindPrefs();

  // ===========================================================================
  // Engine events
  // ===========================================================================
  // Movement mode can change through binds while the menu is closed.
  const refreshMovement = () => UI.refreshCvars($('#quick')).catch(() => {});

  Source.on('activated', (st) => {
    refreshMovement();
    applyState(st);
    document.body.classList.add('ui-visible');
    if (!loading) $('#main-menu .menu__item:not([hidden])')?.focus({ preventScroll: true });
  });
  Source.on('hidden', () => {
    endLoadingScreen();
    document.body.classList.remove('ui-visible');
    UI.closeAll();
  });
  Source.on('state', applyState);
  Source.on('newgame.show', ({ chapter }) => UI.open('newgame', chapter));
  Source.on('quit.confirm', confirmQuit);
  Source.on('menu.command', ({ command }) => {
    const fn = menuCommands[command];
    if (fn) fn();
    else console.warn('[app] unhandled menu command', command);
  });
  Source.on('disconnected', ({ reason }) => {
    if (reason) UI.modal({ title: L('#GameUI_Disconnected', 'Disconnected'), text: reason });
  });

  // ===========================================================================
  // Boot
  // ===========================================================================
  async function boot() {
    try {
      info = await Source.call('game.info');
      await Source.loadStrings(['GameUI_', 'Valve_', `${info.dir}_`]).catch((e) => console.warn(e.message));
    } catch (e) {
      console.error('[app] boot failed', e);
    }

    $('#game-subtitle').textContent = 'Half-Life 2 content';
    document.title = 'SourceSpeedrunMod';
    Source.trackGlass('ui.glass', '#bgblur, .glass, .layer:not(.layer--console), .modal, .console, .toast, .loading__card',
      // Without a map the engine does not redraw the frame; the page draws its own
      // background and CSS backdrop-filter blurs it instead.
      () => !document.body.classList.contains('no-map'));

    {
      let v = 0.8;
      try { const s = localStorage.getItem('htmlui.uiVolume'); if (s != null) v = Number(s); } catch {}
      UISound.setUiVolume(v);
      const uiVol = $('#uivol');
      uiVol.value = v;
      uiVol.addEventListener('input', () => {
        UISound.setUiVolume(uiVol.value);
        try { localStorage.setItem('htmlui.uiVolume', uiVol.value); } catch {}
      });
      uiVol.addEventListener('change', () => Source.sound('click'));
    }
    UISound.init().catch((e) => console.warn(e.message));
    Source.cvar.value('volume').then((v) => UISound.setGameVolume(v ?? 1)).catch(() => {});
    document.addEventListener('cvarchange', (e) => { if (e.detail.name === 'volume') UISound.setGameVolume(e.detail.value); });
    UI.bind(document);

    const st = await Source.call('ui.ready');
    applyState(st);
    refreshMovement();
    document.body.classList.add('ui-visible');
    document.body.classList.remove('is-booting');
  }

  boot();
})();
