# The installer names the game build it found and warns before installing on a build the mod does not know.
# Fixtures are synthetic exes (tools/fixtures/make_build_fixtures.py); nothing is run. Mutants (edited copies of the
# package scripts) must each fail at least one check:
#   unknown installs silently (backend), the GUI never asks, an unknown build is reported as known, -AcceptUnknownBuild ignored.
param([Parameter(Mandatory=$true)][string]$PackageDir,
      [Parameter(Mandatory=$true)][string]$Fixtures,
      [Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
$root = Join-Path $TestRoot "builds-$PID"
New-Item -ItemType Directory -Force $root | Out-Null
$cases = @(
    @{ exe='steam.exe';     found='Found: MDK2 HD Steam build 3902'; known=$true },
    @{ exe='overhaul.exe';  found='Found: MDK2 HD Overhaul Games build'; known=$true },
    @{ exe='unknown-pointer.exe'; found='Found: unknown build (SHA-256 '; known=$false },
    @{ exe='unknown-layout.exe';  found='Found: unknown build (SHA-256 '; known=$false },
    @{ exe='unknown-text.exe';    found='Found: unknown build (SHA-256 '; known=$false })
$script:counter = 0
function Gate-Failures($pkg, $label) {
    $bad = New-Object Collections.Generic.List[string]
    foreach ($case in $cases) {
        $script:counter++
        $dir = Join-Path $root "g$($script:counter)"
        New-Item -ItemType Directory $dir | Out-Null
        Copy-Item (Join-Path $Fixtures $case.exe) (Join-Path $dir 'mdk2hd.exe')
        $hash = (Get-FileHash (Join-Path $dir 'mdk2hd.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        $gui = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'installer-gui.ps1') -TestBuildGate $dir 2>&1) -join "`n"
        if (-not $gui.Contains($case.found)) { $bad.Add("$label $($case.exe): setup should show '$($case.found)', got: $gui") }
        if (-not $case.known -and -not $gui.Contains($hash)) { $bad.Add("$label $($case.exe): the SHA-256 must be shown") }
        if ($case.known -and -not $gui.Contains('NO WARNING')) { $bad.Add("$label $($case.exe): a known build must not warn") }
        if (-not $case.known -and -not ($gui.Contains('WARNING:') -and $gui.Contains('co-op will stay off'))) { $bad.Add("$label $($case.exe): an unknown build must warn that co-op stays off") }
        $settings = Join-Path $dir 'settings.lua'
        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'mdk2-coop.ps1') -GameDir $dir -SettingsPath $settings -Yes 2>&1
        $installed = Test-Path (Join-Path $dir 'dinput8.dll')
        if ($case.known -and (-not $installed -or $LASTEXITCODE -ne 0)) { $bad.Add("$label $($case.exe): a known build installs without questions ($out)") }
        if (-not $case.known) {
            if ($installed -or $LASTEXITCODE -eq 0) { $bad.Add("$label $($case.exe): an unknown build must not install silently") }
            $accepted = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'mdk2-coop.ps1') -GameDir $dir -SettingsPath $settings -Yes -AcceptUnknownBuild 2>&1
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $dir 'dinput8.dll'))) { $bad.Add("$label $($case.exe): -AcceptUnknownBuild installs after the warning ($accepted)") }
            if (-not ("$accepted").Contains('co-op will stay off')) { $bad.Add("$label $($case.exe): the warning is printed even when accepted") }
        }
    }
    return $bad
}
$bad = Gate-Failures $PackageDir 'real'
if ($bad.Count) { $bad | ForEach-Object { Write-Output "FAIL: $_" }; exit 1 }
Write-Output "PASS: installer names the game build and warns on unknown ones ($($cases.Count) fixtures)"
$mutants = @(
    @{ name='backend-unknown-installs-silently'; file='mdk2-coop.ps1'; old='if ($warning) {'; new='if ($false) {' },
    @{ name='gui-never-asks'; file='install-state.ps1'; old='if ($remove -or $build.Known) { return $null }'; new='return $null' },
    @{ name='unknown-reported-known'; file='install-state.ps1'; old='[int]$lines[0] -ge 0'; new='$true' },
    @{ name='accept-flag-ignored'; file='mdk2-coop.ps1'; old='if (-not $AcceptUnknownBuild) {'; new='if ($false) {' })
foreach ($m in $mutants) {
    $copy = Join-Path $root "mutant-$($m.name)"
    Copy-Item -Recurse $PackageDir $copy
    $path = Join-Path $copy $m.file
    $text = [IO.File]::ReadAllText($path)
    if (-not $text.Contains($m.old)) { throw "mutant $($m.name) does not match $($m.file)" }
    [IO.File]::WriteAllText($path, $text.Replace($m.old, $m.new))
    $failures = Gate-Failures $copy $m.name
    if (-not $failures.Count) { Write-Output "FAIL: mutant $($m.name) survived"; exit 1 }
    Write-Output "PASS: mutant $($m.name) caught ($($failures.Count) checks)"
}
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
