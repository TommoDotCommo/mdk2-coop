@echo off
rem Installs mdk2-coop into MDK2 HD (asks host or join). See README.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-coop.ps1" %*
