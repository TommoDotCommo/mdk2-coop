@echo off
rem Upscales the level textures of MDK2 HD 4x with AI (Real-ESRGAN). See README.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mdk2-textures.ps1" %*
