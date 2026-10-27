# MDK2 co-op diagnostics bundle. The launcher's "Send diagnostics" button runs this (the DLL carries a copy and
# writes it to <coop folder>\diag.ps1 first), so it always matches the installed mod. Run by hand:
#   powershell -ExecutionPolicy Bypass -File diag.ps1 -CoopDir "<game dir>\coop" [-GameDir "<game dir>"]
#
# It reads, never changes, the game and its saves, and writes one zip to the desktop:
#   - the newest session logs, flight recorder dumps and crash markers (coop\logs), bridge.log, coop.ini, player.ini,
#     the game's settings.lua, and a listing (names, sizes, times) of the coop folder. No saves, no game files.
#   - system.txt: Windows, CPU, memory, graphics card and driver, displays, game crashes in the Windows event log,
#     Defender detections that mention the mod, overlay and capture programs that are running.
#   - network.txt: adapters, Wi-Fi signal, network profile (public blocks hosting), firewall rules for the game,
#     a ping to the peer in coop.ini, and Tailscale's view of the peer (direct or relayed) when it is installed.
# Names are replaced before anything is zipped: your Windows and computer names, your profile folder, every IP
# address (the same address always gets the same number: ip-3), MAC addresses and Steam IDs.
# manifest.json lists every file with its SHA-256 and whether each session log's hash chain was intact before that.
param(
    [string]$CoopDir = '',
    [string]$GameDir = '',
    [string]$Version = 'unknown',
    [string]$OutDir = '',
    [int]$MaxLogMB = 40,
    [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
$ScriptVersion = 1
$result = $null
if ($CoopDir) { $result = Join-Path $CoopDir 'diag-result.txt' }

function Finish($text) {
    if ($result) { try { Set-Content -LiteralPath $result -Value $text -Encoding ASCII } catch { } }
}

$stage = $null
try {
    if (-not $CoopDir -or -not (Test-Path -LiteralPath $CoopDir)) { throw "coop folder not found: $CoopDir" }
    if (-not $GameDir) { $GameDir = Split-Path -Parent $CoopDir }
    if (-not $OutDir) { $OutDir = [Environment]::GetFolderPath('Desktop') }
    if (-not $OutDir -or -not (Test-Path -LiteralPath $OutDir)) { $OutDir = $env:USERPROFILE }

    $stage = Join-Path ([IO.Path]::GetTempPath()) ('mdk2-coop-diag-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $problems = New-Object System.Collections.ArrayList

    $timings = [ordered]@{}
    function Section($name, [scriptblock]$body) {
        $t0 = Get-Date
        try { & $body } catch { [void]$problems.Add(("{0}: {1}" -f $name, $_.Exception.Message)) }
        $timings[$name] = [int]((Get-Date) - $t0).TotalMilliseconds
    }

    # ---- the hash chain of a session log, checked before anything is redacted ----
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
public static class MdkChain {
    // "<n> <utc ms> <chain> <text>": chain = FNV-1a/64 of "<n> <utc ms> <text>", continued from the previous line's
    // a log the game still has open is read with FileShare.ReadWrite; its last line may be half written (ignored)
    public static byte[] ReadShared(string path) {
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (MemoryStream m = new MemoryStream()) { f.CopyTo(m); return m.ToArray(); }
    }
    public static string Verify(string path) {
        byte[] data = ReadShared(path);
        ulong h = 0xcbf29ce484222325UL;
        int lines = 0, pos = 0;
        bool clean = false;
        string first = "", last = "";
        while (pos < data.Length) {
            int end = Array.IndexOf(data, (byte)10, pos);
            if (end < 0) break;     // still being written, or cut off by a crash: not a broken chain
            int len = end - pos;
            int s1 = Array.IndexOf(data, (byte)32, pos, len);
            int s2 = s1 < 0 ? -1 : Array.IndexOf(data, (byte)32, s1 + 1, end - s1 - 1);
            int s3 = s2 < 0 ? -1 : Array.IndexOf(data, (byte)32, s2 + 1, end - s2 - 1);
            if (s3 < 0) return "broken|" + lines + "|malformed line " + lines + "|" + clean + "|" + first + "|" + last;
            string n = Encoding.ASCII.GetString(data, pos, s1 - pos);
            string utc = Encoding.ASCII.GetString(data, s1 + 1, s2 - s1 - 1);
            string chain = Encoding.ASCII.GetString(data, s2 + 1, s3 - s2 - 1);
            if (n != lines.ToString()) return "broken|" + lines + "|line number " + n + " where " + lines + " was expected (lines missing)|" + clean + "|" + first + "|" + last;
            for (int i = pos; i < s2; i++) { h ^= data[i]; h *= 0x100000001b3UL; }
            h ^= 32; h *= 0x100000001b3UL;
            for (int i = s3 + 1; i < end; i++) { h ^= data[i]; h *= 0x100000001b3UL; }
            if (chain != h.ToString("x16")) return "broken|" + lines + "|chain mismatch at line " + lines + " (edited or damaged)|" + clean + "|" + first + "|" + last;
            if (lines == 0) first = utc;
            last = utc;
            clean = Encoding.ASCII.GetString(data, s3 + 1, end - s3 - 1) == "session end: clean exit";
            lines++;
            pos = end + 1;
        }
        return "ok|" + lines + "||" + clean + "|" + first + "|" + last;
    }
}
'@

    $files = New-Object System.Collections.ArrayList     # staged file names, relative
    function Stage($rel, $content) {
        $path = Join-Path $stage $rel
        $dir = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        [IO.File]::WriteAllText($path, $content, (New-Object Text.UTF8Encoding($false)))
        if (-not $files.Contains($rel)) { [void]$files.Add($rel) }
    }
    function StageCopy($src, $rel) {
        $path = Join-Path $stage $rel
        $dir = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        [IO.File]::WriteAllBytes($path, [MdkChain]::ReadShared($src))      # also while the game has it open
        [void]$files.Add($rel)
    }

    # ---- logs: newest first, up to the size limit; crashes always ----
    $chains = @{}
    Section 'logs' {
        $logs = Join-Path $CoopDir 'logs'
        if (Test-Path -LiteralPath $logs) {
            $all = Get-ChildItem -LiteralPath $logs -File | Sort-Object Name -Descending
            $budget = [int64]$MaxLogMB * 1MB
            $keys = @{}
            foreach ($f in $all) { $k = $f.Name.Split('.')[0]; if (-not $keys.ContainsKey($k)) { $keys[$k] = New-Object System.Collections.ArrayList }; [void]$keys[$k].Add($f) }
            foreach ($k in ($keys.Keys | Sort-Object -Descending)) {
                $crashed = ($keys[$k] | Where-Object { $_.Name -like '*.crash' }).Count -gt 0
                $size = ($keys[$k] | Measure-Object Length -Sum).Sum
                if ($size -gt $budget -and -not $crashed) { [void]$problems.Add("older session $k left out (size limit)"); continue }
                $budget -= $size
                foreach ($f in $keys[$k]) {
                    if ($f.Name -like '*.log') {
                        $chains[$f.Name] = [MdkChain]::Verify($f.FullName)
                    }
                    StageCopy $f.FullName ('logs\' + $f.Name)
                }
            }
        }
    }
    foreach ($name in 'bridge.log', 'bridge.previous.log', 'coop.ini', 'player.ini') {
        $p = Join-Path $CoopDir $name
        if (Test-Path -LiteralPath $p) { Section $name { StageCopy $p $name } }
    }
    Section 'settings.lua' {
        $p = Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua'
        if ((Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).Length -lt 200KB) { StageCopy $p 'game-settings.lua' }
    }
    Section 'coop folder listing' {
        $sb = New-Object Text.StringBuilder
        foreach ($root in @($CoopDir, (Join-Path $env:LOCALAPPDATA 'mdk2-coop'))) {
            if (-not (Test-Path -LiteralPath $root)) { continue }
            [void]$sb.AppendLine("== $root")
            Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike '*\logs\*' } |
                ForEach-Object { [void]$sb.AppendLine(("{0,10}  {1:yyyy-MM-dd HH:mm:ss}  {2}" -f $_.Length, $_.LastWriteTime, $_.FullName.Substring($root.Length))) }
        }
        Stage 'coop-folder.txt' $sb.ToString()
    }

    # ---- system ----
    Section 'system' {
        $sb = New-Object Text.StringBuilder
        function Line($s) { [void]$sb.AppendLine($s) }
        Line "mod version: $Version    diag script: $ScriptVersion    created (utc): $([DateTime]::UtcNow.ToString('o'))"
        $os = Get-CimInstance Win32_OperatingSystem
        Line ("windows: {0} {1} build {2}, {3}; up since {4:yyyy-MM-dd HH:mm}" -f $os.Caption, $os.Version, $os.BuildNumber, $os.OSArchitecture, $os.LastBootUpTime)
        $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
        Line ("cpu: {0}, {1} cores / {2} threads" -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors)
        Line ("memory: {0:N1} GB, {1:N1} GB free" -f ($os.TotalVisibleMemorySize / 1MB), ($os.FreePhysicalMemory / 1MB))
        foreach ($g in Get-CimInstance Win32_VideoController) {
            Line ("graphics: {0}, driver {1} ({2:yyyy-MM-dd}), {3}x{4} @ {5} Hz" -f $g.Name, $g.DriverVersion, $g.DriverDate, $g.CurrentHorizontalResolution, $g.CurrentVerticalResolution, $g.CurrentRefreshRate)
        }
        Line "powershell: $($PSVersionTable.PSVersion)    culture: $((Get-Culture).Name)"
        $exe = Join-Path $GameDir 'mdk2hd.exe'
        if (Test-Path -LiteralPath $exe) {
            $fi = Get-Item -LiteralPath $exe
            Line ("game exe: {0} bytes, modified {1:yyyy-MM-dd}, version {2}" -f $fi.Length, $fi.LastWriteTime, $fi.VersionInfo.FileVersion)
        } else { Line "game exe: mdk2hd.exe not found in the game folder" }
        foreach ($rel in 'dinput8.dll', 'coop\coop.lua') {
            $p = Join-Path $GameDir $rel
            if (Test-Path -LiteralPath $p) {
                $fi = Get-Item -LiteralPath $p
                Line ("{0}: {1} bytes, modified {2:yyyy-MM-dd HH:mm}, sha256 {3}" -f $rel, $fi.Length, $fi.LastWriteTime, (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower())
            } else { Line "${rel}: missing" }
        }
        $sys = Join-Path $env:SystemRoot 'SysWOW64\dinput8.dll'
        if (Test-Path -LiteralPath $sys) { Line ("system dinput8.dll (32-bit): {0}" -f (Get-Item -LiteralPath $sys).VersionInfo.FileVersion) }
        Line ''
        Line '== programs that often get in the way (running now)'
        $watch = 'steam', 'steamwebhelper', 'tailscaled', 'tailscale-ipn', 'discord', 'obs64', 'RTSS', 'MSIAfterburner', 'GameBar', 'GameBarFTServer',
                 'NVIDIA Share', 'nvsphelper64', 'RadeonSoftware', 'overwolf', 'Medal', 'Xbox', 'Beamdog', 'mdk2hd'
        Get-Process -ErrorAction SilentlyContinue | Where-Object { $watch -contains $_.ProcessName } | Sort-Object ProcessName |
            ForEach-Object { $at = try { $_.StartTime.ToString('HH:mm') } catch { '?' }; Line ("  {0} (started {1})" -f $_.ProcessName, $at) }
        Line ''
        Line '== game crashes in the Windows event log (14 days)'
        $found = 0
        try {
            Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddDays(-14); Id = 1000, 1001 } -ErrorAction Stop |
                Where-Object { $_.Message -match 'mdk2' } | Select-Object -First 8 | ForEach-Object {
                    $found++
                    $m = ($_.Message -split "`r?`n" | Where-Object { $_ -match 'Faulting|Fault bucket|Exception code|Fault offset|Event Name|P[0-9]:' } | Select-Object -First 8) -join ' | '
                    Line ("  {0:yyyy-MM-dd HH:mm}  id {1}  {2}" -f $_.TimeCreated, $_.Id, $m)
                }
        } catch { }
        if (-not $found) { Line '  none' }
        Line ''
        Line '== Windows Defender detections that mention the mod'
        $found = 0
        try {
            Get-MpThreatDetection -ErrorAction Stop | Where-Object { ($_.Resources -join ' ') -match 'dinput8|mdk2' } | Select-Object -First 5 | ForEach-Object {
                $found++
                Line ("  {0:yyyy-MM-dd HH:mm}  threat {1}: {2}" -f $_.InitialDetectionTime, $_.ThreatID, ($_.Resources -join ', '))
            }
        } catch { }
        if (-not $found) { Line '  none (or Defender is not the active antivirus)' }
        Stage 'system.txt' $sb.ToString()
    }

    # ---- network ----
    Section 'network' {
        $ErrorActionPreference = 'Continue'      # a program's stderr (tailscale ping with no answer) is output here, not a failure
        $sb = New-Object Text.StringBuilder
        function Line($s) { [void]$sb.AppendLine($s) }
        Line '== adapters that are up'
        Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' } | ForEach-Object {
            Line ("  {0} | {1} | {2} | {3}" -f $_.Name, $_.InterfaceDescription, $_.LinkSpeed, $_.MediaType)
        }
        Line ''
        Line '== addresses (IPv4)'
        Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '127.*' } | ForEach-Object {
            Line ("  {0}/{1} on {2}" -f $_.IPAddress, $_.PrefixLength, $_.InterfaceAlias)
        }
        Line ''
        Line '== network profile (Public blocks incoming connections unless a rule allows them)'
        Get-NetConnectionProfile -ErrorAction SilentlyContinue | ForEach-Object { Line ("  {0}: {1}, ipv4 {2}" -f $_.InterfaceAlias, $_.NetworkCategory, $_.IPv4Connectivity) }
        Line ''
        Line '== wi-fi (name and access point removed)'
        $wl = & netsh wlan show interfaces 2>$null
        if ($wl) { $wl | Where-Object { $_ -match 'State|Radio type|Channel|Receive rate|Transmit rate|Signal|Band' } | ForEach-Object { Line ("  " + $_.Trim()) } } else { Line '  no wi-fi interface' }
        Line ''
        Line '== firewall rules for mdk2hd.exe (a Block rule beats an Allow rule)'
        $rules = @{}
        try {
            Get-NetFirewallApplicationFilter -ErrorAction Stop | Where-Object { $_.Program -like '*mdk2hd*' } | ForEach-Object {
                $r = $_ | Get-NetFirewallRule
                $rules[("{0} | {1} | profile {2} | enabled {3} | {4}" -f $r.Direction, $r.Action, $r.Profile, $r.Enabled, $_.Program)] = 1
            }
        } catch { }
        foreach ($k in ($rules.Keys | Sort-Object | Select-Object -First 12)) { Line ("  " + $k) }
        if (-not $rules.Count) { Line '  none (Windows asks the first time the game listens; a refused prompt blocks hosting)' }

        # the peer from coop.ini ([net] peer_host / peer_port, used for UDP joins)
        $peer = $null
        $ini = Join-Path $CoopDir 'coop.ini'
        if (Test-Path -LiteralPath $ini) {
            $sect = ''
            foreach ($l in Get-Content -LiteralPath $ini) {
                if ($l -match '^\s*\[(.+)\]') { $sect = $Matches[1]; continue }
                if ($sect -eq 'net' -and $l -match '^\s*peer_host\s*=\s*(\S+)') { $peer = $Matches[1] }
            }
        }
        Line ''
        if ($peer -and $peer -notlike '127.*') {
            Line "== ping to the peer in coop.ini: $peer (ICMP; some networks block it)"
            try {
                $png = New-Object System.Net.NetworkInformation.Ping
                $t = @()
                for ($i = 0; $i -lt 6; $i++) {
                    $a = $png.Send($peer, 800)
                    if ($a.Status -eq 'Success') { $t += [int]$a.RoundtripTime }
                    Start-Sleep -Milliseconds 100
                }
                if ($t.Count) {
                    Line ("  replies {0}/6, min {1} ms, avg {2:N0} ms, max {3} ms" -f $t.Count, ($t | Measure-Object -Minimum).Minimum, ($t | Measure-Object -Average).Average, ($t | Measure-Object -Maximum).Maximum)
                } else { Line '  no replies in 0.8 s each (blocked, or not reachable)' }
            } catch { Line "  ping failed: $($_.Exception.Message)" }
        } else {
            Line '== ping: no peer_host in coop.ini (hosting, or Steam lobbies)'
        }
        $ts = $null
        foreach ($c in 'tailscale.exe', (Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe')) {
            $cmd = Get-Command $c -ErrorAction SilentlyContinue
            if ($cmd) { $ts = $cmd.Source; break }
        }
        Line ''
        if ($ts) {
            # only what bears on the connection: no accounts, no device names
            Line '== tailscale'
            $st = (& $ts status --json 2>$null) -join "`n" | ConvertFrom-Json
            $peers = @($st.Peer.PSObject.Properties | ForEach-Object { $_.Value })
            Line ("  backend {0}, {1} peers ({2} online)" -f $st.BackendState, $peers.Count, @($peers | Where-Object { $_.Online }).Count)
            foreach ($h in @($st.Health) | Select-Object -First 3) { if ($h) { Line ("  health: " + $h) } }
            if ($peer) {
                $p = $peers | Where-Object { $_.TailscaleIPs -contains $peer } | Select-Object -First 1
                if ($p) {
                    $path = if ($p.CurAddr) { "direct via " + $p.CurAddr } elseif ($p.Relay) { "relayed via DERP " + $p.Relay } else { 'no path yet' }
                    Line ("  the peer: online {0}, {1}, sent {2} B, received {3} B, last handshake {4}" -f $p.Online, $path, $p.TxBytes, $p.RxBytes, $p.LastHandshake)
                    Line "== tailscale ping to the peer"
                    & $ts ping --c 3 --timeout 2s $peer 2>&1 | Select-Object -First 6 | ForEach-Object { Line ("  " + ($_ -replace '^pong from \S+ \(', 'pong from peer (')) }
                } else { Line '  the peer address is not a Tailscale peer of this computer' }
            }
        } else { Line '== tailscale: not installed' }
        Stage 'network.txt' $sb.ToString()
    }

    # ---- redaction ----
    $userName = [Environment]::UserName
    $machine = [Environment]::MachineName
    $profile = $env:USERPROFILE
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $ipIds = @{}
    $macIds = @{}
    $steamIds = @{}
    $counts = @{ ip = 0; mac = 0; steam = 0; user = 0; machine = 0; profile = 0 }

    function IpLabel([string]$ip) {
        $o = $ip.Split('.') | ForEach-Object { [int]$_ }
        if ($o[0] -eq 10 -or ($o[0] -eq 192 -and $o[1] -eq 168) -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31)) { return 'lan' }
        if ($o[0] -eq 100 -and $o[1] -ge 64 -and $o[1] -le 127) { return 'tailscale-or-cgnat' }
        if ($o[0] -eq 169 -and $o[1] -eq 254) { return 'linklocal' }
        return 'public'
    }
    function Redact([string]$text, [bool]$ips) {
        if ($profile) { $n = ([regex]::Matches($text, [regex]::Escape($profile), 'IgnoreCase')).Count; if ($n) { $counts.profile += $n; $text = $text -replace [regex]::Escape($profile), '%USERPROFILE%' } }
        if ($userName.Length -ge 2) {
            $re = '(?<![A-Za-z0-9])' + [regex]::Escape($userName) + '(?![A-Za-z0-9])'
            $n = ([regex]::Matches($text, $re, 'IgnoreCase')).Count; if ($n) { $counts.user += $n; $text = [regex]::Replace($text, $re, '<user>', 'IgnoreCase') }
        }
        if ($machine.Length -ge 2) {
            $re = '(?<![A-Za-z0-9])' + [regex]::Escape($machine) + '(?![A-Za-z0-9])'
            $n = ([regex]::Matches($text, $re, 'IgnoreCase')).Count; if ($n) { $counts.machine += $n; $text = [regex]::Replace($text, $re, '<machine>', 'IgnoreCase') }
        }
        $text = [regex]::Replace($text, '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', '<email>')
        $text = [regex]::Replace($text, '(?<![A-Za-z0-9-])[A-Za-z0-9-]+\.[A-Za-z0-9-]+\.ts\.net', '<tailnet-host>')
        $text = [regex]::Replace($text, '(?<![A-Za-z0-9])7656119\d{10}(?![0-9])', {
            param($m)
            if (-not $steamIds.ContainsKey($m.Value)) { $steamIds[$m.Value] = $steamIds.Count + 1 }
            $counts.steam++
            return 'steam-' + $steamIds[$m.Value]
        })
        $text = [regex]::Replace($text, '(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])', {
            param($m)
            if (-not $macIds.ContainsKey($m.Value)) { $macIds[$m.Value] = $macIds.Count + 1 }
            $counts.mac++
            return 'mac-' + $macIds[$m.Value]
        })
        if ($ips) {
            $text = [regex]::Replace($text, '(?<![0-9.])(?:(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])(?![0-9]|\.[0-9])', {
                param($m)
                if ($m.Value.StartsWith('127.') -or $m.Value -eq '0.0.0.0') { return $m.Value }
                if (-not $ipIds.ContainsKey($m.Value)) { $ipIds[$m.Value] = $ipIds.Count + 1 }
                $counts.ip++
                return ('ip-{0}({1})' -f $ipIds[$m.Value], (IpLabel $m.Value))
            })
        }
        return $text
    }

    # the chain results go in the manifest as found; what is zipped is the redacted text
    foreach ($rel in @($files)) {
        $path = Join-Path $stage $rel
        $text = $latin1.GetString([IO.File]::ReadAllBytes($path))
        $red = Redact $text ($rel -ne 'system.txt')
        [IO.File]::WriteAllBytes($path, $latin1.GetBytes($red))
    }

    # ---- manifest and a note for whoever opens it ----
    $entries = @()
    $idSeed = New-Object Text.StringBuilder
    foreach ($rel in ($files | Sort-Object)) {
        $path = Join-Path $stage $rel
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
        [void]$idSeed.Append($hash)
        $e = [ordered]@{ file = ($rel -replace '\\', '/'); bytes = (Get-Item -LiteralPath $path).Length; sha256 = $hash }
        $leaf = Split-Path -Leaf $rel
        if ($chains.ContainsKey($leaf)) {
            $c = $chains[$leaf].Split('|')
            $e['chain'] = [ordered]@{ status = $c[0]; lines = [int]$c[1]; problem = $c[2]; clean_exit = ($c[3] -eq 'True'); first_utc_ms = $c[4]; last_utc_ms = $c[5] }
        }
        $entries += , $e
    }
    $idBytes = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::ASCII.GetBytes($idSeed.ToString()))
    $bundleId = ([BitConverter]::ToString($idBytes) -replace '-', '').ToLower().Substring(0, 8)
    $manifest = [ordered]@{
        format = 1; script = $ScriptVersion; mod_version = $Version; bundle_id = $bundleId
        created_utc = [DateTime]::UtcNow.ToString('o')
        redacted = [ordered]@{ ips = $counts.ip; mac_addresses = $counts.mac; steam_ids = $counts.steam; user_names = $counts.user; machine_names = $counts.machine; profile_paths = $counts.profile }
        problems = @($problems)
        timing_ms = $timings
        files = $entries
    }
    $json = $manifest | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), $json, (New-Object Text.UTF8Encoding($false)))
    $note = @"
MDK2 co-op diagnostics bundle $bundleId (mod $Version)

logs\*.log          one file per game run, lines numbered and hash-chained: tools/diag_report.py checks them
logs\*.flight-*.txt the flight recorder: the last minute before a crash, a desync or an F2 mark
logs\*.crash        marks a run that crashed
system.txt          Windows, hardware, game crashes, antivirus, overlays
network.txt         adapters, Wi-Fi, firewall, ping to the peer, Tailscale
manifest.json       every file with its SHA-256, and whether each log's chain was intact before names were removed

Replaced before zipping: your Windows and computer names, your profile folder, IP addresses (ip-1, ip-2...),
MAC addresses and Steam IDs. Nothing else was changed. No saves and no game files are in here.
"@
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $note, (New-Object Text.UTF8Encoding($false)))

    # ---- zip ----
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = Join-Path $OutDir ('mdk2-coop-diag_{0:yyyyMMdd-HHmmss}_{1}.zip' -f (Get-Date), $bundleId)
    # entry names with '/', as every unzip tool expects (CreateFromDirectory writes '\' on some .NET versions)
    $za = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        foreach ($f in (Get-ChildItem -LiteralPath $stage -Recurse -File)) {
            $entry = $f.FullName.Substring($stage.Length + 1) -replace '\\', '/'
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose() }
    Remove-Item -LiteralPath $stage -Recurse -Force
    $stage = $null
    Finish "OK $zip"
    if (-not $NoOpen) { Start-Process explorer.exe -ArgumentList "/select,`"$zip`"" }
} catch {
    Finish ("ERR " + $_.Exception.Message)
    if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
    exit 1
}
