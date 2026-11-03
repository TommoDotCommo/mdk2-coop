# Setup's scriptable verbs (co-op mod standard J6, J9) against disposable fixture game folders; no game is started.
#   MDK2-Coop-Setup.exe status [--json] [<game folder>] | install <game folder> --yes | uninstall <game folder> --yes
#   exit codes 0 done, 1 failed, 2 usage, 3 unsupported build, 4 the game is running
# Also: the old -StatusOnly -Yes switches, a plain copy of the release reported as a mod manager's (Setup refuses to replace
# it and its window offers Play, not "Needs attention"), and the single-file Setup. A profile folder under -TestRoot stands
# in for %LOCALAPPDATA%: the real settings.lua is never touched.
# Mutants (-Mutants), each must fail:  running-code (a running game gives exit 1, not 4), build-code (an unknown build gives
# exit 1, not 3), manager-overwrite (Setup installs over a mod manager's files).
# Windows PowerShell 5.1:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_setup_verbs.ps1 -PackageDir <unpacked release>
#       -Fixtures <tools/fixtures/make_build_fixtures.py output> -TestRoot <scratch> [-SingleFile <Setup.exe>] [-Mutants]
param([Parameter(Mandatory = $true)][string]$PackageDir, [Parameter(Mandatory = $true)][string]$Fixtures,
      [Parameter(Mandatory = $true)][string]$TestRoot, [string]$SingleFile, [switch]$Mutants)
$ErrorActionPreference = 'Stop'
$root = Join-Path $TestRoot ('verbs-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $root | Out-Null
$env:LOCALAPPDATA = Join-Path $root 'profile'
New-Item -ItemType Directory $env:LOCALAPPDATA | Out-Null
$script:failures = @()
function Check($cond, $what) { if (-not $cond) { $script:failures += $what } }

function Run-Setup($exe, [string[]]$arguments) {
    $out = Join-Path $root ([guid]::NewGuid().ToString('N') + '.out')
    $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $out
    $text = if (Test-Path $out) { [IO.File]::ReadAllText($out) } else { '' }
    return [pscustomobject]@{ code = $p.ExitCode; text = $text }
}
function New-Game($name, $exe) {
    $g = Join-Path $root $name
    New-Item -ItemType Directory $g | Out-Null
    Copy-Item $exe (Join-Path $g 'mdk2hd.exe')
    return $g
}
function Json-Of($r) { try { return $r.text | ConvertFrom-Json } catch { return $null } }

function Run-Suite($pkg, $label) {
    $script:failures = @()
    $setup = Join-Path $pkg 'MDK2-Coop-Setup.exe'
    $steam = Join-Path $Fixtures 'steam.exe'
    $g1 = New-Game "$label-g1" $steam
    $r = Run-Setup $setup @('status', '--json', $g1)
    $j = Json-Of $r
    Check ($r.code -eq 0 -and $j -and $j.installs[0].owner -eq 'none' -and $j.installs[0].known_build -and $j.installs[0].build -eq 'Steam build 3902') "status --json before install: $($r.code) $($r.text)"
    $r = Run-Setup $setup @('install', $g1, '--yes')
    Check ($r.code -eq 0) "install --yes exit $($r.code): $($r.text)"
    Check ((Get-FileHash (Join-Path $g1 'dinput8.dll')).Hash -eq (Get-FileHash (Join-Path $pkg 'dinput8.dll')).Hash) 'install copied dinput8.dll'
    Check (Test-Path (Join-Path $g1 'dinput8.dll.coop-receipt.json')) 'install wrote the receipt'
    Check ((Get-Content -Raw (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua')) -match 'coop_init') 'install wrote the loader into the test profile'
    $j = Json-Of (Run-Setup $setup @('status', '--json', $g1))
    Check ($j -and $j.installs[0].owner -eq 'setup') 'status --json after install: setup'
    $r = Run-Setup 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $pkg 'mdk2-coop.ps1'), '-GameDir', $g1, '-StatusOnly', '-Yes')
    $j = Json-Of $r
    Check ($r.code -eq 0 -and $j -and $j.items -and $j.actions -and $j.owner -eq 'setup') "-StatusOnly -Yes still works: $($r.code)"
    $r = Run-Setup $setup @('uninstall', $g1, '--yes')
    Check ($r.code -eq 0 -and -not (Test-Path (Join-Path $g1 'dinput8.dll')) -and -not (Test-Path (Join-Path $g1 'dinput8.dll.coop-receipt.json'))) "uninstall --yes: $($r.code) $($r.text)"
    # usage
    Check ((Run-Setup $setup @('frobnicate')).code -eq 2) 'unknown verb: exit 2'
    Check ((Run-Setup $setup @('install')).code -eq 2) 'install without a folder: exit 2'
    # unsupported build
    $g2 = New-Game "$label-g2" (Join-Path $Fixtures 'unknown-pointer.exe')
    $r = Run-Setup $setup @('install', $g2, '--yes')
    Check ($r.code -eq 3 -and -not (Test-Path (Join-Path $g2 'dinput8.dll'))) "unknown build: exit $($r.code), want 3, nothing written"
    # the game running from that folder (a harmless 32-bit stand-in named mdk2hd.exe, started and stopped here: Setup's
    # PowerShell is 32-bit, as the game is, and reads only 32-bit processes' paths)
    $g3 = New-Game "$label-g3" (Join-Path $env:SystemRoot 'SysWOW64\PING.EXE')
    $running = Start-Process -FilePath (Join-Path $g3 'mdk2hd.exe') -ArgumentList '-n', '60', '127.0.0.1' -WindowStyle Hidden -PassThru
    try {
        Start-Sleep -Milliseconds 500
        $r = Run-Setup $setup @('install', $g3, '--yes', '-AcceptUnknownBuild')
        Check ($r.code -eq 4 -and -not (Test-Path (Join-Path $g3 'dinput8.dll'))) "game running: exit $($r.code), want 4, nothing written"
    } finally { if (-not $running.HasExited) { Stop-Process -Id $running.Id -Force } }
    # a mod manager's install: a plain copy of the release
    $g4 = New-Game "$label-g4" $steam
    New-Item -ItemType Directory (Join-Path $g4 'coop') | Out-Null
    Copy-Item (Join-Path $pkg 'dinput8.dll') $g4; Copy-Item (Join-Path $pkg 'coop\coop.lua') (Join-Path $g4 'coop')
    $before = (Get-FileHash (Join-Path $g4 'dinput8.dll')).Hash
    $j = Json-Of (Run-Setup $setup @('status', '--json', $g4))
    Check ($j -and $j.installs[0].owner -eq 'mod-manager') 'a plain copy of the release: owner mod-manager'
    $r = Run-Setup $setup @('install', $g4, '--yes')
    Check ($r.code -eq 1 -and -not (Test-Path (Join-Path $g4 'dinput8.dll.coop-receipt.json')) -and (Get-FileHash (Join-Path $g4 'dinput8.dll')).Hash -eq $before) "install over a mod manager's files is refused: $($r.code)"
    $gui = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'installer-gui.ps1') -TestOwnerPath $g4) -join ' '
    Check ($gui -match 'button=By mod manager\|enabled=False\|uninstall=False\|play=True\|status=Installed by a mod manager') "Setup window for a mod manager's install: $gui"
    return $script:failures
}

try {
    $bad = @(Run-Suite $PackageDir 'zip')
    if ($bad) { throw ('FAIL (zip Setup): ' + ($bad -join '; ')) }
    Write-Output 'PASS: zip Setup verbs: status --json, install/uninstall --yes, exit codes 2/3/4, -StatusOnly alias, mod-manager install recognised and left alone'
    if ($SingleFile) {
        $single = Join-Path $root 'single'
        New-Item -ItemType Directory $single | Out-Null
        Copy-Item $SingleFile (Join-Path $single 'MDK2-Coop-Setup.exe')
        $g = New-Game 'single-g' (Join-Path $Fixtures 'steam.exe')
        $r = Run-Setup (Join-Path $single 'MDK2-Coop-Setup.exe') @('install', $g, '--yes')
        $j = Json-Of (Run-Setup (Join-Path $single 'MDK2-Coop-Setup.exe') @('status', '--json', $g))
        if ($r.code -ne 0 -or -not $j -or $j.installs[0].owner -ne 'setup') { throw "FAIL (single-file Setup): install $($r.code), status $($j | ConvertTo-Json -Compress -Depth 3)" }
        if ((Run-Setup (Join-Path $single 'MDK2-Coop-Setup.exe') @('uninstall', $g, '--yes')).code -ne 0) { throw 'FAIL (single-file Setup): uninstall' }
        Write-Output 'PASS: single-file Setup runs the verbs from its unpacked payload'
    }
    if ($Mutants) {
        $twins = @(
            @('running-code', "Close the game and run this again.' `$ExitRunning }", "Close the game and run this again.' }"),
            @('build-code', "to install anyway.' `$ExitUnknownBuild }", "to install anyway.' }"),
            @('manager-overwrite', "    if (`$owner -in 'mod-manager', 'vortex') {`r`n        Fail (", "    if (`$false) {`r`n        Fail (")
        )
        foreach ($t in $twins) {
            $m = Join-Path $root ('mutant-' + $t[0])
            Copy-Item $PackageDir $m -Recurse
            $ps = Join-Path $m 'mdk2-coop.ps1'
            $text = [IO.File]::ReadAllText($ps)
            $want = $t[1]; if (-not $text.Contains($want)) { $want = $want.Replace("`r`n", "`n") }
            if (-not $text.Contains($want)) { throw "mutant $($t[0]) does not apply" }
            [IO.File]::WriteAllText($ps, $text.Replace($want, $t[2].Replace("`r`n", $(if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }))))
            $bad = @(Run-Suite $m $t[0])
            if (-not $bad) { throw "FAIL: mutant $($t[0]) survived" }
            Write-Output "KILLED: $($t[0]) ($($bad[0]))"
        }
    }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
