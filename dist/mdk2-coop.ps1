# mdk2-coop installer for MDK2 HD (Steam or the Overhaul Games build). Windows PowerShell 5.1, no other tools.
# Run by MDK2-Coop-Setup.exe (the player-facing installer); the command line below is for scripts and tests:
#   powershell -ExecutionPolicy Bypass -File mdk2-coop.ps1 [-Uninstall] [-GameDir <folder>]
#              [-Transport steam|udp] [-Role host|join] [-LobbyId <id>]
#              [-HostAddress <ip>] [-HostPort 27900] [-ClientPort 27901] [-Yes]
# What it changes: <game>\dinput8.dll, <game>\coop\coop.lua, <game>\coop\coop.ini ([net] keys and [launcher] mode)
# and one loader block in %LOCALAPPDATA%\MDK2HD\settings.lua (backed up first as settings.lua.coop-backup).
param(
    [switch]$Uninstall,
    [switch]$StatusOnly, # read-only JSON inventory; no game or user state changes
    [string]$GameDir,
    [ValidateSet('', 'host', 'join')][string]$Role = '',
    [ValidateSet('', 'steam', 'udp')][string]$Transport = '',
    [string]$LobbyId,
    [string]$HostAddress,
    [int]$HostPort = 0,
    [int]$ClientPort = 0,
    [string]$SettingsPath = (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua'),
    [switch]$GameFilesOnly, # GUI elevation helper: never touches the administrator's profile
    [switch]$UserSettingsOnly, # GUI: run in the original user's session
    [switch]$PrepareDataOnly, # preflight the original user's data before game-file changes
    [switch]$PerUserData,    # GUI installs use writable per-user runtime data
    [string]$DataPath = (Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop'),
    [switch]$FindOnly,      # print the game folder that would be used and stop
    [switch]$Yes            # no questions: defaults for anything not given
)
$ErrorActionPreference = 'Stop'
$AppId       = '213350'
$LoaderBegin = '-- >>> mdk2-coop >>>'
$LoaderEnd   = '-- <<< mdk2-coop <<<'
$LoaderBlock = "$LoaderBegin`r`nif (coop_init) then coop_init() end`r`n$LoaderEnd`r`n"
$SettingsMax = 8000      # the game reads settings.lua into an 8 KB buffer and crashes when it is larger
$Here        = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $Here 'install-state.ps1')

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }
function Fail($msg) { Say "`n$msg" Red; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit 1 }
function Ask($question, $default) {
    if ($Yes) { return $default }
    $a = Read-Host "$question [$default]"
    if ([string]::IsNullOrWhiteSpace($a)) { return $default } else { return $a.Trim() }
}

function Ask-Port($question, $default) {
    while ($true) {
        $a = Ask $question $default
        $n = 0
        if ([int]::TryParse("$a", [ref]$n) -and $n -ge 1024 -and $n -le 65535) { return $n }
        if ($Yes) { Fail "Not a port number: $a" }
        Say 'A port is a number from 1024 to 65535.' Yellow
    }
}

# ---- finding the game ----

function Steam-Libraries {
    $roots = @()
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam', 'HKCU:\Software\Valve\Steam') {
        $p = Get-ItemProperty $k -ErrorAction SilentlyContinue
        if ($p.SteamPath)   { $roots += ($p.SteamPath -replace '/', '\') }
        if ($p.InstallPath) { $roots += $p.InstallPath }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    $libs | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') } | Select-Object -Unique
}

function Find-Game {
    foreach ($lib in Steam-Libraries) {
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        $dirs = @()
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $dirs += Join-Path $lib "steamapps\common\$($m.Groups[1].Value)" }
        }
        $dirs += Join-Path $lib 'steamapps\common\MDK 2 HD'
        foreach ($d in $dirs) { if (Test-Path (Join-Path $d 'mdk2hd.exe')) { return $d } }
    }
    # the Overhaul Games (Beamdog client) build installs here by default
    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $root) { continue }
        $d = Join-Path $root 'Overhaul Games\MDK 2 HD'
        if (Test-Path (Join-Path $d 'mdk2hd.exe')) { return $d }
    }
    return $null
}

function Resolve-GameDir {
    $d = $GameDir
    if (-not $d) { $d = Find-Game }
    if (-not $d) {
        Say 'MDK2 HD was not found in your Steam libraries or the Overhaul Games folder.' Yellow
        if ($Yes) { Fail 'Give the game folder with -GameDir.' }
        $d = Read-Host 'Paste the MDK2 HD folder (the one with mdk2hd.exe)'
    }
    $d = $d.Trim('"', ' ')
    if (-not (Test-Path (Join-Path $d 'mdk2hd.exe'))) { Fail "No mdk2hd.exe in '$d'." }
    return (Resolve-Path $d).ProviderPath
}

function Check-NotRunning($dir) {
    $running = Get-Process mdk2hd -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dir, 'OrdinalIgnoreCase') }
    if ($running) { Fail 'MDK2 HD is running from that folder. Close the game and run this again.' }
}

# ---- coop.ini: only the [net] keys, everything else in the file is kept ----

Add-Type -Namespace Mdk2Coop -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section, string key, string def, System.Text.StringBuilder ret, uint size, string file);
'@

function Ini-Get($file, $section, $key, $default) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][Mdk2Coop.Ini]::GetPrivateProfileString($section, $key, $default, $sb, 256, $file)
    return $sb.ToString()
}
function Ini-Set($file, $section, $key, $value) {
    if (-not [Mdk2Coop.Ini]::WritePrivateProfileString($section, $key, "$value", $file)) { Fail "Could not write $file" }
}

# ---- settings.lua loader block ----

function Remove-Loader($text) {
    return [regex]::Replace($text, "(?m)^" + [regex]::Escape($LoaderBegin) + "[\s\S]*?" + [regex]::Escape($LoaderEnd) + "\r?\n?", '')
}

function Add-Loader {
    Assert-CoopRegularPath $SettingsPath
    Assert-CoopRegularPath "$SettingsPath.coop-backup"
    $dir = Split-Path -Parent $SettingsPath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    if (Test-Path $SettingsPath) {
        $backup = "$SettingsPath.coop-backup"
        if (-not (Test-Path $backup)) { Copy-Item $SettingsPath $backup; Say "  backed up settings.lua to $(Split-Path -Leaf $backup)" }
        $existing = [IO.File]::ReadAllText($SettingsPath)
        if ((Get-CoopLoaderState $existing) -eq 'conflict') { throw 'The loader block was modified; settings were preserved.' }
        $text = Remove-Loader $existing
        if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $text += "`r`n" }
    } else {
        $text = ''     # the game has never saved its settings: it starts fine with only the loader
        # Keep an empty original so a later reinstall never backs up our loader as
        # if it were the player's original settings.
        $backup = "$SettingsPath.coop-backup"
        if (-not (Test-Path $backup)) { [IO.File]::WriteAllText($backup, '', (New-Object Text.ASCIIEncoding)) }
    }
    $text += $LoaderBlock
    if ($text.Length -gt $SettingsMax) { Fail "settings.lua would be $($text.Length) bytes; the game crashes above about 8 KB." }
    [IO.File]::WriteAllText($SettingsPath, $text, (New-Object Text.ASCIIEncoding))
    Say "  loader line in $SettingsPath"
}

# ---- the bridge DLL ----

function Is-OurDll($path) {
    if (-not (Test-Path $path)) { return $false }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains('mdk2-coop bridge')
}

# ---- install / uninstall ----

function Do-Install {
    Say "`nmdk2-coop installer" Cyan
    $game = Resolve-GameDir
    Say "Game folder: $game"
    if ($FindOnly) { return }
    if ($PrepareDataOnly) { Prepare-UserData $game; return }
    if ($UserSettingsOnly) {
        if ($PerUserData) { Prepare-UserData $game }
        Add-Loader; return
    }
    Check-NotRunning $game
    foreach ($f in 'dinput8.dll', 'coop\coop.lua') {
        if (-not (Test-Path (Join-Path $Here $f))) { Fail "$f is missing next to this script. Unpack the whole zip first." }
    }

    $ini = Join-Path $game 'coop\coop.ini'
    if (-not $GameFilesOnly -and (Test-Path (Join-Path $game 'coop\per-user-data'))) {
        $ini = Join-Path $DataPath 'coop.ini'
    }
    $hadIni = Test-Path $ini
    # Host / join, the network and the addresses are chosen in the launcher at every start; these questions are only
    # asked when a network option was given on the command line (scripts, tests).
    $setNet = $Transport -or $Role -or $LobbyId -or $HostAddress -or $HostPort -or $ClientPort
    if ($setNet) {
        $curRole = if ($hadIni) { Ini-Get $ini 'net' 'role' 'host' } else { 'host' }
        $network = $Transport
        if (-not $network) {
            $def = if ($HostAddress -or $HostPort -or $ClientPort) { 'udp' } elseif ($hadIni) { Ini-Get $ini 'net' 'transport' 'udp' } else { 'steam' }
            $network = Ask 'Network: steam (friends and invites, no port forwarding) / udp (direct IP)' $def
        }
        $network = "$network".ToLower()
        if ($network -notin 'steam', 'udp') { Fail 'Network must be steam or udp.' }
        $role = $Role
        if (-not $role) {
            $def = if ($curRole -eq 'client') { 'join' } else { 'host' }
            $role = Ask 'Host a game or join one? (host / join)' $def
        }
        $role = "$role".ToLower(); if ($role -eq 'client') { $role = 'join' }
        if ($role -notin 'host', 'join') { Fail "Answer host or join (got '$role')." }
        if ($network -eq 'steam') {
            if ($LobbyId) {
                $numericLobby = [UInt64]0
                if ($LobbyId -notmatch '^\d+$' -or -not [UInt64]::TryParse($LobbyId, [ref]$numericLobby) -or
                    ($numericLobby -shr 56) -ne 1 -or (($numericLobby -shr 52) -band 15) -ne 8) {
                    Fail 'LobbyId must be a numeric Steam lobby ID from the host.'
                }
            }
        } else {
            if ($role -eq 'join') {
                $addr = $HostAddress
                if (-not $addr) { $addr = Ask "Host's IPv4 address" (Ini-Get $ini 'net' 'peer_host' '127.0.0.1') }
            }
            # the host's port: a host listens on it, a client sends to it
            $knownHostPort = if ($curRole -eq 'client') { Ini-Get $ini 'net' 'peer_port' '27900' } else { Ini-Get $ini 'net' 'listen_port' '27900' }
            $hp = if ($HostPort) { $HostPort } else { Ask-Port 'Host UDP port' $knownHostPort }
            if ($role -eq 'join') {
                $knownOwn = if ($curRole -eq 'client') { Ini-Get $ini 'net' 'listen_port' "$($hp + 1)" } else { "$($hp + 1)" }
                $cp = if ($ClientPort) { $ClientPort } else { Ask-Port 'Your own UDP port (any free one; only matters with several players on one PC)' $knownOwn }
            }
        }

    }

    Say "`nInstalling into $game"
    $dll = Join-Path $game 'dinput8.dll'
    Install-CoopFile $dll ([IO.File]::ReadAllBytes((Join-Path $Here 'dinput8.dll')))
    Install-CoopFile (Join-Path $game 'coop\coop.lua') ([IO.File]::ReadAllBytes((Join-Path $Here 'coop\coop.lua')))
    if ($PerUserData) { Install-CoopFile (Join-Path $game 'coop\per-user-data') ([byte[]]@()) }
    Say '  dinput8.dll and coop\coop.lua copied'
    if ($setNet) {
        Ini-Set $ini 'net' 'transport' $network
        Ini-Set $ini 'net' 'role' $(if ($role -eq 'host') { 'host' } else { 'client' })
        Ini-Set $ini 'launcher' 'mode' $role     # the launcher shows this choice (not a solo start from before)
        if ($network -eq 'steam') {
            Ini-Set $ini 'net' 'lobby_id' "$LobbyId"
        } elseif ($role -eq 'host') {
            Ini-Set $ini 'net' 'listen_port' $hp
        } else {
            Ini-Set $ini 'net' 'listen_port' $cp
            Ini-Set $ini 'net' 'peer_host' $addr; Ini-Set $ini 'net' 'peer_port' $hp
        }
        Say "  coop\coop.ini: $network, $role"
    }
    if (-not $GameFilesOnly) { Add-Loader }

    Say "`nDone. Start MDK2 HD as usual: the co-op launcher opens first. Pick Host or Join there (Steam lobbies or" Green
    Say 'a direct IP address and port) and press Start. Everything else is in its Settings tab.' Green
    if ($setNet) { Say "Chosen here: $network, $role. The launcher can change it at any start." Gray }
    if (-not $Yes) { Read-Host "`nPress Enter to close" | Out-Null }
}

function Do-Uninstall {
    Say "`nmdk2-coop uninstall" Cyan
    $game = Resolve-GameDir
    Say "Game folder: $game"
    if ($FindOnly) { return }
    if ($UserSettingsOnly) { Remove-UserLoader; return }
    Check-NotRunning $game
    $dll = Join-Path $game 'dinput8.dll'
    foreach ($target in @($dll, (Join-Path $game 'coop\coop.lua'), (Join-Path $game 'coop\per-user-data'))) {
        if (Remove-CoopFile $target) { Say "  restored/removed $target" }
        elseif (Test-Path -LiteralPath $target) { Say "  no ownership receipt for $target: kept it; install this package first to establish ownership" Yellow }
    }

    if (-not $GameFilesOnly) { Remove-UserLoader }
    Say "`nCo-op is off. The coop folder (settings, logs, co-op saves) is still in the game folder; delete it if you like." Green
    if (-not $Yes) { Read-Host "`nPress Enter to close" | Out-Null }
}

function Prepare-UserData($game) {
    New-Item -ItemType Directory -Force -Path $DataPath | Out-Null
    # Check actual writes before changing the loader or claiming success.
    $probe = Join-Path $DataPath ([IO.Path]::GetRandomFileName())
    $stream = [IO.File]::Open($probe, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    $stream.Dispose(); [IO.File]::Delete($probe)
    # Preserve existing player data from older writable installs. Never replace data
    # already in the user folder, and do not copy the installed Lua script.
    $old = Join-Path $game 'coop'
    if (Test-Path $old) {
        Get-ChildItem -LiteralPath $old -File | Where-Object { $_.Name -notin 'coop.lua', 'per-user-data' -and $_.Name -notmatch '\.coop-(original|receipt|stage)'  } | ForEach-Object {
            $target = Join-Path $DataPath $_.Name
            if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $_.FullName -Destination $target }
        }
    }
}

function Remove-UserLoader {
    Assert-CoopRegularPath $SettingsPath
    # The game may have changed unrelated options since install. Remove only our
    # exact delimited block; never restore an old settings snapshot over new options.
    if (Test-Path -LiteralPath $SettingsPath) {
        $text = [IO.File]::ReadAllText($SettingsPath)
        if ((Get-CoopLoaderState $text) -eq 'conflict') { throw 'The loader block was modified; settings were preserved.' }
        [IO.File]::WriteAllText($SettingsPath, (Remove-Loader $text), (New-Object Text.ASCIIEncoding))
        Say '  loader block removed; current player options preserved'
    }
}

try {
    if ($StatusOnly) {
        Get-CoopInstallStatus $GameDir $Here $SettingsPath $DataPath | ConvertTo-Json -Depth 6
    } elseif ($Uninstall) { Do-Uninstall } else { Do-Install }
} catch {
    Fail "Something went wrong: $($_.Exception.Message)"
}
