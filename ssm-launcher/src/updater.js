// Mod file updates from a static web host (GitHub Releases / Pages, any CDN,
// an S3 bucket...). The server needs two things next to each other:
//
//   manifest.json   { "version": "1.2.0", "files": [ { "path": "bin/x64/engine.dll", "size": 123, "sha256": "..." } ] }
//   files/<path>    every file listed in the manifest
//
// tools/make-manifest.js builds both from a game folder. The launcher hashes
// the local files (cached by size + mtime), downloads only what differs into
// a staging folder, verifies the hash, then moves the files into place.
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// Never touched by updates: the player's own data.
const KEEP = [/^hl2\/save\//i, /^hl2\/cfg\/config\.cfg$/i, /^hl2\/splits\//i, /^hl2\/custom\//i, /^hl2\/console\.log$/i, /^\.update\//i];

function sha256File(file) {
  return new Promise((resolve, reject) => {
    const h = crypto.createHash('sha256');
    fs.createReadStream(file).on('data', (d) => h.update(d)).on('end', () => resolve(h.digest('hex'))).on('error', reject);
  });
}

function safeRel(rel) {
  const n = rel.replace(/\\/g, '/');
  if (!n || n.startsWith('/') || /^[A-Za-z]:/.test(n) || n.split('/').includes('..')) throw new Error(`bad path in manifest: ${rel}`);
  return n;
}

class Updater {
  constructor(gameDir, cacheFile) {
    this.gameDir = gameDir;
    this.cacheFile = cacheFile;
    try { this.cache = JSON.parse(fs.readFileSync(cacheFile, 'utf8')); } catch { this.cache = {}; }
  }

  async localHash(rel) {
    const file = path.join(this.gameDir, rel);
    let st;
    try { st = fs.statSync(file); } catch { return null; }
    const key = `${st.size}:${st.mtimeMs}`;
    const c = this.cache[rel];
    if (c && c.key === key) return c.sha256;
    const sha256 = await sha256File(file);
    this.cache[rel] = { key, sha256 };
    return sha256;
  }

  saveCache() {
    try { fs.writeFileSync(this.cacheFile, JSON.stringify(this.cache)); } catch {}
  }

  async fetchManifest(url) {
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error(`manifest: HTTP ${res.status}`);
    const m = await res.json();
    if (!m || !Array.isArray(m.files)) throw new Error('manifest: no file list');
    return m;
  }

  // -> { version, toUpdate: [{ path, size, sha256 }], bytes }
  async check(manifestUrl, onProgress = () => {}) {
    const manifest = await this.fetchManifest(manifestUrl);
    const toUpdate = [];
    let i = 0;
    for (const f of manifest.files) {
      const rel = safeRel(f.path);
      onProgress({ phase: 'checking', done: ++i, total: manifest.files.length, file: rel });
      if (KEEP.some((re) => re.test(rel))) continue;
      if ((await this.localHash(rel)) !== String(f.sha256).toLowerCase()) toUpdate.push({ ...f, path: rel });
    }
    this.saveCache();
    return { version: manifest.version || '', toUpdate, bytes: toUpdate.reduce((a, f) => a + (f.size || 0), 0), manifest };
  }

  async apply(manifestUrl, plan, onProgress = () => {}) {
    const base = new URL('files/', manifestUrl);
    const stage = path.join(this.gameDir, '.update');
    fs.rmSync(stage, { recursive: true, force: true });
    let doneBytes = 0;
    // 1. Download + verify everything first, so a failed download changes nothing.
    for (const f of plan.toUpdate) {
      const url = new URL(f.path.split('/').map(encodeURIComponent).join('/'), base);
      const res = await fetch(url);
      if (!res.ok || !res.body) throw new Error(`${f.path}: HTTP ${res.status}`);
      const dest = path.join(stage, f.path);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      const h = crypto.createHash('sha256');
      const out = fs.createWriteStream(dest);
      for await (const chunk of res.body) {
        h.update(chunk);
        doneBytes += chunk.length;
        if (!out.write(chunk)) await new Promise((r) => out.once('drain', r));
        onProgress({ phase: 'downloading', file: f.path, doneBytes, totalBytes: plan.bytes });
      }
      await new Promise((r, j) => out.end((e) => (e ? j(e) : r())));
      if (h.digest('hex') !== String(f.sha256).toLowerCase()) throw new Error(`${f.path}: checksum mismatch`);
    }
    // 2. Move into place (fails if the game is running and holds a DLL).
    for (const f of plan.toUpdate) {
      const dest = path.join(this.gameDir, f.path);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.renameSync(path.join(stage, f.path), dest);
      delete this.cache[f.path];
      onProgress({ phase: 'installing', file: f.path });
    }
    fs.rmSync(stage, { recursive: true, force: true });
    this.saveCache();
  }
}

module.exports = { Updater, sha256File, KEEP };
