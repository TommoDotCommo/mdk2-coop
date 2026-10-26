# Install into disposable fixtures only. The source game executable is read, never run or changed.
param(
    [Parameter(Mandatory=$true)][string]$GameExe,
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [string]$TestRoot
)
$ErrorActionPreference = 'Stop'
$base = if ($TestRoot) { $TestRoot } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'local\tests' }
$root = Join-Path $base "mdk2-coop-installer-$PID"
New-Item -ItemType Directory -Force $root | Out-Null
$script:checks = 0
function Check($condition, $description) {
    if (-not $condition) { throw "FAIL: $description" }
    $script:checks++
}
function Fixture($name) {
    $dir = Join-Path $root $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory $dir | Out-Null
    Copy-Item $GameExe (Join-Path $dir 'mdk2hd.exe')
    return $dir
}
function Install($dir, [string[]]$extra, [int]$expected=0) {
    $settings = Join-Path $dir 'settings.lua'
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'mdk2-coop.ps1') `
        -GameDir $dir -SettingsPath $settings -Yes @extra 2>&1
    if ($LASTEXITCODE -ne $expected) { throw "Installer exit $LASTEXITCODE, expected ${expected}: $output" }
}
$hostDir = Fixture 'host'
Install $hostDir @()
Check (-not (Test-Path (Join-Path $hostDir 'coop\coop.ini'))) 'a plain install asks nothing and leaves host/join to the launcher'
Check (Test-Path (Join-Path $hostDir 'dinput8.dll')) 'bridge installed'
Check (Test-Path (Join-Path $hostDir 'coop\coop.lua')) 'Lua installed'
Check ((Get-Content (Join-Path $hostDir 'settings.lua') -Raw).Contains('if (coop_init)')) 'loader installed in fixture'
Check (-not (Test-Path (Join-Path $hostDir 'steam_api.dll'))) 'no Steam DLL replacement'
Check (-not (Test-Path (Join-Path $hostDir 'steam_appid.txt'))) 'no AppID override'
Install $hostDir @('-Role','join','-LobbyId','109775241000000001')
$ini = Get-Content (Join-Path $hostDir 'coop\coop.ini') -Raw
Check ($ini -match 'role=client' -and $ini -match 'lobby_id=109775241000000001') 'Steam join and lobby ID saved'
Install $hostDir @('-Role','join')
$ini = Get-Content (Join-Path $hostDir 'coop\coop.ini') -Raw
Check ($ini -notmatch 'lobby_id=109775241000000001') 'invite mode clears stale lobby ID'

$udpDir = Fixture 'udp'
Install $udpDir @('-Transport','udp','-Role','join','-HostAddress','127.0.0.1','-HostPort','28950','-ClientPort','28951')
$ini = Get-Content (Join-Path $udpDir 'coop\coop.ini') -Raw
Check ($ini -match 'transport=udp' -and $ini -match 'role=client') 'explicit UDP join'
Check ($ini -match 'peer_host=127.0.0.1' -and $ini -match 'peer_port=28950' -and $ini -match 'listen_port=28951') 'UDP ports/address retained'

$legacyDir = Fixture 'legacy'
New-Item -ItemType Directory (Join-Path $legacyDir 'coop') | Out-Null
Set-Content (Join-Path $legacyDir 'coop\coop.ini') "[net]`r`nrole=host`r`nlisten_port=28952`r`n[game]`r`nenemy_scale=75" -Encoding ASCII
Install $legacyDir @()
$ini = Get-Content (Join-Path $legacyDir 'coop\coop.ini') -Raw
Check ($ini -notmatch 'transport=' -and $ini -match 'listen_port=28952') 'a plain install leaves an existing configuration alone'
Check ($ini -match 'enemy_scale=75') 'unrelated settings preserved'
Install $legacyDir @('-Transport','steam')
$ini = Get-Content (Join-Path $legacyDir 'coop\coop.ini') -Raw
Check ($ini -match 'transport=steam' -and $ini -match 'enemy_scale=75') 'switching transport preserves gameplay settings'

$badDir = Fixture 'invalid'
Install $badDir @('-Transport','steam','-Role','join','-LobbyId','18446744073709551616') 1
Check (-not (Test-Path (Join-Path $badDir 'dinput8.dll'))) 'invalid lobby rejected before install'
Check (-not (Test-Path (Join-Path $badDir 'settings.lua'))) 'invalid lobby leaves settings untouched'
Install $badDir @('-Transport','steam','-Role','join','-LobbyId','76561198000000001') 1
Check (-not (Test-Path (Join-Path $badDir 'dinput8.dll'))) 'user SteamID is not accepted as a lobby'

Install $hostDir @('-Uninstall')
Check (-not (Test-Path (Join-Path $hostDir 'dinput8.dll'))) 'uninstall removes bridge'
Check (-not (Test-Path (Join-Path $hostDir 'coop\coop.lua'))) 'uninstall removes Lua'
Check (-not [IO.File]::ReadAllText((Join-Path $hostDir 'settings.lua')).Contains('coop_init')) 'uninstall removes loader'
Check (Test-Path (Join-Path $hostDir 'coop\coop.ini')) 'uninstall preserves co-op settings'
# Exercise backups produced by older installers after a first-run reinstall.
Set-Content (Join-Path $hostDir 'settings.lua') "sentinel = 42" -Encoding ASCII
Install $hostDir @('-Transport','steam')
Copy-Item (Join-Path $hostDir 'settings.lua') (Join-Path $hostDir 'settings.lua.coop-backup') -Force
Install $hostDir @('-Uninstall')
$restored = [IO.File]::ReadAllText((Join-Path $hostDir 'settings.lua'))
Check ($restored.Contains('sentinel = 42')) 'restored player options preserved'
Check (-not $restored.Contains('coop_init')) 'old backups containing loader are cleaned on uninstall'
Write-Output "PASS: $script:checks installer checks (disposable fixtures only)"
