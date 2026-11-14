// The mod mounts Half-Life 2 content from the Steam install by absolute path
// (hl2/gameinfo.txt). Rewrite those paths for the install found on this PC so
// the mod works wherever Steam put HL2.
'use strict';

const fs = require('fs');
const path = require('path');

// "X:\...\steamapps\common\Half-Life 2" (any library), keeping the rest.
const HL2_PATH_RE = /"([A-Za-z]:\\[^"]*?\\steamapps\\common\\Half-Life 2)(\\[^"]*)?"/gi;

function currentHl2Path(gameDir) {
  try {
    const text = fs.readFileSync(path.join(gameDir, 'hl2', 'gameinfo.txt'), 'utf8');
    const m = new RegExp(HL2_PATH_RE.source, 'i').exec(text);
    return m ? m[1] : null;
  } catch {
    return null;
  }
}

// Returns { changed, from, to } or throws.
function pointAt(gameDir, hl2Dir) {
  const file = path.join(gameDir, 'hl2', 'gameinfo.txt');
  const text = fs.readFileSync(file, 'utf8');
  const target = path.normalize(hl2Dir).replace(/[\\/]+$/, '');
  let from = null;
  const next = text.replace(HL2_PATH_RE, (_, base, rest) => {
    from = from || base;
    return `"${target}${rest || ''}"`;
  });
  if (next === text) return { changed: false, from, to: target };
  fs.writeFileSync(`${file}.bak`, text);
  fs.writeFileSync(file, next);
  return { changed: true, from, to: target };
}

module.exports = { currentHl2Path, pointAt };
