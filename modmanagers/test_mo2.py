#!/usr/bin/env python3
"""T0 for the MO2 plugin against stub mobase / basic_games modules. Game-free.
  test_mo2.py                   asserts the plugin's fields, executables and mod data patterns
  MUTANT=datapath test_mo2.py   must fail (data path is not the game folder)
  MUTANT=steamid  test_mo2.py   must fail (wrong Steam app id)"""
import importlib
import os
from pathlib import Path
import sys
import tempfile
import types

HERE = Path(__file__).resolve().parent
src = (HERE / 'mo2' / 'game_mdk2hd.py').read_text()
mutant = os.environ.get('MUTANT')
if mutant == 'datapath':
    src = src.replace('GameDataPath = ""', 'GameDataPath = "mods"')
elif mutant == 'steamid':
    src = src.replace('GameSteamId = 213350', 'GameSteamId = 1')

tmp = tempfile.mkdtemp()
pkg = Path(tmp) / 'basic_games_stub'
(pkg / 'games').mkdir(parents=True)
(pkg / '__init__.py').write_text('')
(pkg / 'games' / '__init__.py').write_text('')
(pkg / 'games' / 'game_mdk2hd.py').write_text(src)
(pkg / 'basic_game.py').write_text('''
class BasicGame:
    def __init__(self): self.features = []
    def init(self, organizer): return True
    def _register_feature(self, f): self.features.append(f)
    def gameDirectory(self): return "C:/Games/MDK2"
    def binaryName(self): return self.GameBinary
''')
(pkg / 'basic_features.py').write_text('''
class GlobPatterns:
    def __init__(self, unfold=None, valid=None, delete=None): self.unfold, self.valid, self.delete = unfold, valid, delete
class BasicModDataChecker:
    def __init__(self, patterns): self.patterns = patterns
''')
mobase = types.ModuleType('mobase')
mobase.ExecutableInfo = lambda name, path: (name, path)
mobase.IOrganizer = object
qt = types.ModuleType('PyQt6.QtCore')
qt.QFileInfo = lambda d, f: d + '/' + f
sys.modules.update({'mobase': mobase, 'PyQt6': types.ModuleType('PyQt6'), 'PyQt6.QtCore': qt})
sys.path.insert(0, tmp)
game_mod = importlib.import_module('basic_games_stub.games.game_mdk2hd')
g = game_mod.MDK2HDGame()
assert g.init(None) is True

assert g.GameName == 'MDK2 HD'
assert g.GameShortName == 'mdk2hd' and g.GameNexusName == 'mdk2hd'
assert g.GameSteamId == 213350, g.GameSteamId
assert g.GameBinary == 'mdk2hd.exe'
assert g.GameDataPath == '', 'root install: the data path must be the game folder'
assert g.executables() == [('MDK2 Co-op launcher', 'C:/Games/MDK2/mdk2hd.exe')], g.executables()
p = g.features[0].patterns
assert p.unfold == ['mdk2-coop-*'] and 'dinput8.dll' in p.valid and 'coop' in p.valid
assert 'MDK2-Coop-Setup.exe' in p.delete
print('PASS: mo2 plugin')
