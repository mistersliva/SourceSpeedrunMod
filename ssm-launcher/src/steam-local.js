// Local Steam installation: where Steam is, which accounts are signed in on
// this PC, and whether Half-Life 2 (app 220) is installed. Read-only.
'use strict';

const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');

const HL2_APPID = 220;

// --------------------------------------------------------------------- VDF
// Valve KeyValues text format: "key" "value" / "key" { ... }.
function parseVdf(text) {
  const root = {};
  const stack = [root];
  const re = /"((?:[^"\\]|\\.)*)"|([{}])|\/\/[^\n]*/g;
  let key = null;
  let m;
  while ((m = re.exec(text))) {
    if (m[0].startsWith('//')) continue;
    if (m[2] === '{') {
      const obj = {};
      stack[stack.length - 1][key ?? ''] = obj;
      stack.push(obj);
      key = null;
    } else if (m[2] === '}') {
      if (stack.length > 1) stack.pop();
      key = null;
    } else {
      const s = m[1].replace(/\\(.)/g, (_, c) => (c === 'n' ? '\n' : c === 't' ? '\t' : c));
      if (key === null) key = s;
      else { stack[stack.length - 1][key] = s; key = null; }
    }
  }
  return root;
}

// Case-insensitive lookup (Valve files are inconsistent about key case).
function get(obj, k) {
  if (!obj || typeof obj !== 'object') return undefined;
  const hit = Object.keys(obj).find((x) => x.toLowerCase() === k.toLowerCase());
  return hit === undefined ? undefined : obj[hit];
}

function readVdf(file) {
  try { return parseVdf(fs.readFileSync(file, 'utf8')); } catch { return null; }
}

// ---------------------------------------------------------------- Registry
function regQuery(key, value) {
  return new Promise((resolve) => {
    execFile('reg', ['query', key, '/v', value], { windowsHide: true }, (err, stdout) => {
      if (err) return resolve(null);
      const line = stdout.split(/\r?\n/).find((l) => l.trim().startsWith(value));
      if (!line) return resolve(null);
      const parts = line.trim().split(/\s{2,}|\t/);
      resolve(parts.length >= 3 ? parts.slice(2).join(' ').trim() : null);
    });
  });
}

async function steamPath() {
  const fromReg = (await regQuery('HKCU\\Software\\Valve\\Steam', 'SteamPath'))
    || (await regQuery('HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam', 'InstallPath'));
  const candidates = [fromReg, 'C:\\Program Files (x86)\\Steam', 'C:\\Program Files\\Steam'].filter(Boolean);
  for (const c of candidates) {
    const p = path.normalize(c);
    if (fs.existsSync(path.join(p, 'steamapps'))) return p;
  }
  return null;
}

// Account currently signed in to the running Steam client (32-bit id, 0 = none).
async function activeAccountId() {
  const v = await regQuery('HKCU\\Software\\Valve\\Steam\\ActiveProcess', 'ActiveUser');
  const n = v ? parseInt(v, 16) : 0;
  return Number.isFinite(n) ? n : 0;
}

const accountIdToSteam64 = (id) => (BigInt(id) + 76561197960265728n).toString();

function libraries(steam) {
  const vdf = readVdf(path.join(steam, 'steamapps', 'libraryfolders.vdf'));
  const libs = new Map(); // lower-case path -> path (Windows paths are case-insensitive)
  const add = (p) => { const n = path.normalize(p); if (!libs.has(n.toLowerCase())) libs.set(n.toLowerCase(), n); };
  add(steam);
  const folders = get(vdf, 'libraryfolders') || {};
  for (const [k, v] of Object.entries(folders)) {
    if (!/^\d+$/.test(k)) continue;
    const p = typeof v === 'string' ? v : get(v, 'path');
    if (p) add(p);
  }
  return [...libs.values()];
}

// Half-Life 2 install, from the app manifest in any library.
function findApp(libs, appid = HL2_APPID) {
  for (const lib of libs) {
    const manifest = path.join(lib, 'steamapps', `appmanifest_${appid}.acf`);
    const state = get(readVdf(manifest), 'AppState');
    if (!state) continue;
    const installdir = get(state, 'installdir') || 'Half-Life 2';
    const dir = path.join(lib, 'steamapps', 'common', installdir);
    const flags = Number(get(state, 'StateFlags') || 0);
    return {
      library: lib,
      dir,
      exists: fs.existsSync(path.join(dir, 'hl2')),
      // StateFlags 4 = fully installed; other bits = update / download pending.
      fullyInstalled: (flags & 4) === 4 && (flags & ~4 & ~0x200) === 0,
      flags,
      buildId: get(state, 'buildid') || '',
      lastOwner: get(state, 'LastOwner') || '',
      sizeOnDisk: Number(get(state, 'SizeOnDisk') || 0),
      name: get(state, 'name') || 'Half-Life 2',
    };
  }
  return null;
}

function loginUsers(steam) {
  const users = get(readVdf(path.join(steam, 'config', 'loginusers.vdf')), 'users') || {};
  return Object.entries(users).map(([steamid, u]) => ({
    steamid,
    accountName: get(u, 'AccountName') || '',
    personaName: get(u, 'PersonaName') || '',
    mostRecent: get(u, 'MostRecent') === '1',
  }));
}

async function detect() {
  const steam = await steamPath();
  if (!steam) return { steamInstalled: false };
  const libs = libraries(steam);
  const active = await activeAccountId();
  return {
    steamInstalled: true,
    steamPath: steam,
    steamRunning: active > 0,
    activeSteamId: active > 0 ? accountIdToSteam64(active) : null,
    accounts: loginUsers(steam),
    libraries: libs,
    hl2: findApp(libs),
  };
}

module.exports = { detect, parseVdf, HL2_APPID };
