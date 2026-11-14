// "Sign in through Steam" and the Half-Life 2 ownership check.
//
// Sign-in is Steam OpenID 2.0, the flow websites use, done the standard way
// for desktop apps: Steam's sign-in page opens in the user's own browser and
// redirects back to a one-shot server on 127.0.0.1. The launcher never sees
// the password, only the SteamID64 that Steam vouches for — confirmed with
// Steam itself (check_authentication) before it is trusted.
//
// Ownership: Steam only reveals a user's games through the Web API, which
// needs an API key. Sources, in order:
//   1. ownershipEndpoint — a small server that holds the key (tools/ownership-worker.js),
//   2. steamApiKey — a key in launcher-config.json (for your own testing only:
//      never ship a key inside the launcher),
//   3. the local Steam install: the account signed in to Steam on this PC has
//      Half-Life 2 installed (and installed it, or plays it via family sharing).
'use strict';

const http = require('http');
const { shell } = require('electron');

const OPENID = 'https://steamcommunity.com/openid/login';
const SIGNIN_TIMEOUT_MS = 5 * 60 * 1000;

function openIdUrl(returnTo) {
  const p = new URLSearchParams({
    'openid.ns': 'http://specs.openid.net/auth/2.0',
    'openid.mode': 'checkid_setup',
    'openid.return_to': returnTo,
    'openid.realm': new URL('/', returnTo).href,
    'openid.identity': 'http://specs.openid.net/auth/2.0/identifier_select',
    'openid.claimed_id': 'http://specs.openid.net/auth/2.0/identifier_select',
  });
  return `${OPENID}?${p}`;
}

// Ask Steam whether the signed assertion it redirected with is genuine.
async function verify(params, returnTo) {
  if (params['openid.op_endpoint'] !== OPENID) throw new Error('unexpected OpenID provider');
  if (!String(params['openid.return_to'] || '').startsWith(returnTo)) throw new Error('unexpected return address');
  const body = new URLSearchParams(params);
  body.set('openid.mode', 'check_authentication');
  const res = await fetch(OPENID, { method: 'POST', body, headers: { 'Content-Type': 'application/x-www-form-urlencoded' } });
  const text = await res.text();
  if (!/is_valid\s*:\s*true/.test(text)) throw new Error('Steam did not confirm the sign-in');
  const m = /^https:\/\/steamcommunity\.com\/openid\/id\/(\d{17})$/.exec(params['openid.claimed_id'] || '');
  if (!m) throw new Error('unexpected Steam identity');
  return m[1];
}

function page(title, text) {
  return `<!doctype html><meta charset="utf-8"><title>${title}</title>
<style>html,body{margin:0;height:100%;background:#16181c;color:#e8e8e8;font-family:Bahnschrift,'Segoe UI',sans-serif}
body{display:grid;place-items:center}div{padding:2rem 2.4rem;background:#23252a;border-left:3px solid #7cc8f5}
h1{margin:0 0 .6rem;font-weight:400;letter-spacing:.1em;text-transform:uppercase;font-size:1.3rem}p{margin:0;color:#b4b6ba}</style>
<div><h1>${title}</h1><p>${text}</p></div>`;
}

let pending = null; // only one sign-in at a time

function signIn() {
  if (pending) pending.cancel();
  return new Promise((resolve, reject) => {
    let returnTo = '';
    let settled = false;
    const server = http.createServer(async (req, res) => {
      const url = new URL(req.url, 'http://127.0.0.1');
      if (url.pathname !== '/openid') { res.writeHead(404).end(); return; }
      const params = Object.fromEntries(url.searchParams);
      const reply = (status, title, text) => {
        res.writeHead(status, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(page(title, text));
      };
      if (params['openid.mode'] === 'cancel') {
        reply(200, 'Cancelled', 'You can close this tab.');
        return finish(new Error('cancelled'));
      }
      try {
        const steamid = await verify(params, returnTo);
        reply(200, 'Signed in', 'You can close this tab and return to the SourceSpeedrunMod launcher.');
        finish(null, steamid);
      } catch (e) {
        reply(400, 'Sign-in failed', e.message);
        finish(e);
      }
    });
    const timer = setTimeout(() => finish(new Error('Sign-in timed out')), SIGNIN_TIMEOUT_MS);
    function finish(err, steamid) {
      if (settled) return;
      settled = true;
      pending = null;
      clearTimeout(timer);
      server.close();
      err ? reject(err) : resolve(steamid);
    }
    pending = { cancel: () => finish(new Error('cancelled')) };
    server.on('error', (e) => finish(e));
    server.listen(0, '127.0.0.1', () => {
      returnTo = `http://127.0.0.1:${server.address().port}/openid`;
      shell.openExternal(openIdUrl(returnTo));
    });
  });
}

function cancelSignIn() {
  if (pending) pending.cancel();
}

// Public profile (name, avatar, privacy) from the community XML endpoint.
async function profile(steamid) {
  try {
    const res = await fetch(`https://steamcommunity.com/profiles/${steamid}/?xml=1`);
    const xml = await res.text();
    const tag = (t) => {
      const m = new RegExp(`<${t}>(?:<!\\[CDATA\\[)?([\\s\\S]*?)(?:\\]\\]>)?</${t}>`).exec(xml);
      return m ? m[1].trim() : '';
    };
    return { name: tag('steamID'), avatar: tag('avatarMedium'), privacy: tag('privacyState') };
  } catch {
    return { name: '', avatar: '', privacy: '' };
  }
}

// -> { owned: true | false | null, source }
async function ownsApp(steamid, appid, { config = {}, local = {} } = {}) {
  if (config.ownershipEndpoint) {
    try {
      const u = new URL(config.ownershipEndpoint);
      u.searchParams.set('steamid', steamid);
      u.searchParams.set('appid', String(appid));
      const r = await fetch(u);
      const j = await r.json();
      if (typeof j.owned === 'boolean') return { owned: j.owned, source: 'server' };
    } catch {}
  }
  if (config.steamApiKey) {
    try {
      const u = new URL('https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/');
      u.searchParams.set('key', config.steamApiKey);
      u.searchParams.set('steamid', steamid);
      u.searchParams.set('include_played_free_games', '1');
      u.searchParams.set('appids_filter[0]', String(appid));
      const j = await (await fetch(u)).json();
      const r = j.response || {};
      // An empty response means the profile's game details are private.
      if ('game_count' in r) return { owned: (r.games || []).some((g) => g.appid === appid), source: 'webapi' };
    } catch {}
  }
  const hl2 = local.hl2;
  if (hl2?.exists && (hl2.lastOwner === steamid || local.activeSteamId === steamid)) return { owned: true, source: 'local' };
  return { owned: null, source: 'unknown' };
}

module.exports = { signIn, cancelSignIn, profile, ownsApp };
