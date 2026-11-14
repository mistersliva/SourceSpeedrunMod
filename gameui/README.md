# HTML GameUI

`gameui.dll` is an HTML/CSS/JS implementation of the Source engine game UI. The
main menu, pause menu, New Game / Load / Save dialogs, Options, the loading
screen and the developer console are web pages rendered off-screen by
[Chromium Embedded Framework](https://bitbucket.org/chromiumembedded/cef) and
composited by vgui over the game. The engine still talks to the DLL through the
unchanged `IGameUI` (`GameUI011`) and `IGameConsole` (`GameConsole005`)
interfaces, so no engine changes are required.

x64 Windows only.

## Layout

| Path | What |
| --- | --- |
| `cefbridge/` | CMake project (C++20). `htmlhost.h` is a CEF-free facade; builds `htmlhost.lib`, `libcef_dll_wrapper.lib` and the helper process `gameui_cefsub.exe`. |
| `HtmlUI.*` | Owns the browser view, the fullscreen vgui popup that presents it (BGRA texture upload, mouse/keyboard forwarding), the event queue and request routing. |
| `UiApi.*` | Engine API callable from JS (`/api/<method>`). |
| `UiAssets.*` | Static content: UI files, raw game files, VTF/TGA → PNG. |
| `GameUI_Interface.*`, `GameConsole.*` | `IGameUI` / `IGameConsole` implementations that forward engine notifications to the page. |
| `htmlui/` | The web UI (copied to `game/platform/htmlui` by the post-build step). |

## Building

`build_game_thirdparty.bat x64` now also runs `build_htmlui_thirdparty.bat x64`,
which downloads the pinned CEF minimal distribution into `thirdparty/cef`
(SHA1-verified) and builds `cefbridge`. Then build the solution as usual. The
gameui post-build step copies the CEF runtime into `game/bin/x64/cef/` and the
web UI into `game/platform/htmlui/`.

## Content resolution

| URL | Source |
| --- | --- |
| `https://gameui.local/ui/<path>` | `htmlui/<path>` from the `GAME` search path (mods can override any file), then `PLATFORM` |
| `https://gameui.local/game/<path>` | any file from the `GAME` search path (fonts, sounds, …) |
| `https://gameui.local/img/vtf/<material>` | `materials/<material>.vtf` decoded to PNG |
| `https://gameui.local/img/tga/<path>` | `<path>` (TGA) decoded to PNG, e.g. save thumbnails |

## JavaScript API (`js/source.js`)

```js
await Source.call('cvar.get', { names: ['volume'] });   // any API method
Source.cmd('map d1_trainstation_01');                     // console command
await Source.cvar.set('volume', 0.5);
Source.on('activated', (state) => { … });                 // engine events
Source.L('#GameUI_NewGame', 'New Game');                  // localized string
Source.sound('ui/buttonclick.wav');
```

API methods: `ui.ready`, `ui.escape`, `ui.reload`, `ui.devtools`, `state`, `cmd`,
`quit`, `cvar.get`, `cvar.set`, `config.save`, `localize`, `localize.prefix`,
`game.info`, `sound.play`, `chapters`, `newgame`, `saves.list`, `saves.load`,
`saves.save`, `saves.delete`, `video.get`, `video.set`, `binds.list`,
`binds.keyname`, `binds.set`, `binds.unbind`, `binds.defaults`,
`console.submit`, `console.complete`, `console.close`.

Events: `activated`, `hidden`, `state`, `escape`, `loading.start`,
`loading.progress`, `loading.end`, `loading.pending`, `console.print`,
`console.show`, `console.hide`, `console.clear`, `newgame.show`,
`quit.confirm`, `menu.command`, `disconnected`, `credits.finished`.

## UI layer (`js/ui.js`)

* Layers: `<section class="layer" data-layer="name">`, `UI.open/close/closeTop`.
  While a layer, modal or Esc interceptor is active the engine's Esc-to-resume
  is blocked and Esc closes the top-most item instead.
* `await UI.modal({ title, text, buttons })`, `UI.toast(text)`.
* Declarative bindings: `data-loc`, `data-cvar` (range / checkbox / select),
  `data-cmd`, `data-open`, `data-close`, `data-tabs`.

Opening `gameui/htmlui/index.html` from any regular web server uses a mock
backend, so the UI can be developed in a desktop browser.

## Console commands / launch options

* `htmlui_reload` — reload the page (pick up edited files).
* `htmlui_devtools` — open Chromium DevTools.
* `htmlui_fps` — renderer frame rate (default 60).
* `-htmlui_debug [port]` — enable remote DevTools on `http://127.0.0.1:<port>` (default 9222).
* `-htmlui_nogpu` — software rendering for the UI.
