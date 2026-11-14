// Half-Life 2 ownership check for the launcher, as a Cloudflare Worker (free tier
// is plenty). It keeps the Steam Web API key on the server, where it belongs.
//
// Deploy:
//   1. Get a key: https://steamcommunity.com/dev/apikey
//   2. npm create cloudflare@latest ssm-ownership  (choose "Hello World" Worker)
//      and replace src/index.js with this file
//   3. npx wrangler secret put STEAM_API_KEY      (paste the key)
//   4. npx wrangler deploy                        → https://ssm-ownership.<you>.workers.dev
//   5. launcher-config.json next to the launcher exe:
//      { "ownershipEndpoint": "https://ssm-ownership.<you>.workers.dev/owns" }
//
// GET /owns?steamid=7656119...&appid=220  ->  { "owned": true | false | null }
// null = the player's "Game details" are private on Steam (they can make them
// public, or the launcher falls back to the local Steam install).

const ALLOWED_APPS = new Set([220]); // Half-Life 2

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const json = (body, status = 200) => new Response(JSON.stringify(body), {
      status,
      headers: { 'content-type': 'application/json', 'cache-control': 'max-age=600' },
    });
    if (url.pathname !== '/owns') return json({ error: 'not found' }, 404);
    const steamid = url.searchParams.get('steamid') || '';
    const appid = Number(url.searchParams.get('appid') || 220);
    if (!/^7656\d{13}$/.test(steamid) || !ALLOWED_APPS.has(appid)) return json({ error: 'bad request' }, 400);

    const cache = caches.default;
    const key = new Request(`https://cache.local/owns/${steamid}/${appid}`);
    const hit = await cache.match(key);
    if (hit) return hit;

    const api = new URL('https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/');
    api.searchParams.set('key', env.STEAM_API_KEY);
    api.searchParams.set('steamid', steamid);
    api.searchParams.set('include_played_free_games', '1');
    api.searchParams.set('appids_filter[0]', String(appid));
    const r = await fetch(api);
    if (!r.ok) return json({ owned: null, error: `steam ${r.status}` }, 502);
    const data = (await r.json()).response || {};
    const body = 'game_count' in data
      ? { owned: (data.games || []).some((g) => g.appid === appid) }
      : { owned: null, reason: 'private' };
    const res = json(body);
    ctx.waitUntil(cache.put(key, res.clone()));
    return res;
  },
};
