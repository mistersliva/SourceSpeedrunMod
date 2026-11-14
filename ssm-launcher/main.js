// SourceSpeedrunMod launcher — main process.
'use strict';

const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const steamLocal = require('./src/steam-local');
const steamAuth = require('./src/steam-auth');
const gameinfo = require('./src/gameinfo');
const { Updater } = require('./src/updater');

const GAME_EXE = 'hl2_win64.exe';

// ------------------------------------------------------------------ Settings
const settingsFile = () => path.join(app.getPath('userData'), 'settings.json');

// Optional defaults shipped next to the exe (e.g. the update server URL).
function bundledConfig() {
  for (const dir of [process.env.PORTABLE_EXECUTABLE_DIR, path.dirname(process.execPath), __dirname].filter(Boolean)) {
    try { return JSON.parse(fs.readFileSync(path.join(dir, 'launcher-config.json'), 'utf8')); } catch {}
  }
  return {};
}

// ownershipEndpoint / steamApiKey come from launcher-config.json (see README).
const DEFAULTS = { gameDir: '', launchOptions: '-novid', windowed: false, closeOnPlay: true, updateUrl: '', ownershipEndpoint: '', steamApiKey: '', account: null };
let settings = { ...DEFAULTS };
function loadSettings() {
  let saved = {};
  try { saved = JSON.parse(fs.readFileSync(settingsFile(), 'utf8')); } catch {}
  const bundled = bundledConfig();
  settings = { ...DEFAULTS, ...bundled, ...saved };
  // Server-side settings always come from the shipped config, never from the
  // per-user file.
  settings.ownershipEndpoint = bundled.ownershipEndpoint || '';
  settings.steamApiKey = bundled.steamApiKey || '';
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
    process.env.PORTABLE_EXECUTABLE_DIR,           // portable exe placed in the game folder
    path.dirname(process.execPath),
    path.join(process.env.PORTABLE_EXECUTABLE_DIR || path.dirname(process.execPath), 'game'),
    path.resolve(__dirname, '..', 'game'),          // development checkout
  ];
  return candidates.find(isGameDir) || '';
}

function modVersion(gameDir) {
  try { return fs.readFileSync(path.join(gameDir, 'ssm-version.txt'), 'utf8').trim(); } catch { return ''; }
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
  return {
    version: app.getVersion(),
    settings: { ...publicSettings, hasOwnershipCheck: !!(settings.ownershipEndpoint || steamApiKey) },
    gameDir,
    modVersion: gameDir ? modVersion(gameDir) : '',
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
  const allowed = ['gameDir', 'launchOptions', 'windowed', 'closeOnPlay', 'updateUrl'];
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

let updatePlan = null;
handle('update:check', async () => {
  const gameDir = findGameDir();
  if (!gameDir) throw new Error('Game folder not found.');
  if (!settings.updateUrl) throw new Error('No update server set.');
  const u = new Updater(gameDir, path.join(app.getPath('userData'), 'hash-cache.json'));
  updatePlan = await u.check(settings.updateUrl, (p) => send('update:progress', p));
  return { version: updatePlan.version, files: updatePlan.toUpdate.length, bytes: updatePlan.bytes, current: modVersion(gameDir) };
});
handle('update:apply', async () => {
  const gameDir = findGameDir();
  if (!updatePlan) throw new Error('Check for updates first.');
  if (gameProcess) throw new Error('Close the game before updating.');
  const u = new Updater(gameDir, path.join(app.getPath('userData'), 'hash-cache.json'));
  await u.apply(settings.updateUrl, updatePlan, (p) => send('update:progress', p));
  if (updatePlan.version) fs.writeFileSync(path.join(gameDir, 'ssm-version.txt'), updatePlan.version);
  updatePlan = null;
  return true;
});
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
  });
  app.on('window-all-closed', () => app.quit());
}
