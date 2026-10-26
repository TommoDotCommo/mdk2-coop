# mdk2-coop couch play: 2-4 players on one PC and one screen, one gamepad each. Windows PowerShell 5.1.
# Double-click Couch.cmd, or:
#   powershell -ExecutionPolicy Bypass -File mdk2-couch.ps1 [-Players 2..4] [-Monitor <n>] [-GameDir <folder>]
#              [-Port 27960] [-Look 700] [-Stop] [-NoLaunch] [-Yes]
# mdk2-coop must be installed first (Install.cmd). This makes one game copy per player next to the game
# folder ("MDK 2 HD couch\p1" .. "p4"; mdk2.tar is hard-linked, so a copy costs about 2 MB), tiles one
# borderless window per player on the chosen monitor and starts them: player 1 hosts, the others join
# over localhost. Your own game folder and coop.ini are not touched.
# Changes outside the couch folder: the loader block in %LOCALAPPDATA%\MDK2HD\settings.lua if it is not there.
param(
    [ValidateRange(2, 4)][int]$Players = 0,
    [int]$Monitor = 0,              # 1 = first monitor in Windows' order; default: the primary one
    [string]$GameDir,
    [int]$Port = 27960,             # player 1 listens here, the others on the next ports
    [int]$Look = 700,               # right stick look speed (mouse counts per second at full tilt)
    [switch]$Stop,                  # close the couch games
    [switch]$NoLaunch,              # set everything up, start nothing
    [string]$SettingsPath = (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua'),
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$Builds      = @(
    'BF5FAA81B18837B4C07F35941B107E5C23CAA8B105536CCD9C8E5A8E37357F43'      # Steam build 3902
    '2E4AE928CDC5D39FEBF1D2AA2AB764B42353028DD19838121CF25ED284ED39F9'      # Overhaul Games build
)
$AppId       = '213350'
$LoaderBegin = '-- >>> mdk2-coop >>>'
$LoaderEnd   = '-- <<< mdk2-coop <<<'
$LoaderBlock = "$LoaderBegin`r`nif (coop_init) then coop_init() end`r`n$LoaderEnd`r`n"
$SettingsMax = 8000

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }
function Fail($msg) { Say "`n$msg" Red; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit 1 }

Add-Type -Namespace Mdk2Coop -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
'@
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace Mdk2Coop -Name Dpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
[void][Mdk2Coop.Dpi]::SetProcessDPIAware()      # screen sizes in real pixels

function Ini-Set($file, $section, $key, $value) {
    if (-not [Mdk2Coop.Ini]::WritePrivateProfileString($section, $key, "$value", $file)) { Fail "Could not write $file" }
}

# ---- finding the game (same search as the installer) ----

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

# ---- layout: the biggest 16:9 window per player that fits, centred in its share of the monitor ----

function Get-Layout($screen, $count) {
    $b = $screen.Bounds
    if ($count -eq 2) { $cols = 1; $rows = 2 } else { $cols = 2; $rows = 2 }
    $cw = [int][math]::Floor($b.Width / $cols)
    $ch = [int][math]::Floor($b.Height / $rows)
    $h = [math]::Min($ch, [int][math]::Floor($cw * 9 / 16))
    $w = [int][math]::Floor($h * 16 / 9)
    $w -= $w % 2; $h -= $h % 2
    $tiles = @()
    for ($i = 0; $i -lt $count; $i++) {
        $c = $i % $cols; $r = [int][math]::Floor($i / $cols)
        $tiles += [pscustomobject]@{
            X = $b.X + $c * $cw + [int][math]::Floor(($cw - $w) / 2)
            Y = $b.Y + $r * $ch + [int][math]::Floor(($ch - $h) / 2)
            W = $w; H = $h
        }
    }
    return $tiles
}

function Couch-Processes($root) {
    Get-Process mdk2hd -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root, 'OrdinalIgnoreCase') }
}

# ---- one game copy ----

function Link-Or-Copy($src, $dst) {
    if (Test-Path $dst) { Remove-Item $dst -Force }
    try { New-Item -ItemType HardLink -Path $dst -Target $src | Out-Null; return $true }
    catch { Copy-Item $src $dst; return $false }
}

function Make-Copy($game, $dir) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'coop') | Out-Null
    $linked = Link-Or-Copy (Join-Path $game 'mdk2.tar') (Join-Path $dir 'mdk2.tar')
    foreach ($f in 'mdk2hd.exe', 'steam_api.dll', 'dinput8.dll') {
        Copy-Item (Join-Path $game $f) (Join-Path $dir $f) -Force
    }
    Copy-Item (Join-Path $game 'coop\coop.lua') (Join-Path $dir 'coop\coop.lua') -Force
    Set-Content -Path (Join-Path $dir 'steam_appid.txt') -Value $AppId -Encoding ASCII
    return $linked
}

function Ensure-Loader {
    $text = if (Test-Path $SettingsPath) { [IO.File]::ReadAllText($SettingsPath) } else { '' }
    if ($text.Contains($LoaderBegin)) { return }
    if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $text += "`r`n" }
    $text += $LoaderBlock
    if ($text.Length -gt $SettingsMax) { Fail "settings.lua would be $($text.Length) bytes; the game crashes above about 8 KB." }
    $dir = Split-Path -Parent $SettingsPath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    [IO.File]::WriteAllText($SettingsPath, $text, (New-Object Text.ASCIIEncoding))
    Say "  loader line added to $SettingsPath"
}

function Do-Couch {
    Say "`nmdk2-coop couch play" Cyan
    $game = $GameDir
    if (-not $game) { $game = Find-Game }
    if (-not $game) { Fail 'MDK2 HD was not found in your Steam libraries or the Overhaul Games folder. Give the game folder with -GameDir.' }
    $game = (Resolve-Path $game.Trim('"', ' ')).ProviderPath.TrimEnd('\')
    $root = "$game couch"
    if ($Stop) {
        $p = Couch-Processes $root
        if ($p) { $p | Stop-Process -Force; Say "Closed $(@($p).Count) couch game(s)." Green } else { Say 'No couch games are running.' }
        return
    }
    if (-not (Test-Path (Join-Path $game 'mdk2hd.exe'))) { Fail "No mdk2hd.exe in '$game'." }
    $h = (Get-FileHash -Algorithm SHA256 (Join-Path $game 'mdk2hd.exe')).Hash
    if ($Builds -notcontains $h) { Fail "This mdk2hd.exe is not a build the mod is made for (Steam build 3902 or the Overhaul Games build).`nSHA-256: $h" }
    foreach ($f in 'dinput8.dll', 'coop\coop.lua') {
        if (-not (Test-Path (Join-Path $game $f))) { Fail "$f is missing in the game folder. Run Install.cmd first." }
    }
    if (Couch-Processes $root) { Fail 'The couch games are already running. Run Couch.cmd -Stop (or close them) first.' }

    $n = $Players
    if (-not $n) {
        $a = if ($Yes) { '2' } else { Read-Host 'How many players on this screen? (2-4) [2]' }
        if ([string]::IsNullOrWhiteSpace($a)) { $a = '2' }
        if (-not [int]::TryParse($a, [ref]$n) -or $n -lt 2 -or $n -gt 4) { Fail 'Choose 2, 3 or 4 players.' }
    }
    $screens = [System.Windows.Forms.Screen]::AllScreens
    if ($Monitor -ge 1 -and $Monitor -le $screens.Count) { $screen = $screens[$Monitor - 1] }
    else { $screen = [System.Windows.Forms.Screen]::PrimaryScreen }
    $tiles = Get-Layout $screen $n
    Say "Game folder: $game"
    Say "$n players on $($screen.DeviceName) ($($screen.Bounds.Width)x$($screen.Bounds.Height)), windows $($tiles[0].W)x$($tiles[0].H)"
    Ensure-Loader

    for ($i = 1; $i -le $n; $i++) {
        $dir = Join-Path $root "p$i"
        $linked = Make-Copy $game $dir
        if (-not $linked) { Say "  p$i`: could not hard-link mdk2.tar (different drive?), copied it instead" Yellow }
        $ini = Join-Path $dir 'coop\coop.ini'
        Ini-Set $ini 'net' 'transport' 'udp'
        if ($i -eq 1) {
            Ini-Set $ini 'net' 'role' 'host'; Ini-Set $ini 'net' 'listen_port' $Port
        } else {
            Ini-Set $ini 'net' 'role' 'client'; Ini-Set $ini 'net' 'listen_port' ($Port + $i - 1)
            Ini-Set $ini 'net' 'peer_host' '127.0.0.1'; Ini-Set $ini 'net' 'peer_port' $Port
        }
        Ini-Set $ini 'controls' 'devkeys' 0
        Ini-Set $ini 'controls' 'preset' 'modern'
        $t = $tiles[$i - 1]
        Ini-Set $ini 'couch' 'slot' ($i - 1); Ini-Set $ini 'couch' 'count' $n; Ini-Set $ini 'couch' 'pad' ($i - 1)
        Ini-Set $ini 'couch' 'x' $t.X; Ini-Set $ini 'couch' 'y' $t.Y; Ini-Set $ini 'couch' 'w' $t.W; Ini-Set $ini 'couch' 'h' $t.H
        Ini-Set $ini 'couch' 'look' $Look
        $pini = Join-Path $dir 'coop\player.ini'
        if (-not (Test-Path $pini) -or -not ((Get-Content $pini -Raw) -match 'name=')) { Ini-Set $pini 'player' 'name' "Player $i" }
    }
    Say "Game copies ready in $root" Green
    if ($NoLaunch) { return }

    for ($i = 1; $i -le $n; $i++) {
        $dir = Join-Path $root "p$i"
        Start-Process -FilePath (Join-Path $dir 'mdk2hd.exe') -WorkingDirectory $dir | Out-Null
        Say "  started player $i (gamepad $i)"
        Start-Sleep -Seconds 3      # the host first, so the others find it
    }
    Say "`nPlayer 1 starts the game from the title screen (A = select, B / Start = back or pause);" Green
    Say 'the others follow into the level by themselves. Gamepad: left stick move, right stick look,'
    Say 'right trigger fire, left trigger sniper, A jump, X hold to revive, Y use, bumpers change item.'
    Say 'Close the windows with Alt+F4 on any of them, or run Couch.cmd -Stop.'
}

try { Do-Couch } catch { Fail "Something went wrong: $($_.Exception.Message)" }
