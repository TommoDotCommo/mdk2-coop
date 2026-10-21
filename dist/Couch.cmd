@echo off
rem Couch co-op: 2-4 players on one PC, one gamepad each. See README.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-couch.ps1" %*
