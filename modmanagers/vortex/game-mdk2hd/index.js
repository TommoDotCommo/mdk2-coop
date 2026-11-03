// Vortex game extension for MDK2 HD and the MDK2 Co-op mod (mod managers: Vortex, MO2, by hand: see the README).
// Same layout as the extensions in Nexus-Mods/vortex-games: game-mdk2hd/{index.js, info.json, gameart.jpg}.
// The mod is a drop-in package: its files go into the game folder as they are (queryModPath '.'), and the mod
// sets up everything else itself at its first start. Nothing to configure after installing.
const path = require('path');
const { util } = require('vortex-api');

// The Nexus Mods game domain. Vortex uses it as the game id; change it here if Nexus lists the game under another name.
const GAME_ID = 'mdk2hd';
const STEAMAPP_ID = '213350';
const EXECUTABLE = 'mdk2hd.exe';
// The files a release puts into the game folder (coop-mod.json in the package lists them with their SHA-256).
const GAME_FILES = ['dinput8.dll', 'coop/coop.lua'];

function norm(file) {
  return file.split(path.sep).join('/').split('\\').join('/');
}

// Steam is the only store with MDK2 HD that Vortex knows; the Overhaul Games (Beamdog) build is found by Browse.
function findGame() {
  return util.GameStoreHelper.findByAppId([STEAMAPP_ID]).then(game => game.gamePath);
}

// The release zip keeps the game-folder files in one top folder (<name>/dinput8.dll) beside Setup and the README, with
// a copy of coop-mod.json at the zip root. Vortex would install that folder into the game folder as it is, so this
// installer lifts the game-folder files out. A zip that already mirrors the game folder (dinput8.dll at the root) is
// left to Vortex's own installer.
function topFolderLayout(files) {
  const list = files.map(norm);
  const top = list.find(f => /^[^/]+\/dinput8\.dll$/i.test(f));
  if (top === undefined) {
    return undefined;
  }
  const folder = top.split('/')[0];
  return GAME_FILES.every(f => list.includes(folder + '/' + f)) ? folder : undefined;
}

function testSupported(files, gameId) {
  const supported = gameId === GAME_ID && topFolderLayout(files) !== undefined;
  return Promise.resolve({ supported, requiredFiles: [] });
}

function install(files) {
  const folder = topFolderLayout(files);
  const instructions = files.filter(f => !/[\\/]$/.test(f)).map(f => ({ f, n: norm(f) }))
    .filter(x => GAME_FILES.some(g => x.n === folder + '/' + g))
    .map(x => ({ type: 'copy', source: x.f, destination: x.n.slice(folder.length + 1).split('/').join(path.sep) }));
  return Promise.resolve({ instructions });
}

function main(context) {
  context.registerGame({
    id: GAME_ID,
    name: 'MDK2 HD',
    mergeMods: true,
    queryPath: findGame,
    queryModPath: () => '.',
    logo: 'gameart.jpg',
    executable: () => EXECUTABLE,
    requiredFiles: [EXECUTABLE],
    environment: { SteamAPPId: STEAMAPP_ID },
    details: { steamAppId: parseInt(STEAMAPP_ID, 10) },
    // The co-op launcher is not a separate program: the mod opens it when the game starts (Shift brings it back if it
    // is switched off). So the tool is the game itself, started the way the mod expects, and the default primary.
    supportedTools: [
      {
        id: 'mdk2-coop-launcher',
        name: 'MDK2 Co-op launcher',
        logo: 'gameart.jpg',
        executable: () => EXECUTABLE,
        requiredFiles: [EXECUTABLE],
        relative: true,
        defaultPrimary: true,
      },
    ],
  });
  context.registerInstaller('mdk2-coop', 25, testSupported, install);
  return true;
}

module.exports = { default: main, GAME_ID, GAME_FILES };
