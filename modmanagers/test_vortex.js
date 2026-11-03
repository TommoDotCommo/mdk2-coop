// T0 for the Vortex extension against a stub vortex-api. Game-free.
//   node modmanagers/test_vortex.js            asserts registerGame's fields, the tool and the installer
//   MUTANT=modpath node modmanagers/test_vortex.js   must fail (the mod path is not the game folder)
//   MUTANT=steamid  ...                              must fail (wrong Steam app id)
const assert = require('assert');
const fs = require('fs');
const Module = require('module');
const path = require('path');

let src = fs.readFileSync(path.join(__dirname, 'vortex/game-mdk2hd/index.js'), 'utf8');
if (process.env.MUTANT === 'modpath') src = src.replace("queryModPath: () => '.'", "queryModPath: () => 'mods'");
if (process.env.MUTANT === 'steamid') src = src.replace("STEAMAPP_ID = '213350'", "STEAMAPP_ID = '1'");

const stub = { util: { GameStoreHelper: { findByAppId: ids => Promise.resolve({ gamePath: 'C:/steam/' + ids[0] }) } } };
const mod = new Module(path.join(__dirname, 'vortex/game-mdk2hd/index.js'));
mod.filename = mod.id;
mod.paths = [];
const origLoad = Module._load;
Module._load = function (request, ...rest) { return request === 'vortex-api' ? stub : origLoad.call(this, request, ...rest); };
mod._compile(src, mod.filename);
Module._load = origLoad;

const calls = { games: [], installers: [] };
const context = { registerGame: g => calls.games.push(g), registerInstaller: (...a) => calls.installers.push(a) };
assert.strictEqual(mod.exports.default(context), true);

(async () => {
  assert.strictEqual(calls.games.length, 1);
  const g = calls.games[0];
  assert.strictEqual(g.id, 'mdk2hd');
  assert.strictEqual(g.name, 'MDK2 HD');
  assert.strictEqual(g.executable(), 'mdk2hd.exe');
  assert.deepStrictEqual(g.requiredFiles, ['mdk2hd.exe']);
  assert.strictEqual(g.queryModPath(), '.');
  assert.strictEqual(g.mergeMods, true);
  assert.strictEqual(g.environment.SteamAPPId, '213350');
  assert.strictEqual(g.details.steamAppId, 213350);
  assert.strictEqual(await g.queryPath(), 'C:/steam/213350');
  assert.strictEqual(g.supportedTools.length, 1);
  const t = g.supportedTools[0];
  assert.strictEqual(t.id, 'mdk2-coop-launcher');
  assert.strictEqual(t.executable(), 'mdk2hd.exe');
  assert.strictEqual(t.defaultPrimary, true);

  // the installer lifts the release zip's game-folder files out of its top folder, and leaves a root-mirroring zip alone
  const [id, prio, test, install] = calls.installers[0];
  assert.strictEqual(id, 'mdk2-coop');
  const top = 'mdk2-coop-v1.0.0/';
  const release = ['coop-mod.json', top, top + 'coop/', top + 'dinput8.dll', top + 'coop/coop.lua', top + 'MDK2-Coop-Setup.exe',
    top + 'README.txt', top + 'coop-mod.json'];
  assert.strictEqual((await test(release, 'mdk2hd')).supported, true);
  assert.strictEqual((await test(release, 'skyrim')).supported, false);
  assert.strictEqual((await test(['dinput8.dll', 'coop/coop.lua'], 'mdk2hd')).supported, false);
  const got = (await install(release)).instructions.map(i => [i.type, i.source, i.destination.split(path.sep).join('/')]);
  assert.deepStrictEqual(got, [['copy', top + 'dinput8.dll', 'dinput8.dll'], ['copy', top + 'coop/coop.lua', 'coop/coop.lua']]);
  console.log('PASS: vortex extension');
})().catch(e => { console.error('FAIL: ' + e.message); process.exit(1); });
