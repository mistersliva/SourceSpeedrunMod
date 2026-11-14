# SourceSpeedrunMod launcher

Electron launcher in the mod's own style. Players download only the launcher;
it installs and updates the mod itself.

- **Install / Update / Play.** The big button downloads the mod from the latest
  GitHub release with progress (MB, speed, time left), extracts it, and turns
  into **Play**. Newer releases show up as **Update**. Cancel any time.
- Finds Steam, its libraries and the Half-Life 2 install (app 220).
- **Sign in through Steam** and a Half-Life 2 ownership check.
- Points the mod's `hl2/gameinfo.txt` at wherever Steam installed HL2, then starts the game.

```
npm install            # once
npm start              # run from source (uses ../game as the mod folder if present)
npm run dist           # build dist/SSM-Launcher-Setup.exe
```

`SSM-Launcher-Setup.exe` is a one-click installer. It asks for admin rights once
(UAC) and installs to `C:\Program Files\SSM-Launcher`. The mod is downloaded
into `SSM-Launcher\Games\SourceSpeedrunMod`; the installer makes `Games\`
writable for normal users, because the launcher runs without admin rights and
the game writes saves and configs into its own folder. Uninstalling or updating
the launcher never deletes `Games\` (see `build/installer.nsh`). Installed, the
launcher starts in well under a second.

## Publishing a mod release

The launcher reads `https://api.github.com/repos/<repo>/releases/latest`
(`mistersliva/SourceSpeedrunMod`; change it with `releaseRepo` in
`launcher-config.json` next to the launcher exe). It downloads the asset named
`SourceSpeedrunMod-<version>.zip`. The zip must hold a single top-level
`SourceSpeedrunMod/` folder with the game files, and no player data (saves,
`config.cfg`, `autoexec.cfg`, splits, the `bin/x64/cef/cache` profile) or `.pdb` files.

1. Build the game (Release), then package it:

   ```
   powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -GameDir ..\game -Version v1.1.0 -OutDir ..\release
   ```

   It copies the game without the files above and adds the Visual C++ runtime
   next to the binaries (players don't need the VC++ redistributable). It then
   **refuses to package** anything that would fail on a clean PC: DLLs that need
   Visual Studio's debug runtime, or imports that are neither in the package nor
   part of Windows. That check exists because v1.0.0 shipped debug builds of the
   Lua/Squirrel DLLs and failed on PCs without Visual Studio.
2. `gh release create v1.1.0 ..\release\SourceSpeedrunMod-v1.1.0.zip dist\SSM-Launcher-Setup.exe`

The tag is the mod version. Launchers with an older installed version
(`ssm-version.txt` in the mod folder) offer **Update**. Updates extract over the
install, so player saves and configs stay. A development build (no version
file) never gets overwritten.

## Steam sign-in and ownership

Sign-in is **Steam OpenID** (what websites use): Steam's page opens in the
player's browser and redirects back to a one-shot server on `127.0.0.1`. The
password only ever goes to Steam; the launcher gets the SteamID64 and confirms
it with Steam (`check_authentication`) before trusting it.

Steam only reveals which games an account owns through the **Web API**, which
needs a key. Never put the key in the launcher (anyone can extract it). Run the
tiny server in `tools/ownership-worker.js` (Cloudflare Worker, free tier; steps
are at the top of the file) and ship this next to the launcher exe:

```json
{ "ownershipEndpoint": "https://ssm-ownership.<you>.workers.dev/owns" }
```

Without it the launcher still knows HL2 is playable when the account signed in
to Steam on this PC has it installed. For a private profile ("Game details:
private") the Web API can't tell, and the launcher shows "Unknown" instead of blocking.

## Code signing

Unsigned installers get a SmartScreen warning on first run. An OV/EV
code-signing certificate (or Azure Trusted Signing) removes it; electron-builder
signs automatically once `CSC_LINK` / `CSC_KEY_PASSWORD` are set.
