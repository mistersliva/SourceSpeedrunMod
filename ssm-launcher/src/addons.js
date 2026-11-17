// Optional downloads next to the mod:
//   cutsceneless  Cutsceneless maps (peng's patch), from their own GitHub
//                 repository, into <game>\cutsceneless\maps. The game mounts
//                 them when "Cutsceneless" is on in its settings.
//   ffmpeg        ffmpeg.exe into <game>\bin, used by the game to render
//                 replays into .mp4 (Windows builds from github.com/FFmpeg-Builds).
'use strict';

const fs = require('fs');
const path = require('path');
const modInstall = require('./mod-install');

const UA = { Accept: 'application/vnd.github+json', 'User-Agent': 'SourceSpeedrunMod-Launcher' };
const VERSION_FILE = 'ssm-addon-version.txt';

async function githubJson(url) {
  const res = await fetch(url, { headers: UA });
  if (res.status === 404) throw new Error('Not published yet.');
  if (!res.ok) throw new Error(`GitHub: HTTP ${res.status}`);
  return res.json();
}

const ADDONS = {
  cutsceneless: {
    title: 'Cutsceneless maps',
    // Folder inside the game folder; the zip has one top-level folder.
    folder: 'cutsceneless',
    async release(settings) {
      const r = await githubJson(`https://api.github.com/repos/${settings.cutscenelessRepo}/releases/latest`);
      const asset = (r.assets || []).find((a) => /\.zip$/i.test(a.name));
      if (!asset) throw new Error(`Release ${r.tag_name} has no zip.`);
      return { tag: r.tag_name, asset: { name: asset.name, size: asset.size, url: asset.browser_download_url } };
    },
    installed(gameDir) {
      const dir = path.join(gameDir, 'cutsceneless');
      if (!fs.existsSync(path.join(dir, 'maps'))) return '';
      try { return fs.readFileSync(path.join(dir, VERSION_FILE), 'utf8').trim() || 'installed'; } catch { return 'installed'; }
    },
    async install(gameDir, rel, onProgress) {
      const dest = path.join(gameDir, 'cutsceneless');
      // Replace, never merge, so maps removed upstream do not linger.
      fs.rmSync(dest, { recursive: true, force: true });
      await modInstall.installArchive({ asset: rel.asset, dir: gameDir, dest, tarArgs: ['--strip-components', '1'], onProgress });
      fs.writeFileSync(path.join(dest, VERSION_FILE), rel.tag);
    },
    remove(gameDir) {
      fs.rmSync(path.join(gameDir, 'cutsceneless'), { recursive: true, force: true });
    },
  },

  ffmpeg: {
    title: 'ffmpeg (replay videos)',
    async release() {
      // FFmpeg-Builds publishes rolling builds under the "latest" tag; take the newest
      // release-branch GPL build (it includes libx264).
      const r = await githubJson('https://api.github.com/repos/FFmpeg-Builds/FFmpeg-Builds/releases/tags/latest');
      const builds = (r.assets || [])
        .map((a) => ({ a, m: /^ffmpeg-n(\d+)\.(\d+)-latest-win64-gpl-\d+\.\d+\.zip$/i.exec(a.name) }))
        .filter((x) => x.m)
        .sort((x, y) => (Number(y.m[1]) - Number(x.m[1])) || (Number(y.m[2]) - Number(x.m[2])));
      if (!builds.length) throw new Error('No ffmpeg build found.');
      const { a, m } = builds[0];
      return { tag: `n${m[1]}.${m[2]}`, asset: { name: a.name, size: a.size, url: a.browser_download_url } };
    },
    installed(gameDir) {
      return fs.existsSync(path.join(gameDir, 'bin', 'ffmpeg.exe')) ? 'installed' : '';
    },
    async install(gameDir, rel, onProgress) {
      // Only bin\ffmpeg.exe from "<build>/bin/ffmpeg.exe".
      await modInstall.installArchive({
        asset: rel.asset, dir: gameDir, dest: path.join(gameDir, 'bin'),
        tarArgs: ['--strip-components', '2', '*/bin/ffmpeg.exe'], onProgress, spaceFactor: 1.6,
      });
      if (!fs.existsSync(path.join(gameDir, 'bin', 'ffmpeg.exe'))) throw new Error('ffmpeg.exe was not found in the download.');
    },
    remove(gameDir) {
      fs.rmSync(path.join(gameDir, 'bin', 'ffmpeg.exe'), { force: true });
    },
  },
};

// Release info is cached per add-on (GitHub allows 60 API requests an hour).
const cache = {};
async function release(id, settings, force = false) {
  const c = cache[id];
  if (!force && c && Date.now() - c.at < 10 * 60 * 1000) return c;
  const next = { at: Date.now(), rel: null, error: '' };
  try { next.rel = await ADDONS[id].release(settings); } catch (e) { next.error = e.message; }
  cache[id] = next;
  return next;
}

async function status(gameDir, settings) {
  const out = {};
  for (const id of Object.keys(ADDONS)) {
    const c = await release(id, settings);
    out[id] = {
      title: ADDONS[id].title,
      installed: gameDir ? ADDONS[id].installed(gameDir) : '',
      latest: c.rel?.tag || '',
      size: c.rel?.asset.size || 0,
      error: c.error,
    };
  }
  return out;
}

async function install(id, gameDir, settings, onProgress) {
  if (!ADDONS[id]) throw new Error('Unknown add-on.');
  if (!gameDir) throw new Error('Install the mod first.');
  const c = await release(id, settings, true);
  if (!c.rel) throw new Error(c.error || 'Download not found.');
  await ADDONS[id].install(gameDir, c.rel, onProgress);
  onProgress({ phase: 'done' });
  return c.rel.tag;
}

async function remove(id, gameDir) {
  if (!ADDONS[id] || !gameDir) return;
  if (await modInstall.isGameRunning(gameDir)) throw new Error('Close the game first.');
  ADDONS[id].remove(gameDir);
}

module.exports = { status, install, remove, ADDONS };
