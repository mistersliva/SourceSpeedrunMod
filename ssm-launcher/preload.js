// Bridge between the launcher page and the main process (no Node in the page).
'use strict';

const { contextBridge, ipcRenderer } = require('electron');

async function call(channel, ...args) {
  const r = await ipcRenderer.invoke(channel, ...args);
  if (!r.ok) throw new Error(r.error);
  return r.value;
}

contextBridge.exposeInMainWorld('launcher', {
  state: () => call('state'),
  play: () => call('play'),
  setSettings: (patch) => call('settings:set', patch),
  pickGameDir: () => call('game:pickDir'),
  openGameDir: () => call('game:openDir'),
  steamSignIn: () => call('steam:signIn'),
  steamSignOut: () => call('steam:signOut'),
  steamCancel: () => call('steam:cancel'),
  steamRefresh: () => call('steam:refresh'),
  steamInstallHl2: () => call('steam:install'),
  steamStore: () => call('steam:store'),
  checkMod: () => call('mod:check'),
  installMod: () => call('mod:install'),
  cancelInstall: () => call('mod:cancel'),
  installAddon: (id) => call('addon:install', id),
  removeAddon: (id) => call('addon:remove', id),
  pickInstallDir: () => call('mod:pickInstallDir'),
  launcherUpdate: () => call('launcher:update'),
  restartToUpdate: () => call('launcher:restart'),
  minimize: () => call('win:minimize'),
  close: () => call('win:close'),
  on: (channel, fn) => {
    if (!['state-changed', 'toast', 'install:progress', 'launcher-update'].includes(channel)) return;
    ipcRenderer.on(channel, (_e, data) => fn(data));
  },
});
