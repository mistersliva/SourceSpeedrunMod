// Installs / updates the mod from the latest GitHub release.
//
// The release carries one zip (SourceSpeedrunMod-<version>.zip) with a single
// top-level "SourceSpeedrunMod/" folder. The launcher downloads it with
// progress, then extracts it over the install folder with Windows' own tar
// (bsdtar, reads zip). The zip never contains player data (saves, configs,
// splits), so an update leaves those alone.
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn, execFile } = require('child_process');

const DEFAULT_REPO = 'mistersliva/SourceSpeedrunMod';
const ASSET_RE = /^SourceSpeedrunMod-.*\.zip$/i;
const VERSION_FILE = 'ssm-version.txt';
// Windows' bsdtar reads zip; a GNU tar earlier in PATH (Git for Windows) does not.
const TAR = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe');

function installedVersion(dir) {
  try { return fs.readFileSync(path.join(dir, VERSION_FILE), 'utf8').trim(); } catch { return ''; }
}

// Compare "v1.2.3" style tags; unknown formats compare as strings.
function newer(a, b) {
  const pa = String(a).replace(/^v/i, '').split('.').map(Number);
  const pb = String(b).replace(/^v/i, '').split('.').map(Number);
  if (pa.some(Number.isNaN) || pb.some(Number.isNaN)) return String(a) !== String(b);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  }
  return false;
}

async function latestRelease(repo = DEFAULT_REPO) {
  const res = await fetch(`https://api.github.com/repos/${repo}/releases/latest`, {
    headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'SourceSpeedrunMod-Launcher' },
  });
  if (res.status === 404) throw new Error('No release has been published yet.');
  if (!res.ok) throw new Error(`GitHub: HTTP ${res.status}`);
  const r = await res.json();
  const asset = (r.assets || []).find((a) => ASSET_RE.test(a.name));
  if (!asset) throw new Error(`Release ${r.tag_name} has no mod download.`);
  return {
    tag: r.tag_name,
    name: r.name || r.tag_name,
    notes: r.body || '',
    published: r.published_at,
    url: r.html_url,
    asset: { name: asset.name, size: asset.size, url: asset.browser_download_url },
  };
}

function isGameRunning(dir) {
  return new Promise((resolve) => {
    execFile('powershell.exe', ['-NoProfile', '-Command', '(Get-Process hl2_win64 -ErrorAction SilentlyContinue).Path'], { windowsHide: true }, (err, out) => {
      if (err) return resolve(false);
      resolve(String(out).toLowerCase().includes(path.join(dir, 'hl2_win64.exe').toLowerCase()));
    });
  });
}

function freeBytes(dir) {
  try {
    const s = fs.statfsSync(dir);
    return s.bavail * s.bsize;
  } catch {
    return Infinity;
  }
}

// One install at a time; cancel() aborts the download or stops tar.
let current = null;

function checkWritable(dir) {
  try {
    fs.mkdirSync(dir, { recursive: true });
    const probe = path.join(dir, '.write-test');
    fs.writeFileSync(probe, '');
    fs.rmSync(probe);
  } catch {
    throw new Error(`No permission to write to ${dir}. Choose another install folder in Settings.`);
  }
}

// Downloads `asset` ({ name, size, url }) into <dir>\.download and extracts it
// with tar into `dest`. `tarArgs` are extra arguments (strip, excludes,
// member patterns). Progress: download, then extract.
async function installArchive({ asset, dir, dest, tarArgs = [], onProgress, spaceFactor = 2.2 }) {
  if (current) throw new Error('An install is already running.');
  const abort = new AbortController();
  let tar = null;
  current = { cancel: () => { abort.abort(); if (tar) tar.kill(); } };
  const tmpDir = path.join(dir, '.download');
  try {
    checkWritable(dir);
    if (await isGameRunning(dir)) throw new Error('Close the game first.');
    const need = asset.size * spaceFactor;
    if (freeBytes(dir) < need) throw new Error(`Not enough disk space: ${(need / 1073741824).toFixed(1)} GB needed.`);

    // 1. Download.
    fs.mkdirSync(tmpDir, { recursive: true });
    const zip = path.join(tmpDir, asset.name);
    const res = await fetch(asset.url, { signal: abort.signal, headers: { 'User-Agent': 'SourceSpeedrunMod-Launcher' } });
    if (!res.ok || !res.body) throw new Error(`Download failed: HTTP ${res.status}`);
    const total = Number(res.headers.get('content-length')) || asset.size;
    const out = fs.createWriteStream(zip);
    let done = 0;
    const started = Date.now();
    let lastEmit = 0;
    try {
      for await (const chunk of res.body) {
        done += chunk.length;
        if (!out.write(chunk)) await new Promise((r) => out.once('drain', r));
        const now = Date.now();
        if (now - lastEmit > 120) {
          lastEmit = now;
          const speed = done / Math.max(0.001, (now - started) / 1000);
          onProgress({ phase: 'download', done, total, speed, eta: speed > 0 ? (total - done) / speed : 0 });
        }
      }
    } finally {
      await new Promise((r) => out.end(r));
    }
    if (done !== total) throw new Error('Download incomplete.');
    onProgress({ phase: 'download', done, total, speed: 0, eta: 0 });

    // 2. Extract (files are counted first so extraction has a real progress bar).
    const files = await new Promise((resolve, reject) => {
      execFile(TAR, ['-tf', zip], { windowsHide: true, maxBuffer: 64 * 1024 * 1024 }, (err, stdout) => {
        if (err) return reject(new Error('The download is damaged.'));
        resolve(stdout.split(/\r?\n/).filter((l) => l && !l.endsWith('/')).length);
      });
    });
    fs.mkdirSync(dest, { recursive: true });
    await new Promise((resolve, reject) => {
      tar = spawn(TAR, ['-xvf', zip, '-C', dest, ...tarArgs], { windowsHide: true });
      let n = 0;
      let buf = '';
      let lastErr = '';
      tar.stderr.on('data', (d) => {
        buf += d.toString();
        const lines = buf.split(/\r?\n/);
        buf = lines.pop();
        for (const l of lines) {
          if (/^x /.test(l)) { if (!l.endsWith('/')) n++; } else if (l.trim()) lastErr = l.trim();
        }
        onProgress({ phase: 'extract', done: Math.min(n, files), total: files });
      });
      tar.on('error', reject);
      tar.on('close', (code) => {
        if (abort.signal.aborted) return reject(new Error('cancelled'));
        if (code !== 0) return reject(new Error(`Extracting failed${lastErr ? `: ${lastErr}` : ''}`));
        resolve();
      });
    });
    fs.rmSync(tmpDir, { recursive: true, force: true });
  } catch (e) {
    // Drop the partial download.
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch {}
    if (abort.signal.aborted) throw new Error('cancelled');
    throw e;
  } finally {
    current = null;
  }
}

async function install({ release, dir, onProgress }) {
  // A half-extracted update is repaired by installing again (the version file
  // is only written on success).
  await installArchive({
    asset: release.asset,
    dir,
    dest: dir,
    tarArgs: ['--strip-components', '1', '--exclude', '*/SSM-Launcher.exe'],
    onProgress,
  });
  fs.writeFileSync(path.join(dir, VERSION_FILE), release.tag);
  onProgress({ phase: 'done' });
}

function cancel() {
  if (current) current.cancel();
}

module.exports = { latestRelease, install, installArchive, cancel, installedVersion, newer, isGameRunning, DEFAULT_REPO };
