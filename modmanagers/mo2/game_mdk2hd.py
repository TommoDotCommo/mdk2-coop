"""Mod Organizer 2 game plugin (basic_games) for MDK2 HD and the MDK2 Co-op mod.

Copy this file into <MO2>/plugins/basic_games/games/ and restart MO2. The mod is a drop-in package: its files
(dinput8.dll and coop/coop.lua) go into the game folder, so the data path is the game folder itself (root install).
The co-op launcher is not a separate program: the mod opens it when the game starts, so the executable list has the
game once, named for that. See the README, "Mod managers", for the usvfs caveat about the proxy DLL.
"""
try:
    from PyQt6.QtCore import QFileInfo
except ImportError:  # MO2 before 2.5
    from PyQt5.QtCore import QFileInfo

import mobase

from ..basic_features import BasicModDataChecker, GlobPatterns
from ..basic_game import BasicGame


class MDK2HDGame(BasicGame):
    Name = "MDK2 HD Support Plugin"
    Author = "mdk2-coop"
    Version = "1.0.0"

    GameName = "MDK2 HD"
    GameShortName = "mdk2hd"
    # The Nexus Mods game domain; keep it equal to GAME_ID in the Vortex extension.
    GameNexusName = "mdk2hd"
    GameSteamId = 213350
    GameBinary = "mdk2hd.exe"
    GameDataPath = ""
    GameSupportURL = "https://github.com/TommoDotCommo/mdk2-coop#mod-managers"

    def init(self, organizer: mobase.IOrganizer):
        super().init(organizer)
        # The release zip keeps the game-folder files in one top folder beside Setup and the README: unfold the folder
        # and drop what is not a game-folder file, so the mod installs as dinput8.dll + coop/coop.lua in the game folder.
        self._register_feature(
            BasicModDataChecker(
                GlobPatterns(
                    unfold=["mdk2-coop-*"],
                    valid=["dinput8.dll", "dinput8.chain.dll", "coop", "coop-mod.json"],
                    delete=[
                        "MDK2-Coop-Setup.exe",
                        "*.ps1",
                        "install-*",
                        "Couch.cmd",
                        "README.txt",
                        "LICENSE.txt",
                    ],
                )
            )
        )
        return True

    def executables(self):
        game = QFileInfo(self.gameDirectory(), self.binaryName())
        return [mobase.ExecutableInfo("MDK2 Co-op launcher", game)]
