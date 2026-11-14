// Launcher page.
'use strict';

const $ = (id) => document.getElementById(id);
const L = window.launcher;
let st = null;
let progress = null; // last install:progress event while installing

function toast(text, kind = '') {
  const t = document.createElement('div');
  t.className = `toast${kind ? ` toast--${kind}` : ''}`;
  t.textContent = text;
  $('toasts').append(t);
  setTimeout(() => t.remove(), 4200);
}

function setValue(id, text, cls = '') {
  const el = $(id);
  el.textContent = text;
  el.className = `row__value${el.classList.contains('row__value--path') ? ' row__value--path' : ''}${cls ? ` ${cls}` : ''}`;
}

const gb = (bytes) => `${(bytes / 1073741824).toFixed(1)} GB`;
const mb = (bytes) => `${Math.round(bytes / 1048576)} MB`;
function duration(s) {
  if (!Number.isFinite(s) || s <= 0) return '';
  s = Math.ceil(s);
  return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, '0')} s`;
}

// ---------------------------------------------------------------- Pages
function showPage(name) {
  for (const b of document.querySelectorAll('.menu__item')) b.classList.toggle('is-current', b.dataset.page === name);
  for (const p of document.querySelectorAll('.page')) {
    const on = p.dataset.page === name;
    p.hidden = !on;
    p.classList.remove('is-open');
    if (on) { void p.offsetWidth; p.classList.add('is-open'); }
  }
}
for (const b of document.querySelectorAll('.menu__item')) b.addEventListener('click', () => showPage(b.dataset.page));

// ---------------------------------------------------------------- Main button
// install | update | play, depending on what is on disk and on GitHub.
function mainAction(s) {
  if (s.installing) return 'busy';
  if (!s.gameDir) return 'install';
  if (s.updateAvailable) return 'update';
  return 'play';
}

function renderPlaybox(s) {
  const hl2 = s.steam?.hl2;
  const a = s.account;
  const action = mainAction(s);
  const btn = $('play');
  const status = $('play-status');
  let msg = '';
  let cls = '';
  let enabled = true;

  if (action === 'busy') {
    btn.textContent = 'Installing';
    enabled = false;
    msg = '';
  } else if (action === 'install') {
    btn.textContent = 'Install';
    if (s.release) msg = `${s.release.name} · ${mb(s.release.size)} download`;
    else if (s.releaseError) { msg = `Can't reach the download: ${s.releaseError}`; cls = 'is-bad'; enabled = false; }
    else { msg = 'Checking for the latest version…'; enabled = false; }
  } else if (action === 'update') {
    btn.textContent = 'Update';
    msg = `${s.release.name} is available (you have ${s.modVersion}) — ${mb(s.release.size)}`;
    cls = 'is-warn';
  } else {
    btn.textContent = 'Play';
    msg = 'Ready';
    if (s.running) { msg = 'Game is running'; enabled = false; }
    else if (!hl2 || !hl2.exists) { msg = 'Half-Life 2 is not installed in Steam'; cls = 'is-bad'; enabled = false; }
    else if (a?.owned === false) { msg = 'This Steam account does not own Half-Life 2'; cls = 'is-bad'; enabled = false; }
    else if (!hl2.fullyInstalled) { msg = 'Half-Life 2 is updating in Steam'; cls = 'is-warn'; }
    else if (!a) msg = 'Ready — sign in through Steam to verify ownership';
  }
  status.textContent = msg;
  status.className = `playbox__status ${cls}`;
  btn.disabled = !enabled;
  btn.dataset.action = action;
  $('play-anyway').hidden = action !== 'update' || s.running;
  $('dl').hidden = action !== 'busy';
}

function renderProgress() {
  const p = progress;
  if (!p) return;
  const fill = $('dl-fill');
  fill.classList.remove('is-indeterminate');
  if (p.phase === 'download') {
    fill.style.width = `${p.total ? (p.done / p.total) * 100 : 0}%`;
    const speed = p.speed ? ` · ${(p.speed / 1048576).toFixed(1)} MB/s` : '';
    const eta = duration(p.eta);
    $('dl-text').textContent = `Downloading ${mb(p.done)} / ${mb(p.total)}${speed}${eta ? ` · ${eta} left` : ''}`;
    $('dl-cancel').hidden = false;
  } else if (p.phase === 'extract') {
    fill.style.width = `${p.total ? (p.done / p.total) * 100 : 0}%`;
    $('dl-text').textContent = `Installing files ${p.done} / ${p.total}`;
    $('dl-cancel').hidden = false;
  } else if (p.phase === 'start') {
    fill.classList.add('is-indeterminate');
    $('dl-text').textContent = 'Starting download…';
  }
}

// ---------------------------------------------------------------- Render
function ownedText(a) {
  if (!a) return ['Sign in through Steam to check', 'is-dim'];
  if (a.owned === true) return ['Yes', 'is-ok'];
  if (a.owned === false) return ['No — not on this account', 'is-bad'];
  return ['Unknown', 'is-warn'];
}

function render(s) {
  st = s;
  const hl2 = s.steam?.hl2;
  const a = s.account;

  // Home
  setValue('home-account', a ? a.name : 'Not signed in', a ? '' : 'is-dim');
  setValue('home-owned', ...ownedText(a));
  if (!s.steam?.steamInstalled) setValue('home-installed', 'Steam is not installed', 'is-bad');
  else if (!hl2) setValue('home-installed', 'No', 'is-bad');
  else if (!hl2.exists) setValue('home-installed', 'Listed in Steam but the files are missing', 'is-bad');
  else if (!hl2.fullyInstalled) setValue('home-installed', 'Downloading / update pending in Steam', 'is-warn');
  else setValue('home-installed', `Yes — ${hl2.library} (${gb(hl2.sizeOnDisk)})`, 'is-ok');
  $('home-installed').title = hl2?.dir || '';
  setValue('home-game', s.gameDir || 'Not installed yet', s.gameDir ? '' : 'is-dim');
  if (!s.gameDir) setValue('home-version', '—', 'is-dim');
  else if (s.updateAvailable) setValue('home-version', `${s.modVersion} — ${s.release.name} available`, 'is-warn');
  else setValue('home-version', s.modVersion || 'Development build', s.modVersion ? 'is-ok' : 'is-dim');
  $('install-hl2').hidden = !(s.steam?.steamInstalled && (!hl2 || !hl2.exists) && a?.owned !== false);
  $('buy-hl2').hidden = a?.owned !== false;

  // Steam page
  $('account-signed-out').hidden = !!a;
  $('account-signed-in').hidden = !a;
  $('steam-signout').hidden = !a;
  $('steam-refresh').hidden = !a;
  if (a) {
    $('account-name').textContent = a.name;
    $('account-id').textContent = `SteamID ${a.steamid}`;
    if (a.avatar) $('account-avatar').src = a.avatar;
    setValue('account-owned', ...ownedText(a));
    setValue('account-source', {
      server: 'Steam Web API (mod server)',
      webapi: 'Steam Web API',
      local: 'Installed in Steam on this PC',
      unknown: s.settings.hasOwnershipCheck ? 'Could not check — profile game details are private' : 'Could not check — sign in to Steam on this PC',
    }[a.ownedSource] || '—', a.ownedSource === 'unknown' ? 'is-warn' : '');
  }
  if (!s.steam?.steamInstalled) setValue('steam-client', 'Not installed', 'is-bad');
  else setValue('steam-client', s.steam.steamRunning ? 'Running' : 'Not running', s.steam.steamRunning ? 'is-ok' : 'is-dim');
  const local = s.steam?.accounts?.find((x) => x.steamid === s.steam.activeSteamId);
  setValue('steam-local-user', local ? local.personaName || local.steamid : s.steam?.steamRunning ? s.steam.activeSteamId : '—');

  // Updates
  setValue('upd-current', s.gameDir ? s.modVersion || 'Development build' : 'Not installed', s.modVersion ? '' : 'is-dim');
  if (s.release) setValue('upd-available', s.release.name, s.updateAvailable ? 'is-warn' : 'is-ok');
  else setValue('upd-available', s.releaseError || 'Checking…', s.releaseError ? 'is-bad' : 'is-dim');
  setValue('upd-size', s.release ? mb(s.release.size) : '—');
  $('upd-notes').textContent = s.release?.notes || '';
  $('upd-apply').hidden = !s.updateAvailable && !!s.gameDir;
  $('upd-apply').textContent = s.gameDir ? 'Update' : 'Install';
  $('upd-reinstall').hidden = !(s.release && s.modVersion && !s.updateAvailable);
  for (const b of [$('upd-apply'), $('upd-reinstall')]) b.disabled = s.installing || !s.release || s.running;

  // Settings
  setValue('set-installdir', s.settings.installDir || '—');
  setValue('set-gamedir', s.gameDir || 'Not installed');
  $('set-installdir-pick').disabled = s.installing;
  if (document.activeElement !== $('set-launch')) $('set-launch').value = s.settings.launchOptions || '';
  for (const c of document.querySelectorAll('.cycler')) renderCycler(c);
  $('launcher-version').textContent = `Launcher ${s.version}`;
  $('open-gamedir').hidden = !s.gameDir;

  renderPlaybox(s);
}

async function refresh() {
  try { render(await L.state()); } catch (e) { toast(e.message, 'error'); }
}

// ---------------------------------------------------------------- Settings
function renderCycler(c) {
  if (!st) return;
  const values = c.dataset.values.split(',');
  const labels = c.dataset.labels.split(',');
  const cur = String(st.settings[c.dataset.setting]);
  if (!c.firstChild) {
    c.innerHTML = '<button aria-label="Previous">‹</button><span></span><button aria-label="Next">›</button>';
    const step = async (d) => {
      const i = (values.indexOf(String(st.settings[c.dataset.setting])) + d + values.length) % values.length;
      const v = values[i] === 'true' ? true : values[i] === 'false' ? false : values[i];
      await L.setSettings({ [c.dataset.setting]: v });
      refresh();
    };
    c.children[0].addEventListener('click', () => step(-1));
    c.children[2].addEventListener('click', () => step(1));
  }
  c.children[1].textContent = labels[Math.max(0, values.indexOf(cur))];
}

$('set-launch').addEventListener('change', async () => { await L.setSettings({ launchOptions: $('set-launch').value.trim() }); refresh(); });
$('set-gamedir-pick').addEventListener('click', async () => {
  try { if (await L.pickGameDir()) refresh(); } catch (e) { toast(e.message, 'error'); }
});
$('set-installdir-pick').addEventListener('click', async () => {
  try { if (await L.pickInstallDir()) refresh(); } catch (e) { toast(e.message, 'error'); }
});
$('open-gamedir').addEventListener('click', () => L.openGameDir());

// ---------------------------------------------------------------- Steam
$('steam-signin').addEventListener('click', async () => {
  $('steam-signin').disabled = true;
  $('steam-signin').textContent = 'Waiting for Steam in your browser…';
  try {
    await L.steamSignIn();
    toast('Signed in through Steam', 'ok');
  } catch (e) {
    if (e.message !== 'cancelled') toast(e.message, 'error');
  }
  $('steam-signin').disabled = false;
  $('steam-signin').textContent = 'Sign in through Steam';
  refresh();
});
$('steam-signout').addEventListener('click', async () => { await L.steamSignOut(); refresh(); });
$('steam-refresh').addEventListener('click', async () => {
  try { await L.steamRefresh(); } catch (e) { toast(e.message, 'error'); }
  refresh();
});
$('install-hl2').addEventListener('click', () => L.steamInstallHl2());
$('buy-hl2').addEventListener('click', () => L.steamStore());
$('refresh').addEventListener('click', refresh);

// ---------------------------------------------------------------- Install / update
L.on('install:progress', (p) => { progress = p; renderProgress(); });

async function installMod() {
  progress = { phase: 'start' };
  $('dl').hidden = false;
  renderProgress();
  try {
    const r = await L.installMod();
    toast(`SourceSpeedrunMod ${r.version} installed`, 'ok');
  } catch (e) {
    if (e.message === 'cancelled') toast('Download cancelled');
    else toast(e.message, 'error');
  }
  progress = null;
  refresh();
}
$('dl-cancel').addEventListener('click', () => L.cancelInstall());
$('upd-apply').addEventListener('click', installMod);
$('upd-reinstall').addEventListener('click', installMod);
$('upd-check').addEventListener('click', async () => {
  try { render(await L.checkMod()); } catch (e) { toast(e.message, 'error'); }
});

// ---------------------------------------------------------------- Play / window
async function play() {
  $('play').disabled = true;
  try { await L.play(); } catch (e) { toast(e.message, 'error'); }
  refresh();
}
$('play').addEventListener('click', () => {
  const action = $('play').dataset.action;
  if (action === 'install' || action === 'update') installMod();
  else if (action === 'play') play();
});
$('play-anyway').addEventListener('click', play);
$('win-min').addEventListener('click', () => L.minimize());
$('win-close').addEventListener('click', () => L.close());
L.on('state-changed', refresh);
L.on('toast', (t) => toast(t.text, t.kind));

refresh();
// Steam can finish installing / be started while the launcher is open.
setInterval(() => { if (!document.hidden && !st?.installing) refresh(); }, 5000);
