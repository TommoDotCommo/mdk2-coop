# mdk2-coop

Co-op for MDK2 HD (Steam app 213350, build 3902). Work in progress, nothing playable yet.
Built with the [Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; research notes live in
`games/mdk2-hd/NOTES.md` there.

No game files are in this repo. You need your own copy of MDK2 HD.

## Dev harness

`lua/coop_dev.lua` is appended to `%LOCALAPPDATA%\MDK2HD\settings.lua`, which the game executes at boot
after its own scripts.

    tools/inject.sh         # inject (backs up settings.lua once to settings.lua.coop-backup)
    tools/inject.sh --off   # remove

In game: F9 BioWare debug menu, F7 jump to level 1 checkpoint 2, F6 spawn player 2 puppet (follows you),
F8 hand the puppet to the game's Kurt AI.

Untested: changing options in game probably rewrites settings.lua and drops the injection; if so, rerun the script.
