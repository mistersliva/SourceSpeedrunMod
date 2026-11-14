// Launcher page.
'use strict';

const $ = (id) => document.getElementById(id);
const L = window.launcher;
let st = null;

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
  setValue('home-game', s.gameDir || 'Not found — set it in Settings', s.gameDir ? '' : 'is-bad');
  setValue('home-version', s.modVersion || 'Development build', s.modVersion ? '' : 'is-dim');
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
  setValue('upd-current', s.modVersion || 'Development build', s.modVersion ? '' : 'is-dim');
  setValue('upd-server', s.settings.updateUrl || 'Not set (Settings)', s.settings.updateUrl ? '' : 'is-dim');
  $('upd-check').disabled = !s.settings.updateUrl || !s.gameDir;

  // Settings
  setValue('set-gamedir', s.gameDir || 'Not found');
  if (document.activeElement !== $('set-launch')) $('set-launch').value = s.settings.launchOptions || '';
  if (document.activeElement !== $('set-update')) $('set-update').value = s.settings.updateUrl || '';
  for (const c of document.querySelectorAll('.cycler')) renderCycler(c);
  $('launcher-version').textContent = `Launcher ${s.version}`;

  // Play
  const status = $('play-status');
  let msg = 'Ready';
  let cls = '';
  let canPlay = !!s.gameDir && !s.running;
  if (s.running) { msg = 'Game is running'; cls = ''; }
  else if (!s.gameDir) { msg = 'Mod folder not found — see Settings'; cls = 'is-bad'; }
  else if (!hl2 || !hl2.exists) { msg = 'Half-Life 2 is not installed in Steam'; cls = 'is-bad'; canPlay = false; }
  else if (a?.owned === false) { msg = 'This Steam account does not own Half-Life 2'; cls = 'is-bad'; canPlay = false; }
  else if (!hl2.fullyInstalled) { msg = 'Half-Life 2 is updating in Steam'; cls = 'is-warn'; }
  else if (!a) { msg = 'Ready — sign in through Steam to verify ownership'; }
  status.textContent = msg;
  status.className = `playbox__status ${cls}`;
  $('play').disabled = !canPlay;
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

function bindInput(id, key) {
  const el = $(id);
  el.addEventListener('change', async () => { await L.setSettings({ [key]: el.value.trim() }); refresh(); });
}
bindInput('set-launch', 'launchOptions');
bindInput('set-update', 'updateUrl');

$('set-gamedir-pick').addEventListener('click', async () => {
  try { if (await L.pickGameDir()) refresh(); } catch (e) { toast(e.message, 'error'); }
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

// ---------------------------------------------------------------- Updates
L.on('update:progress', (p) => {
  $('upd-progress').hidden = false;
  if (p.phase === 'checking') {
    $('upd-bar').style.width = `${Math.round((p.done / p.total) * 100)}%`;
    $('upd-text').textContent = `Checking ${p.file}`;
  } else if (p.phase === 'downloading') {
    $('upd-bar').style.width = `${p.totalBytes ? Math.round((p.doneBytes / p.totalBytes) * 100) : 0}%`;
    $('upd-text').textContent = `Downloading ${p.file} — ${(p.doneBytes / 1048576).toFixed(1)} / ${(p.totalBytes / 1048576).toFixed(1)} MB`;
  } else if (p.phase === 'installing') {
    $('upd-text').textContent = `Installing ${p.file}`;
  }
});
$('upd-check').addEventListener('click', async () => {
  $('upd-check').disabled = true;
  $('upd-apply').hidden = true;
  try {
    const r = await L.checkUpdates();
    $('upd-progress').hidden = true;
    if (r.files) {
      setValue('upd-available', `${r.version || 'New files'} — ${r.files} file(s), ${(r.bytes / 1048576).toFixed(1)} MB`, 'is-warn');
      $('upd-text').textContent = '';
      $('upd-apply').hidden = false;
    } else {
      setValue('upd-available', `Up to date${r.version ? ` (${r.version})` : ''}`, 'is-ok');
      $('upd-text').textContent = '';
    }
  } catch (e) {
    $('upd-progress').hidden = true;
    $('upd-text').textContent = '';
    toast(e.message, 'error');
  }
  $('upd-check').disabled = false;
});
$('upd-apply').addEventListener('click', async () => {
  $('upd-apply').disabled = true;
  $('upd-check').disabled = true;
  try {
    await L.applyUpdates();
    toast('Update installed', 'ok');
    $('upd-apply').hidden = true;
    setValue('upd-available', 'Up to date', 'is-ok');
  } catch (e) {
    toast(e.message, 'error');
  }
  $('upd-progress').hidden = true;
  $('upd-text').textContent = '';
  $('upd-apply').disabled = false;
  $('upd-check').disabled = false;
  refresh();
});

// ---------------------------------------------------------------- Play / window
$('play').addEventListener('click', async () => {
  $('play').disabled = true;
  try { await L.play(); } catch (e) { toast(e.message, 'error'); }
  refresh();
});
$('win-min').addEventListener('click', () => L.minimize());
$('win-close').addEventListener('click', () => L.close());
L.on('state-changed', refresh);
L.on('toast', (t) => toast(t.text, t.kind));

refresh();
// Steam can finish installing / be started while the launcher is open.
setInterval(() => { if (!document.hidden) refresh(); }, 5000);
