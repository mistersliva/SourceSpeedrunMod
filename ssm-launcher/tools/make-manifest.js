#!/usr/bin/env node
// Builds an update release from a game folder:
//   node tools/make-manifest.js <gameDir> <outDir> [version]
// Writes <outDir>/manifest.json and copies every file to <outDir>/files/<path>.
// Upload <outDir> anywhere static and put the manifest URL in the launcher
// (Settings → Update server) or in launcher-config.json next to the exe.
'use strict';

const fs = require('fs');
const path = require('path');
const { sha256File, KEEP } = require('../src/updater');

// Not shipped: player data, caches, build leftovers.
const SKIP = [...KEEP, /\.pdb$/i, /^hl2\/save\//i, /\/cache\//i, /^bin\/x64\/cef\/.*\.log$/i, /\.bak$/i, /^launcher/i];

async function main() {
  const [gameDir, outDir, version = new Date().toISOString().slice(0, 10)] = process.argv.slice(2);
  if (!gameDir || !outDir) {
    console.error('usage: node tools/make-manifest.js <gameDir> <outDir> [version]');
    process.exit(1);
  }
  const files = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, e.name);
      if (e.isDirectory()) walk(full);
      else files.push(path.relative(gameDir, full).replace(/\\/g, '/'));
    }
  };
  walk(gameDir);
  const manifest = { version, files: [] };
  for (const rel of files.sort()) {
    if (SKIP.some((re) => re.test(rel))) continue;
    const src = path.join(gameDir, rel);
    const dst = path.join(outDir, 'files', rel);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.copyFileSync(src, dst);
    manifest.files.push({ path: rel, size: fs.statSync(src).size, sha256: await sha256File(src) });
  }
  fs.writeFileSync(path.join(outDir, 'manifest.json'), JSON.stringify(manifest, null, 1));
  const mb = manifest.files.reduce((a, f) => a + f.size, 0) / 1048576;
  console.log(`manifest: ${manifest.files.length} files, ${mb.toFixed(1)} MB, version ${version}`);
}

main().catch((e) => { console.error(e); process.exit(1); });
