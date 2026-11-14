# SourceSpeedrunMod launcher

Electron launcher in the mod's own style. It:

- finds Steam, its libraries and the Half-Life 2 install (app 220) on this PC,
- lets the player **sign in through Steam** and checks that the account owns HL2,
- points the mod's `hl2/gameinfo.txt` at wherever Steam installed HL2, then starts `hl2_win64.exe`,
- downloads mod updates (only changed files, SHA-256 verified).

```
npm install            # once
npm start              # run from source (uses ../game as the mod folder)
npm run dist           # build dist/SSM-Launcher.exe (single portable exe)
```

Put `SSM-Launcher.exe` in the mod folder (next to `hl2_win64.exe`); it finds the
folder by itself. Otherwise the player picks it in Settings.

## Steam sign-in and ownership

Sign-in is **Steam OpenID** (what websites use): Steam's page opens in the
player's browser and redirects back to a one-shot server on `127.0.0.1`. The
password only ever goes to Steam; the launcher gets the SteamID64 and confirms
it with Steam (`check_authentication`) before trusting it.

Steam only reveals which games an account owns through the **Web API**, which
needs a key. Never put the key in the launcher (anyone can extract it). Run the
tiny server in `tools/ownership-worker.js` (Cloudflare Worker, free tier; steps
are at the top of the file) and ship this next to the exe:

```json
// launcher-config.json
{
  "ownershipEndpoint": "https://ssm-ownership.<you>.workers.dev/owns",
  "updateUrl": "https://<your host>/ssm/manifest.json"
}
```

Without it the launcher still knows HL2 is playable when the account signed in
to Steam on this PC has it installed. For a private profile ("Game details:
private") the Web API can't tell, and the launcher shows "Unknown" instead of blocking.

## Downloads and updates

Two parts: getting the mod the first time, and keeping it current.

**First download.** Zip the mod folder (`game\` without `hl2\save`,
`hl2\cfg\config.cfg`, `*.pdb`) together with `SSM-Launcher.exe` and publish
it as a **GitHub Release** asset (2 GB per file, free bandwidth). The game
content itself is never shipped: players need Half-Life 2 from Steam, and the
launcher mounts it from their install.

**Updates.** Build a release folder from your current build and upload it to
any static host (GitHub Pages, a Release, Cloudflare R2, S3...):

```
node tools/make-manifest.js ..\game ..\release 1.1.0
```

This writes `release/manifest.json` (path, size and SHA-256 per file) and copies
the files to `release/files/`. Upload the whole `release` folder and set
`updateUrl` to the manifest's URL. The launcher's Updates page compares hashes,
downloads only what changed into `.update\`, verifies each file, then moves
them into place. Player saves, `config.cfg`, `splits\` and `custom\` are never
touched. GitHub Releases can't hold a folder tree; for those, host `release/`
on GitHub Pages or R2 instead.

**Updating the launcher itself.** Switch the build target to `nsis` and add
`electron-updater` with a `publish` entry for GitHub Releases. The installed
launcher then updates itself from new releases. The portable exe can't
update itself; ship a new exe in the mod update instead.

**Code signing.** Unsigned exes get a SmartScreen warning on first run. An OV/EV
code-signing certificate (or Azure Trusted Signing) removes it; electron-builder
signs automatically once `CSC_LINK` / `CSC_KEY_PASSWORD` are set.
