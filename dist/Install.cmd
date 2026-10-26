@echo off
rem Installs mdk2-coop into MDK2 HD. Host or join is chosen in the launcher. See README.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-coop.ps1" %*
