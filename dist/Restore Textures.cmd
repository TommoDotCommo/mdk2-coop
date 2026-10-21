@echo off
rem Puts the original MDK2 HD textures back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-textures.ps1" -Restore %*
