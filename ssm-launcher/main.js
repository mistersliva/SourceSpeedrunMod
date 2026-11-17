// SourceSpeedrunMod launcher — main process.
'use strict';

const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const steamLocal = require('./src/steam-local');
const steamAuth = require('./src/steam-auth');
const gameinfo = require('./src/gameinfo');
const modInstall = require('./src/mod-install');
const addons = require('./src/addons');

const GAME_EXE = 'hl2_win64.exe';

// ------------------------------------------------------------------ Settings
const settingsFile = () => path.join(app.getPath('userData'), 'settings.json');

// Optional defaults shipped next to the exe (release repository, ownership server).
function bundledConfig() {
  for (const dir of [path.dirname(process.execPath), __dirname].filter(Boolean)) {
    try { return JSON.parse(fs.readFileSync(path.join(dir, 'launcher-config.json'), 'utf8').replace(/^﻿/, '')); } catch {}
  }
  return {};
}

// ownershipEndpoint / steamApiKey come from launcher-config.json (see README).
// gameDir: a mod folder the player picked (e.g. a development build).
// installDir: where the launcher installs the mod from GitHub releases.
// Installed launcher: <launcher folder>\Games\SourceSpeedrunMod (the installer
// makes Games\ writable). Launcher 1.1 installed the mod in %LOCALAPPDATA%.
const legacyInstallDir = () => path.join(process.env.LOCALAPPDATA || app.getPath('userData'), 'SourceSpeedrunMod');
const defaultInstallDir = () => (app.isPackaged ? path.join(path.dirname(process.execPath), 'Games', 'SourceSpeedrunMod') : legacyInstallDir());
const DEFAULTS = { gameDir: '', installDir: '', launchOptions: '-novid', windowed: false, closeOnPlay: true, releaseRepo: modInstall.DEFAULT_REPO, cutscenelessRepo: 'mistersliva/SSM-Cutsceneless', withCutsceneless: false, ownershipEndpoint: '', steamApiKey: '', account: null };
let settings = { ...DEFAULTS };
function loadSettings() {
  let saved = {};
  try { saved = JSON.parse(fs.readFileSync(settingsFile(), 'utf8').replace(/^﻿/, '')); } catch {}
  const bundled = bundledConfig();
  settings = { ...DEFAULTS, ...bundled, ...saved };
  // Server-side settings always come from the shipped config, never from the
  // per-user file.
  settings.ownershipEndpoint = bundled.ownershipEndpoint || '';
  settings.steamApiKey = bundled.steamApiKey || '';
  // Unless the player chose a folder: keep an existing 1.1 install where it is,
  // otherwise use the default.
  if (!saved.installDirCustom) settings.installDir = isGameDir(legacyInstallDir()) ? legacyInstallDir() : defaultInstallDir();
}
function saveSettings() {
  const { ownershipEndpoint, steamApiKey, ...user } = settings;
  fs.mkdirSync(path.dirname(settingsFile()), { recursive: true });
  fs.writeFileSync(settingsFile(), JSON.stringify(user, null, 1));
}

// ---------------------------------------------------------------- Game folder
const isGameDir = (d) => !!d && fs.existsSync(path.join(d, GAME_EXE)) && fs.existsSync(path.join(d, 'hl2', 'gameinfo.txt'));

function findGameDir() {
  const candidates = [
    settings.gameDir,
    settings.installDir,
    path.dirname(process.execPath),                 // launcher placed in the mod folder
    path.resolve(__dirname, '..', 'game'),          // development checkout
  ];
  return candidates.find(isGameDir) || '';
}

const modVersion = (gameDir) => modInstall.installedVersion(gameDir);

// Latest release, cached for a few minutes (GitHub allows 60 requests/hour).
let release = null;
let releaseAt = 0;
let releaseError = '';
async function checkRelease(force = false) {
  if (!force && Date.now() - releaseAt < 5 * 60 * 1000) return release;
  releaseAt = Date.now();
  try {
    release = await modInstall.latestRelease(settings.releaseRepo);
    releaseError = '';
  } catch (e) {
    releaseError = e.message;
  }
  return release;
}

// ------------------------------------------------------------------- State
let win = null;
let gameProcess = null;
let account = null; // { steamid, name, avatar, owned, ownedSource }

async function refreshAccount() {
  if (!settings.account?.steamid) { account = null; return; }
  const local = await steamLocal.detect().catch(() => ({}));
  const steamid = settings.account.steamid;
  const [prof, own] = await Promise.all([
    steamAuth.profile(steamid),
    steamAuth.ownsApp(steamid, steamLocal.HL2_APPID, { config: { ownershipEndpoint: settings.ownershipEndpoint, steamApiKey: settings.steamApiKey }, local }),
  ]);
  account = { steamid, name: prof.name || settings.account.name || steamid, avatar: prof.avatar, privacy: prof.privacy, owned: own.owned, ownedSource: own.source };
  settings.account = { steamid, name: account.name };
  saveSettings();
}

async function state() {
  const steam = await steamLocal.detect().catch(() => ({ steamInstalled: false }));
  const gameDir = findGameDir();
  const mounted = gameDir ? gameinfo.currentHl2Path(gameDir) : null;
  const { steamApiKey, ...publicSettings } = settings;
  const installed = gameDir ? modVersion(gameDir) : '';
  return {
    version: app.getVersion(),
    settings: { ...publicSettings, hasOwnershipCheck: !!(settings.ownershipEndpoint || steamApiKey) },
    gameDir,
    modVersion: installed,
    // Updates are offered for launcher-installed copies (they have a version
    // file), never over a development build.
    release: release && { tag: release.tag, name: release.name, notes: release.notes, url: release.url, size: release.asset.size },
    releaseError,
    updateAvailable: !!(release && installed && modInstall.newer(release.tag, installed)),
    installing,
    addons: await addons.status(gameDir, settings).catch(() => ({})),
    hl2Mounted: mounted,
    steam,
    account,
    running: !!gameProcess,
  };
}

const send = (channel, data) => { if (win && !win.isDestroyed()) win.webContents.send(channel, data); };

// ---------------------------------------------------------------------- Play
function splitArgs(s) {
  const out = [];
  const re = /"([^"]*)"|(\S+)/g;
  let m;
  while ((m = re.exec(s || ''))) out.push(m[1] ?? m[2]);
  return out;
}

async function play() {
  const gameDir = findGameDir();
  if (!gameDir) throw new Error('Game folder not found. Set it in Settings.');
  if (gameProcess) throw new Error('The game is already running.');

  // Mount HL2 content from wherever Steam installed it on this PC.
  const steam = await steamLocal.detect().catch(() => ({}));
  if (steam.hl2?.exists) gameinfo.pointAt(gameDir, steam.hl2.dir);

  const args = ['-game', 'hl2', ...splitArgs(settings.launchOptions)];
  if (settings.windowed && !args.some((a) => /^-(windowed|sw|fullscreen)$/i.test(a))) args.push('-windowed');
  gameProcess = spawn(path.join(gameDir, GAME_EXE), args, { cwd: gameDir, detached: true, stdio: 'ignore' });
  gameProcess.on('exit', () => { gameProcess = null; send('state-changed'); if (win?.isMinimized()) win.restore(); });
  gameProcess.on('error', (e) => { gameProcess = null; send('toast', { kind: 'error', text: e.message }); send('state-changed'); });
  gameProcess.unref();
  if (settings.closeOnPlay) setTimeout(() => app.quit(), 1500);
  else win?.minimize();
  return true;
}

// ------------------------------------------------------------------- Window
function createWindow() {
  win = new BrowserWindow({
    width: 1180,
    height: 680,
    minWidth: 960,
    minHeight: 560,
    frame: false,
    backgroundColor: '#0d0f12',
    title: 'SourceSpeedrunMod',
    icon: path.join(__dirname, 'build', 'icon.png'),
    show: false,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true },
  });
  win.once('ready-to-show', () => win.show());
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  // External links open in the user's browser.
  win.webContents.setWindowOpenHandler(({ url }) => { if (/^https:\/\//.test(url)) shell.openExternal(url); return { action: 'deny' }; });
}

// ---------------------------------------------------------------------- IPC
function handle(channel, fn) {
  ipcMain.handle(channel, async (_e, ...args) => {
    try { return { ok: true, value: await fn(...args) }; } catch (e) { return { ok: false, error: e.message || String(e) }; }
  });
}

handle('state', state);
handle('play', play);
handle('settings:set', (patch) => {
  const allowed = ['gameDir', 'launchOptions', 'windowed', 'closeOnPlay', 'withCutsceneless'];
  for (const k of Object.keys(patch || {})) if (allowed.includes(k)) settings[k] = patch[k];
  saveSettings();
  return settings;
});
handle('game:pickDir', async () => {
  const r = await dialog.showOpenDialog(win, { title: 'SourceSpeedrunMod game folder (contains hl2_win64.exe)', properties: ['openDirectory'] });
  if (r.canceled || !r.filePaths[0]) return null;
  if (!isGameDir(r.filePaths[0])) throw new Error('That folder does not contain hl2_win64.exe and hl2\\gameinfo.txt.');
  settings.gameDir = r.filePaths[0];
  saveSettings();
  return settings.gameDir;
});
handle('game:openDir', () => { const d = findGameDir(); if (d) shell.openPath(d); });
handle('steam:signIn', async () => {
  const steamid = await steamAuth.signIn();
  if (win) { if (win.isMinimized()) win.restore(); win.focus(); }
  settings.account = { steamid };
  saveSettings();
  await refreshAccount();
  return account;
});
handle('steam:signOut', async () => {
  settings.account = null;
  account = null;
  saveSettings();
});
handle('steam:cancel', () => steamAuth.cancelSignIn());
handle('steam:refresh', async () => { await refreshAccount(); return account; });
handle('steam:install', () => shell.openExternal('steam://install/220'));
handle('steam:store', () => shell.openExternal('https://store.steampowered.com/app/220/'));

// Install or update the mod from the latest release.
let installing = false;
handle('mod:check', async () => { await checkRelease(true); return state(); });
handle('mod:install', async () => {
  if (gameProcess) throw new Error('Close the game first.');
  const rel = await checkRelease(true);
  if (!rel) throw new Error(releaseError || 'No release found.');
  // Update in place when the mod was installed by the launcher, otherwise
  // install into the install folder (never over a development build).
  const current = findGameDir();
  const dir = current && modVersion(current) ? current : settings.installDir;
  installing = true;
  send('state-changed');
  try {
    const onProgress = (p) => send('install:progress', { ...p, what: 'mod' });
    await modInstall.install({ release: rel, dir, onProgress });
    // First install with "also download Cutsceneless" ticked.
    let extra = '';
    if (settings.withCutsceneless && !addons.ADDONS.cutsceneless.installed(dir)) {
      try {
        await addons.install('cutsceneless', dir, settings, (p) => send('install:progress', { ...p, what: 'cutsceneless' }));
      } catch (e) {
        extra = e.message === 'cancelled' ? 'Cutsceneless download cancelled' : `Cutsceneless maps: ${e.message}`;
      }
    }
    return { dir, version: rel.tag, extra };
  } finally {
    installing = false;
    send('state-changed');
  }
});
handle('mod:cancel', () => modInstall.cancel());
handle('addon:install', async (id) => {
  if (gameProcess) throw new Error('Close the game first.');
  if (installing) throw new Error('An install is already running.');
  installing = true;
  send('state-changed');
  try {
    return await addons.install(id, findGameDir(), settings, (p) => send('install:progress', { ...p, what: id }));
  } finally {
    installing = false;
    send('state-changed');
  }
});
handle('addon:remove', async (id) => {
  if (gameProcess) throw new Error('Close the game first.');
  await addons.remove(id, findGameDir());
  return state();
});
handle('mod:pickInstallDir', async () => {
  const r = await dialog.showOpenDialog(win, { title: 'Install SourceSpeedrunMod to', properties: ['openDirectory', 'createDirectory'] });
  if (r.canceled || !r.filePaths[0]) return null;
  let dir = r.filePaths[0];
  // Picking a non-empty parent folder installs into a SourceSpeedrunMod subfolder.
  if (!isGameDir(dir) && fs.existsSync(dir) && fs.readdirSync(dir).length) dir = path.join(dir, 'SourceSpeedrunMod');
  settings.installDir = dir;
  settings.installDirCustom = true;
  saveSettings();
  return dir;
});
// ------------------------------------------------------- Launcher self-update
// The installer and latest.yml are attached to each GitHub release; a newer
// launcher downloads in the background and installs on restart (or on quit).
let launcherUpdate = { state: 'idle' }; // idle | downloading | ready | error
function setupAutoUpdate() {
  if (!app.isPackaged) return;
  let autoUpdater;
  try { ({ autoUpdater } = require('electron-updater')); } catch { return; }
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = true;
  const report = (u) => { launcherUpdate = u; send('launcher-update', u); };
  autoUpdater.on('update-available', (i) => report({ state: 'downloading', version: i.version, percent: 0 }));
  autoUpdater.on('download-progress', (p) => report({ ...launcherUpdate, state: 'downloading', percent: Math.round(p.percent) }));
  autoUpdater.on('update-downloaded', (i) => report({ state: 'ready', version: i.version }));
  autoUpdater.on('error', () => { if (launcherUpdate.state === 'downloading') report({ state: 'idle' }); });
  const check = () => autoUpdater.checkForUpdates().catch(() => {});
  check();
  setInterval(check, 2 * 60 * 60 * 1000);
  handle('launcher:restart', () => {
    if (gameProcess) throw new Error('Close the game first.');
    if (installing) throw new Error('Wait for the download to finish.');
    setImmediate(() => autoUpdater.quitAndInstall(true, true));
  });
}
handle('launcher:update', () => launcherUpdate);

handle('win:minimize', () => win.minimize());
handle('win:close', () => win.close());

// ---------------------------------------------------------------------- App
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
  app.whenReady().then(() => {
    loadSettings();
    createWindow();
    refreshAccount().then(() => send('state-changed')).catch(() => {});
    checkRelease().then(() => send('state-changed')).catch(() => {});
    setupAutoUpdate();
  });
  app.on('window-all-closed', () => app.quit());
}
