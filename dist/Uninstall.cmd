@echo off
rem Removes mdk2-coop from MDK2 HD and restores settings.lua.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-coop.ps1" -Uninstall %*
