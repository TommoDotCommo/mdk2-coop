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
. (Join-Path $PackageDir 'install-state.ps1')
# Exercise the shipped native bootstrap with a headless script beside it. This
# checks console allocation before WinForms starts without changing any game.
if (Test-Path (Join-Path $PackageDir 'MDK2-Coop-Setup.exe')) {
    $startupDir = Join-Path $root 'setup startup'
    New-Item -ItemType Directory $startupDir | Out-Null
    $setup = Join-Path $startupDir 'MDK2-Coop-Setup.exe'
    Copy-Item (Join-Path $PackageDir 'MDK2-Coop-Setup.exe') $setup
    [IO.File]::WriteAllText((Join-Path $startupDir 'installer-gui.ps1'), @'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class StartupProbe {
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
}
"@
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'console.txt'), [StartupProbe]::GetConsoleWindow().ToInt64().ToString())
'@)
    $process = Start-Process -FilePath $setup -PassThru -Wait
    Check ($process.ExitCode -eq 0) 'native setup starts successfully'
    $resultFile = Join-Path $startupDir 'console.txt'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path $resultFile) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    Check ((Test-Path $resultFile) -and [IO.File]::ReadAllText($resultFile) -eq '0') 'native setup starts PowerShell without a console window'
}
$hostDir = Fixture 'host'
foreach ($mode in 'install', 'uninstall') {
    $completion = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -TestCompletion $mode
    Check ($LASTEXITCODE -eq 0 -and $completion -eq "completion $mode OK") "GUI $mode completion choices"
}
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

# The GUI elevates only game files, then updates the original user's settings.
$splitDir = Fixture 'split'
$probe = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -ProbePath $splitDir
Check ($LASTEXITCODE -eq 0 -and $probe -eq 'writable') 'GUI detects writable game folder'
Check (@(Get-ChildItem -Force $splitDir).Count -eq 1) 'GUI permission probe cleans up its temporary file'
$probe = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -ProbePath (Join-Path $root 'missing')
Check ($LASTEXITCODE -ne 0 -and $probe -eq 'not writable') 'GUI rejects a missing folder'
$protectedDir = Fixture 'protected'
$originalAcl = Get-Acl -LiteralPath $protectedDir
try {
    $restrictedAcl = Get-Acl -LiteralPath $protectedDir
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $deny = New-Object Security.AccessControl.FileSystemAccessRule($identity, 'CreateFiles', 'None', 'None', 'Deny')
    $restrictedAcl.AddAccessRule($deny)
    Set-Acl -LiteralPath $protectedDir -AclObject $restrictedAcl
    $probe = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -ProbePath $protectedDir
    Check ($LASTEXITCODE -ne 0 -and $probe -eq 'not writable') 'GUI detects a protected game folder'
} finally { Set-Acl -LiteralPath $protectedDir -AclObject $originalAcl }
Install $splitDir @('-GameFilesOnly')
Check (Test-Path (Join-Path $splitDir 'dinput8.dll')) 'game-only step installs bridge'
Check (-not (Test-Path (Join-Path $splitDir 'settings.lua'))) 'game-only step does not touch user settings'
Install $splitDir @('-UserSettingsOnly')
Check ([IO.File]::ReadAllText((Join-Path $splitDir 'settings.lua')).Contains('coop_init')) 'user-only step installs loader'
Install $splitDir @('-Uninstall','-GameFilesOnly')
Check (-not (Test-Path (Join-Path $splitDir 'dinput8.dll'))) 'game-only uninstall removes bridge'
Check ([IO.File]::ReadAllText((Join-Path $splitDir 'settings.lua')).Contains('coop_init')) 'game-only uninstall leaves user settings'
Install $splitDir @('-Uninstall','-UserSettingsOnly')
Check (-not [IO.File]::ReadAllText((Join-Path $splitDir 'settings.lua')).Contains('coop_init')) 'user-only uninstall removes loader'

$profileDir = Fixture 'profile'
$dataDir = Join-Path $root 'user-data'
New-Item -ItemType Directory (Join-Path $profileDir 'coop') | Out-Null
Set-Content (Join-Path $profileDir 'coop\coop.ini') 'old settings' -Encoding ASCII
Install $profileDir @('-PrepareDataOnly','-PerUserData','-DataPath',$dataDir)
Check ((Get-Content (Join-Path $dataDir 'coop.ini') -Raw) -match 'old settings') 'preflight prepares user data before game files'
Check (-not (Test-Path (Join-Path $profileDir 'dinput8.dll'))) 'preflight leaves game files untouched'
Install $profileDir @('-GameFilesOnly','-PerUserData')
Check (Test-Path (Join-Path $profileDir 'coop\per-user-data')) 'GUI install marks per-user data'
Install $profileDir @('-UserSettingsOnly','-PerUserData','-DataPath',$dataDir)
Check ((Get-Content (Join-Path $dataDir 'coop.ini') -Raw) -match 'old settings') 'user data migrates from game folder'
Check (-not (Test-Path (Join-Path $dataDir 'coop.lua'))) 'installed script is not copied into user data'
Set-Content (Join-Path $dataDir 'coop.ini') 'new settings' -Encoding ASCII
Install $profileDir @('-UserSettingsOnly','-PerUserData','-DataPath',$dataDir)
Check ((Get-Content (Join-Path $dataDir 'coop.ini') -Raw) -match 'new settings') 'reinstall preserves user data'
Install $profileDir @('-Transport','udp','-Role','host','-HostPort','28950','-DataPath',$dataDir)
Check ((Get-Content (Join-Path $dataDir 'coop.ini') -Raw) -match 'listen_port=28950') 'command-line configuration updates the selected user data'
Install $profileDir @('-Uninstall','-GameFilesOnly','-PerUserData')
Check (-not (Test-Path (Join-Path $profileDir 'coop\per-user-data'))) 'uninstall removes per-user marker'
Check (Test-Path (Join-Path $dataDir 'coop.ini')) 'uninstall preserves user data'

# Load the actual DLL in a harmless fixture executable with an isolated user profile.
if (Test-Path (Join-Path $PackageDir 'data_loader_test.exe')) {
$loadDir = Fixture 'bridge-data'
Copy-Item (Join-Path $PackageDir 'data_loader_test.exe') (Join-Path $loadDir 'data_loader_test.exe')
Copy-Item (Join-Path $PackageDir 'dinput8.dll') (Join-Path $loadDir 'dinput8.dll')
New-Item -ItemType Directory (Join-Path $loadDir 'coop') | Out-Null
$priorAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $root 'fake-profile'
    New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
    & (Join-Path $loadDir 'data_loader_test.exe')
    Check ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $loadDir 'coop\bridge.log'))) 'bridge keeps per-copy data for test and couch copies'
    New-Item -ItemType File (Join-Path $loadDir 'coop\per-user-data') | Out-Null
    & (Join-Path $loadDir 'data_loader_test.exe')
    $userLog = Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop\bridge.log'
    Check ($LASTEXITCODE -eq 0 -and (Test-Path $userLog)) 'bridge writes player logs in the user profile'
    Check ((Get-Content $userLog -Raw) -match 'files in .*mdk2-coop\\coop') 'bridge uses profile for runtime settings'
    & (Join-Path $loadDir 'data_loader_test.exe')
    Check (Test-Path (Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop\bridge.previous.log')) 'profile log rotates on restart'
} finally { $env:LOCALAPPDATA = $priorAppData }
} else { Write-Output 'SKIP: bridge data loader fixture not supplied' }

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

# Regression: an unrelated Lua payload has the same restoration guarantees as a DLL.
$restoreDir = Fixture 'restore-originals'
New-Item -ItemType Directory (Join-Path $restoreDir 'coop') | Out-Null
[IO.File]::WriteAllText((Join-Path $restoreDir 'dinput8.dll'), 'foreign proxy')
[IO.File]::WriteAllText((Join-Path $restoreDir 'coop\coop.lua'), 'foreign script')
[IO.File]::WriteAllText((Join-Path $restoreDir 'coop\save.dat'), 'save sentinel')
Install $restoreDir @()
Install $restoreDir @()
[IO.File]::AppendAllText((Join-Path $restoreDir 'settings.lua'), "`r`nnew_option = 7`r`n")
Install $restoreDir @('-Uninstall')
Check ([IO.File]::ReadAllText((Join-Path $restoreDir 'dinput8.dll')) -eq 'foreign proxy') 'DLL original restored exactly'
Check ([IO.File]::ReadAllText((Join-Path $restoreDir 'coop\coop.lua')) -eq 'foreign script') 'Lua original restored exactly'
Check ([IO.File]::ReadAllText((Join-Path $restoreDir 'coop\save.dat')) -eq 'save sentinel') 'save unchanged'
Check ([IO.File]::ReadAllText((Join-Path $restoreDir 'settings.lua')).Contains('new_option = 7')) 'new player options survive uninstall'
# Exercise CLI Update/Repair decisions against actual receipts and payload bytes.
$maintDir = Fixture 'maintenance'
Install $maintDir @()
$dll = Join-Path $maintDir 'dinput8.dll'; $lua = Join-Path $maintDir 'coop\coop.lua'
function Status($dir) {
    $json = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'mdk2-coop.ps1') -GameDir $dir -SettingsPath (Join-Path $dir 'settings.lua') -StatusOnly -Yes
    if ($LASTEXITCODE -ne 0) { throw 'Status command failed' }
    return ($json | ConvertFrom-Json)
}
Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('previous release fixture'))
Check ((Status $maintDir).actions.install -eq 'Update') 'CLI detects older owned payload'
Install $maintDir @()
Check ((Get-CoopHash $dll) -eq (Get-CoopHash (Join-Path $PackageDir 'dinput8.dll'))) 'CLI update deploys current payload'
Remove-Item $lua
Check ((Status $maintDir).actions.install -eq 'Repair') 'CLI detects missing managed payload'
Install $maintDir @()
Check ((Get-CoopHash $lua) -eq (Get-CoopHash (Join-Path $PackageDir 'coop\coop.lua'))) 'CLI repair restores missing payload'
[IO.File]::WriteAllText($dll, 'user modification')
Check ((Status $maintDir).actions.blocked) 'CLI status blocks modified payload'
Install $maintDir @() 1
Check ([IO.File]::ReadAllText($dll) -eq 'user modification') 'CLI refuses modified replacement'
# An unowned payload is never removed by uninstall.
$unownedDir = Fixture 'unowned'
[IO.File]::WriteAllText((Join-Path $unownedDir 'dinput8.dll'), 'unowned proxy')
Install $unownedDir @('-Uninstall')
Check ([IO.File]::ReadAllText((Join-Path $unownedDir 'dinput8.dll')) -eq 'unowned proxy') 'CLI uninstall preserves unowned payload'
$completion = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -TestLayout
Check ($LASTEXITCODE -eq 0) 'setup layout geometry'
Write-Output "PASS: $script:checks installer checks (including original restoration and layout)"

$actions = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PackageDir 'installer-gui.ps1') -TestActionsPath $maintDir
Check ($LASTEXITCODE -eq 0 -and $actions -match 'PASS: existing game actions') 'existing game play and shortcut actions'
Write-Output "PASS: $script:checks installer checks (including play and desktop shortcut)"
