# Disposable option integrations and discovery fixtures. Firewall uses a fake netsh, never UAC or real firewall rules.
param([Parameter(Mandatory=$true)][string]$TestRoot, [switch]$Mutants)
$ErrorActionPreference = 'Stop'
$source = [IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\install-state.ps1'))
$source = $source.Replace("Join-Path `$PSScriptRoot 'install-policy.exe'", "'unused-policy.exe'")
function Require($condition, $message) { if (-not $condition) { throw "ASSERT: $message" } }
function Run-Checks($code) {
    $root = Join-Path $TestRoot ('options-' + [guid]::NewGuid().ToString('N'))
    $registry = 'HKCU:\Software\mdk2-coop-test-' + [guid]::NewGuid().ToString('N')
    New-Item -ItemType Directory $root | Out-Null
    $code = $code.Replace("'HKCU:\Software\Classes\mdk2-coop'", "'$registry'").Replace("'HKCU:\Software\Classes\mdk2-coop\shell\open\command'", "'$registry\shell\open\command'")
    $code = $code.Replace("[Environment]::GetFolderPath('DesktopDirectory')", "'$($root.Replace("'", "''"))'")
    . ([scriptblock]::Create($code))
    try {
        $game = Join-Path $root 'game with spaces'; New-Item -ItemType Directory $game | Out-Null
        [IO.File]::WriteAllText((Join-Path $game 'mdk2hd.exe'), 'fixture')
        $rule = [pscustomobject]@{ Name='MDK2 Co-op (UDP)'; ApplicationName=(Join-Path $game 'mdk2hd.exe'); Protocol=17; Direction=1 }
        Require (Test-CoopFirewallRule $rule $game) 'our firewall rule recognised'
        foreach ($property in 'Name', 'ApplicationName', 'Protocol', 'Direction') {
            $originalValue = $rule.$property
            $rule.$property = if ($property -in 'Name', 'ApplicationName') { 'other' } else { 0 }
            Require (-not (Test-CoopFirewallRule $rule $game)) "other firewall $property excluded"
            $rule.$property = $originalValue
        }
        # Fake external firewall command. Checks arguments sent to netsh and models the matching rule.
        $script:rules = @(); $script:netshCalls = @()
        function Get-CoopFirewallRules($game) { return $script:rules }
        function Invoke-CoopHiddenProcess($file, $arguments) {
            Require ($file -eq 'netsh.exe') 'only netsh for firewall changes'
            $script:netshCalls += $arguments
            if ($arguments -match ' add rule ') {
                Require ($arguments -match 'name="MDK2 Co-op \(UDP\)"') 'stable firewall name'
                Require ($arguments.Contains(('program="{0}"' -f (Join-Path $game 'mdk2hd.exe')))) 'firewall exact executable'
                Require ($arguments -match 'dir=in action=allow' -and $arguments -match 'protocol=UDP' -and $arguments -match 'profile=private,public' -and $arguments -match 'enable=yes') 'inbound UDP, both profiles, enabled'
                $script:rules = @([pscustomobject]@{ Enabled=$true; Action=1; Profiles=6 })
            } elseif ($arguments -match ' delete rule ') {
                Require ($arguments -match 'program="' -and $arguments -match 'protocol=UDP dir=in') 'scoped firewall removal'
                $script:rules = @()
            } else { throw 'Unexpected firewall operation' }
            return [pscustomobject]@{ ExitCode=0 }
        }
        $before = Get-CoopOptions $game
        Require (-not $before.desktop_shortcut -and -not $before.invite_links -and -not $before.allow_hosting) 'options initially absent'
        Set-CoopUserOptions $game $true $true
        Set-CoopFirewall $game $false
        Set-CoopFirewall $game $false
        $after = Get-CoopOptions $game
        Require ($after.desktop_shortcut -and $after.invite_links -and $after.allow_hosting) 'options reported installed'
        foreach ($profiles in 2, 4) {
            $script:rules[0].Profiles = $profiles
            Require (-not (Get-CoopOptions $game).allow_hosting) 'both firewall profiles required'
        }
        $script:rules[0].Profiles = 6
        $script:rules[0].Enabled = $false
        Require (-not (Get-CoopOptions $game).allow_hosting) 'disabled firewall rule is not hosting allowed'
        $script:rules[0].Enabled = $true
        $script:rules[0].Action = 0
        Require (-not (Get-CoopOptions $game).allow_hosting) 'blocking firewall rule is not hosting allowed'
        $script:rules[0].Action = 1
        Require ((Get-ItemProperty $registry).'URL Protocol' -eq '') 'URL Protocol marker'
        Require ((Get-Item "$registry\shell\open\command").GetValue('') -eq ('"{0}" --coop-invite "%1"' -f (Join-Path $game 'mdk2hd.exe'))) 'URL command quotes path and invite'
        $other = Join-Path $root 'other'; New-Item -ItemType Directory $other | Out-Null
        Remove-CoopUserOptions $other
        Require ((Get-CoopOptions $game).invite_links -and (Get-CoopOptions $game).desktop_shortcut) 'other install cannot remove selected integrations'
        Remove-CoopUserOptions $game
        Set-CoopFirewall $game $true
        $after = Get-CoopOptions $game
        Require (-not $after.desktop_shortcut -and -not $after.invite_links -and -not $after.allow_hosting) 'all three integrations removed'
        # Read-only discovery: two Steam libraries (modern VDF), Overhaul, and GOG HD; ignore original MDK2.
        $steam = Join-Path $root 'Steam'; $library = Join-Path $root 'Library'
        $overhaulRoot = Join-Path $root 'Programs'; $gog = Join-Path $root 'GOG HD'; $original = Join-Path $root 'MDK2'
        $custom = Join-Path $root 'Custom Overhaul'; $epicGame = Join-Path $root 'Epic HD'; $programData = Join-Path $root 'ProgramData'
        $dirs = @((Join-Path $steam 'steamapps\common\MDK 2 HD'), (Join-Path $library 'steamapps\common\Custom MDK'), (Join-Path $overhaulRoot 'Overhaul Games\MDK 2 HD'), $gog, $custom, $epicGame)
        foreach ($dir in $dirs) { New-Item -ItemType Directory -Force $dir | Out-Null; [IO.File]::WriteAllText((Join-Path $dir 'mdk2hd.exe'), 'fixture') }
        New-Item -ItemType Directory $original | Out-Null; [IO.File]::WriteAllText((Join-Path $original 'mdk2.exe'), 'original')
        [IO.File]::WriteAllText((Join-Path $steam 'steamapps\libraryfolders.vdf'), ('"libraryfolders" {{ "0" {{ "path" "{0}" }} "1" {{ "path" "{1}" }} }}' -f $steam.Replace('\','\\'), $library.Replace('\','\\')))
        [IO.File]::WriteAllText((Join-Path $library 'steamapps\appmanifest_213350.acf'), '"AppState" { "installdir" "Custom MDK" }')
        $epicManifests = Join-Path $programData 'Epic\EpicGamesLauncher\Data\Manifests'
        New-Item -ItemType Directory -Force $epicManifests | Out-Null
        $epicManifest = Join-Path $epicManifests 'mdk.item'
        [IO.File]::WriteAllText($epicManifest, (@{ InstallLocation=$epicGame } | ConvertTo-Json))
        function Get-ItemProperty($Path, $ErrorAction) {
            if ($Path -like '*Valve\Steam') { return [pscustomobject]@{ SteamPath=$steam } }
            if ($Path -eq 'fixture-gog') { return [pscustomobject]@{ path=$gog } }
            if ($Path -eq 'fixture-overhaul') { return [pscustomobject]@{ DisplayName='MDK2 HD'; InstallLocation=$custom } }
            if ($Path -eq 'fixture-original') { return [pscustomobject]@{ path=$original } }
        }
        function Get-ChildItem($Path, $LiteralPath, $Filter, $ErrorAction) {
            if ($LiteralPath -eq $epicManifests) { return [pscustomobject]@{ FullName=$epicManifest } }
            if ($Path -eq 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall') { return [pscustomobject]@{ PSPath='fixture-overhaul' } }
            if ($Path -eq 'HKLM:\SOFTWARE\GOG.com\Games') { return @([pscustomobject]@{ PSPath='fixture-gog' }, [pscustomobject]@{ PSPath='fixture-original' }) }
        }
        $oldSystemDrive = $env:SystemDrive; $oldProgram64 = $env:ProgramW6432; $oldData = $env:ProgramData; $oldProgram = $env:ProgramFiles; $oldProgram32 = ${env:ProgramFiles(x86)}
        try {
            $env:SystemDrive = $root; $env:ProgramW6432 = $overhaulRoot; $env:ProgramData = $programData; $env:ProgramFiles = $overhaulRoot; ${env:ProgramFiles(x86)} = $overhaulRoot
            $found = @(Find-AllGames)
            Require ($found.Count -eq 6) "six distinct installs discovered, got $($found.Count)"
            foreach ($dir in $dirs) { Require ($found -contains $dir) "discovered $dir" }
        } finally { $env:SystemDrive = $oldSystemDrive; $env:ProgramW6432 = $oldProgram64; $env:ProgramData = $oldData; $env:ProgramFiles = $oldProgram; ${env:ProgramFiles(x86)} = $oldProgram32 }
        function Get-CoopGameBuild($exe) { return [pscustomobject]@{ Name=$(if ($exe.StartsWith($steam)) { 'Steam build 3902' } else { 'unknown' }); Known=$exe.StartsWith($steam) } }
        function Get-CoopInstallStatus($dir, $package, $settings, $data) { return [pscustomobject]@{ owner=$(if ($dir -eq $script:installedDir) { 'manual' } else { 'none' }) } }
        $script:installedDir = ''
        $cards = @(Get-CoopInstallCards '' '' '' $dirs)
        Require ($cards.Count -eq 6 -and @($cards | Where-Object selected).Count -eq 1 -and $cards[0].selected) 'all build cards, Steam preselected'
        $script:installedDir = $gog
        $cards = @(Get-CoopInstallCards '' '' '' $dirs)
        Require ($cards[3].selected -and @($cards | Where-Object selected).Count -eq 1) 'installed co-op preselected'
    } finally {
        Remove-Item -LiteralPath $registry -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
Run-Checks $source
Write-Output 'PASS: three integrations, scoped removal, invite command, all install cards; fake netsh (no elevation)'
if ($Mutants) {
    foreach ($twin in @(
        @('profile=private,public', 'profile=private'),
        @("`$rule.Name -eq 'MDK2 Co-op (UDP)'", '$true'),
        @("`$rule.ApplicationName -eq (Join-Path `$game 'mdk2hd.exe')", '$true'),
        @('$rule.Protocol -eq 17', '$true'),
        @('$rule.Direction -eq 1', '$true'),
        @('($profiles -band 6) -eq 6', '($profiles -band 2) -eq 2'),
        @('$rule.Enabled -and $rule.Action -eq 1', '$true'),
        @('protocol=UDP profile=', 'protocol=TCP profile='),
        @(' --coop-invite "%1"', ' %1'),
        @('if ($options.invite_links)', 'if ($false)'),
        @("        `$dirs += Join-Path `$lib 'steamapps\common\MDK 2 HD'", ''),
        @('if (-not $selected -and $snapshot.owner -ne ''none'')', 'if ($false)')
    )) {
        Require ($source.Contains($twin[0])) 'mutant applies'
        $failed = $false
        try { Run-Checks ($source.Replace($twin[0], $twin[1])) } catch { $failed = $true }
        Require $failed "mutant must fail: $($twin[0])"
        Write-Output "KILLED: $($twin[0])"
    }
}
