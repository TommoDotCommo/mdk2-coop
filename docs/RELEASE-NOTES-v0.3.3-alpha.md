# MDK2 HD Co-op v0.3.3-alpha

This patch fixes graphical installer startup and flashing in v0.3.2-alpha.
Extract the entire ZIP, then open `MDK2-Coop-Setup.exe`.

## Installer fixes

- Setup opens its window without briefly creating console windows or hiding the
  setup window itself.
- Status checks and ordinary installation steps run without console windows.
- The read-only status helper explicitly runs as the current user, avoiding an
  unexpected administrator-permission requirement.
- Status refreshes cannot re-enter each other, and the installation inventory
  redraws in one batch.

## Validation

- 57 installer checks passed against disposable fixtures, covering startup,
  install, update, repair, uninstall, original-file restoration, preservation of
  saves and settings, and layout at different display scales.
- The real packaged setup window was verified visible and closed normally without
  installing. The packaged status helper ran without elevation.
- The hidden-process regression and deliberately broken console-window variant
  passed their expected checks; existing ownership-policy mutants were detected.
- The release package inventory and SHA-256 checksum were verified.

## Known limitations

- The broader installer gate still fails its separate native bridge data-loader
  smoke check. The 57 installer fixture checks above omit that optional probe.
- Elevated installation was not retested for this patch. The full multiplayer
  feature and campaign suites were not rerun; the gameplay limitations documented
  in [v0.3.2-alpha](https://github.com/TommoDotCommo/mdk2-coop/releases/tag/v0.3.2-alpha) still apply.
