# Game-free: pure policy, disposable filesystem fixtures and source mutants.
# Windows PowerShell 5.1: powershell -NoProfile -File tools/test_install_state.ps1 -Mutants
param([switch]$Mutants, [string]$PolicyPath, [string]$TestRoot = ([IO.Path]::GetTempPath()))
$ErrorActionPreference = 'Stop'
# Parse every shipped script with the actual Windows PowerShell grammar.
foreach ($scriptFile in Get-ChildItem (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist') -Filter '*.ps1') {
    $tokens = $null; $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($scriptFile.FullName, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "PowerShell parse error in $($scriptFile.Name): $($parseErrors[0].Message)" }
}
$module = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist/install-state.ps1'
$source = [IO.File]::ReadAllText($module)
# scriptblock mutants have no module PSScriptRoot; pin only the helper location.
$helper = if ($PolicyPath) { $PolicyPath } else { Join-Path (Split-Path -Parent $module) 'install-policy.exe' }
$source = $source.Replace("Join-Path `$PSScriptRoot 'install-policy.exe'", "'$($helper.Replace("'", "''"))'")
function Require($value, $message) { if (-not $value) { throw "ASSERT: $message" } }
function Reject($operation, $message) {
    $rejected = $false
    try { & $operation | Out-Null } catch { $rejected = $true }
    Require $rejected $message
}
# Exercise the actual child process, not just ProcessStartInfo properties. The
# console probe reports a failure if Windows attached or allocated any console.
function Test-HiddenProcess($code) {
    . ([scriptblock]::Create($code))
    $root = Join-Path $TestRoot ([guid]::NewGuid().ToString())
    New-Item -ItemType Directory $root | Out-Null
    try {
        $probe = Join-Path $root 'console probe.exe'
        Add-Type -OutputAssembly $probe -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
class ConsoleProbe {
    [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
    static int Main(string[] args) {
        if (GetConsoleWindow() != IntPtr.Zero) return 91;
        if (args.Length != 1 || args[0] != "argument with spaces") return 92;
        Console.Error.Write(new string('e', 131072));
        Console.Out.WriteLine("17");
        return 0;
    }
}
'@
        $result = Invoke-CoopHiddenProcess $probe '"argument with spaces"'
        Require ($result.ExitCode -eq 0) 'child has no console and receives quoted arguments'
        Require ($result.Output.Trim() -eq '17') 'stdout captured'
        Require ($result.Error.Length -eq 131072) 'stderr drained without deadlock'
        $result = Invoke-CoopHiddenProcess $probe 'wrong'
        Require ($result.ExitCode -eq 92) 'nonzero exit preserved'
        $script:CoopPolicy = $probe
        Reject { Invoke-CoopPolicy @('wrong') } 'policy rejects failed child'
        Reject { Invoke-CoopPolicy @('unsafe argument') } 'policy rejects non-protocol arguments'
    } finally { Remove-Item -LiteralPath $root -Recurse -Force }
}
# A pseudoconsole has no HWND. Use a hidden real console for the inheritance
# mutant, restoring the runner's parent console afterwards.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ProbeConsole {
    [DllImport("kernel32.dll")] public static extern bool AllocConsole();
    [DllImport("kernel32.dll")] public static extern bool FreeConsole();
    [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int show);
}
'@
$allocated = $false
try {
    if ([ProbeConsole]::GetConsoleWindow() -eq [IntPtr]::Zero) {
        [void][ProbeConsole]::FreeConsole()
        $allocated = [ProbeConsole]::AllocConsole()
        if (-not $allocated) { throw 'Could not establish console inheritance fixture' }
        [void][ProbeConsole]::ShowWindow([ProbeConsole]::GetConsoleWindow(), 0)
    }
    Test-HiddenProcess $source
    Write-Output 'PASS: hidden helper process, streams, arguments and exit status'
    if ($Mutants) {
        Reject { Test-HiddenProcess ($source.Replace('$info.CreateNoWindow = $true', '$info.CreateNoWindow = $false')) } 'console-window mutant detected'
        Write-Output 'PASS: console-window mutant rejected'
    }
} finally {
    if ($allocated) {
        [void][ProbeConsole]::FreeConsole()
        [void][ProbeConsole]::AttachConsole([uint32]::MaxValue)
    }
}
function Run-Checks($code) {
    . ([scriptblock]::Create($code))
    $root = Join-Path $TestRoot ([guid]::NewGuid().ToString())
    New-Item -ItemType Directory $root | Out-Null
    try {
        $r = [pscustomobject]@{ schema=1; installed='old'; original='original'; pending='pending' }
        Require ((Get-CoopFileState 'new' 'new' $r 'original') -eq 'current') 'current'
        Require ((Get-CoopFileState 'new' 'old' $r 'original') -eq 'outdated') 'update'
        Require ((Get-CoopFileState 'new' 'pending' $r 'original') -eq 'outdated') 'pending update'
        Require ((Get-CoopFileState 'new' 'foreign' $r 'original') -eq 'modified') 'foreign edit'
        Require ((Get-CoopFileState 'new' 'new' $r 'bad') -eq 'conflict') 'bad backup blocks even current payload'
        Require ((Get-CoopFileState 'new' $null $r 'original') -eq 'missing') 'partial missing payload'
        Require ((Get-CoopFileState 'new' 'original' $r 'original') -eq 'missing') 'interrupted install'
        Require ((Get-CoopFileState 'new' 'foreign' $null $null) -eq 'unmanaged') 'unmanaged baseline'
        Require ((Get-CoopFileState 'new' $null $null 'orphan') -eq 'conflict') 'orphan backup'
        Require ((Get-CoopFileState 'new' 'new' $r 'original' $true) -eq 'unknown') 'read failure'
        Require ((Get-CoopActions @('current','missing') $true).install -eq 'Repair') 'repair action'
        Require ((Get-CoopActions @('outdated','current') $true).install -eq 'Update') 'update action'
        Require ((Get-CoopActions @('missing') $false).install -eq 'Install') 'install action'
        Require (-not (Get-CoopActions @('current') $true).install) 'no redundant install'
        foreach ($state in 'modified','conflict','unknown') {
            Require (-not (Get-CoopActions @($state,'missing') $true).install) "blocked $state"
            Require (-not (Get-CoopActions @($state,'missing') $true).uninstall) "blocked uninstall $state"
        }
        $loader = "-- >>> mdk2-coop >>>`r`nif (coop_init) then coop_init() end`r`n-- <<< mdk2-coop <<<`r`n"
        Require ((Get-CoopLoaderState $loader) -eq 'current') 'loader exact'
        Require ((Get-CoopLoaderState ($loader + $loader)) -eq 'conflict') 'duplicate loader'
        Require ((Get-CoopLoaderState ($loader.Replace('coop_init()', 'evil()'))) -eq 'conflict') 'modified loader'
        Require ((Get-CoopLoaderState ($loader.TrimEnd() + 'foreign()')) -eq 'conflict') 'loader trailing code'
        Require ((Get-CoopLoaderState '-- <<< mdk2-coop <<<') -eq 'conflict') 'partial loader'
        $outside = Join-Path $root 'outside'
        New-Item -ItemType Directory $outside | Out-Null
        $link = Join-Path $root 'junction'
        New-Item -ItemType Junction -Path $link -Target $outside | Out-Null
        try {
            Reject { Install-CoopFile (Join-Path $link 'payload') ([byte[]]@(1)) } 'junction install refused'
            Require (-not (Test-Path (Join-Path $outside 'payload'))) 'linked destination untouched'
        } finally { [IO.Directory]::Delete($link) }
        $target = Join-Path $root 'payload'
        [IO.File]::WriteAllText($target, 'original')
        $originalHash = Get-CoopHash $target
        Install-CoopFile $target ([Text.Encoding]::ASCII.GetBytes('version one'))
        Require ((Get-CoopHash "$target.coop-original") -eq $originalHash) 'backup original'
        Install-CoopFile $target ([Text.Encoding]::ASCII.GetBytes('version two'))
        Require ((Get-CoopHash "$target.coop-original") -eq $originalHash) 'update never replaces backup'
        $installed = Get-CoopHash $target
        [IO.File]::WriteAllText($target, 'player modification')
        Reject { Install-CoopFile $target ([byte[]]@(1,2,3)) } 'modified update refused'
        Reject { Remove-CoopFile $target } 'modified uninstall refused'
        Require ([IO.File]::ReadAllText($target) -eq 'player modification') 'modified file untouched'
        [IO.File]::WriteAllText($target, 'version two')
        [IO.File]::WriteAllText("$target.coop-original", 'corrupt')
        Reject { Remove-CoopFile $target } 'corrupt backup refused'
        [IO.File]::WriteAllText("$target.coop-original", 'original')
        Require (Remove-CoopFile $target) 'remove owned file'
        Require ((Get-CoopHash $target) -eq $originalHash) 'restore original bytes'
        Require (-not (Test-Path "$target.coop-receipt.json")) 'receipt retired'
        Remove-Item $target
        Install-CoopFile $target ([byte[]]@(1,2,3))
        Remove-Item $target
        Require (Remove-CoopFile $target) 'partial missing file uninstall'
        Require (-not (Test-Path $target)) 'new file stays absent'
        [IO.File]::WriteAllText($target, 'identical')
        Install-CoopFile $target ([Text.Encoding]::ASCII.GetBytes('identical'))
        Require (Remove-CoopFile $target) 'own exact package payload with original backup'
        Require ([IO.File]::ReadAllText($target) -eq 'identical') 'unowned identical original restored'
        [IO.File]::WriteAllText($target, 'unmanaged')
        Require (-not (Remove-CoopFile $target)) 'unmanaged uninstall returns false'
        Require ([IO.File]::ReadAllText($target) -eq 'unmanaged') 'unmanaged bytes preserved'
        [IO.File]::WriteAllText("$target.coop-receipt.json", '{}')
        Reject { Install-CoopFile $target ([byte[]]@(1)) } 'corrupt receipt refused'
        # A schema mutant can still reject '{}' because its hash is also invalid.
        # Isolate receipt fields so every format guard must reject on its own.
        $validHash = 'a' * 64
        foreach ($invalid in @(
            @{ schema=2; installed=$validHash; original=$null; pending=$null },
            @{ schema=1; installed='invalid'; original=$null; pending=$null },
            @{ schema=1; installed=$validHash; original='invalid'; pending=$null },
            @{ schema=1; installed=$validHash; original=$null; pending='invalid' }
        )) {
            [IO.File]::WriteAllText("$target.coop-receipt.json", ($invalid | ConvertTo-Json -Compress))
            Reject { Read-CoopReceipt $target } 'invalid receipt field refused independently'
        }
        # Sibling saves must be byte-identical across every install/uninstall operation.
        $save = Join-Path $root 'save.dat'; [IO.File]::WriteAllBytes($save, ([byte[]]@(0,255,12,34)))
        $saved = Get-CoopHash $save
        Remove-Item "$target.coop-receipt.json"
        Install-CoopFile $target ([byte[]]@(8,7,6))
        Require (Remove-CoopFile $target) 'sibling fixture uninstall'
        Require ((Get-CoopHash $save) -eq $saved) 'save unchanged'
        # End-to-end Get-CoopInstallStatus inventory and dynamic button transitions.
        $pkg = Join-Path $root 'pkg'; $game = Join-Path $root 'game'; $data = Join-Path $root 'data'; $settings = Join-Path $root 'settings.lua'
        New-Item -ItemType Directory (Join-Path $pkg 'coop'), (Join-Path $game 'coop'), $data | Out-Null
        [IO.File]::WriteAllText((Join-Path $pkg 'dinput8.dll'), 'dll-v1')
        [IO.File]::WriteAllText((Join-Path $pkg 'coop\coop.lua'), 'lua-v1')
        $st = Get-CoopInstallStatus $game $pkg $settings $data $false
        Require ($st.actions.install -eq 'Install' -and -not $st.actions.uninstall -and -not $st.actions.blocked) 'fresh install status'
        Install-CoopFile (Join-Path $game 'dinput8.dll') ([Text.Encoding]::ASCII.GetBytes('dll-v1'))
        Install-CoopFile (Join-Path $game 'coop\coop.lua') ([Text.Encoding]::ASCII.GetBytes('lua-v1'))
        [IO.File]::WriteAllText($settings, $loader, (New-Object Text.ASCIIEncoding))
        $st = Get-CoopInstallStatus $game $pkg $settings $data $false
        Require ($st.actions.install -eq '' -and $st.actions.uninstall -and -not $st.actions.blocked) 'current status'
        [IO.File]::WriteAllText((Join-Path $pkg 'dinput8.dll'), 'dll-v2')
        $st = Get-CoopInstallStatus $game $pkg $settings $data $false
        Require ($st.actions.install -eq 'Update' -and $st.actions.uninstall) 'outdated status'
        Remove-Item (Join-Path $game 'coop\coop.lua')
        $st = Get-CoopInstallStatus $game $pkg $settings $data $false
        Require ($st.actions.install -eq 'Repair' -and $st.actions.uninstall) 'partial repair status'
        [IO.File]::WriteAllText((Join-Path $game 'dinput8.dll'), 'foreign-dll')
        $st = Get-CoopInstallStatus $game $pkg $settings $data $false
        Require ($st.actions.install -eq '' -and -not $st.actions.uninstall -and $st.actions.blocked) 'modified blocked status'
        # Another mod's dinput8.dll is kept as dinput8.chain.dll (the co-op DLL loads it) and comes back on uninstall.
        $g2 = Join-Path $root 'game2'; New-Item -ItemType Directory $g2 | Out-Null
        $dll = Join-Path $g2 'dinput8.dll'
        [IO.File]::WriteAllText($dll, 'asi-loader')
        $asi = Get-CoopHash $dll
        Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('mdk2-coop bridge attached v2'))
        Require ((Get-CoopHash (Join-Path $g2 'dinput8.chain.dll')) -eq $asi -and -not (Test-Path "$dll.coop-original")) 'foreign dinput8.dll kept as dinput8.chain.dll'
        Require ((Get-CoopObservation $dll (Get-CoopHash $dll)).status -eq 'current') 'chained install is current'
        Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('mdk2-coop bridge attached v3'))
        Require ((Get-CoopHash (Join-Path $g2 'dinput8.chain.dll')) -eq $asi) 'update keeps the chain file'
        Require (Remove-CoopFile $dll) 'uninstall over a chain'
        Require ((Get-CoopHash $dll) -eq $asi -and -not (Test-Path (Join-Path $g2 'dinput8.chain.dll'))) 'the chained dinput8.dll comes back'
        # An old copy of this mod is a backup, never a chain (it would run the mod twice).
        [IO.File]::WriteAllText($dll, 'old mdk2-coop bridge attached')
        Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('mdk2-coop bridge attached v2'))
        Require ((Test-Path "$dll.coop-original") -and -not (Test-Path (Join-Path $g2 'dinput8.chain.dll'))) 'an old copy of the mod is not chained'
        Require (Remove-CoopFile $dll) 'uninstall over an old copy'
        # Setup before this release kept another mod's DLL as .coop-original: the next install moves it to the chain name.
        [IO.File]::WriteAllText($dll, 'widescreen-fix')
        $fix = Get-CoopHash $dll
        Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('mdk2-coop bridge attached v2'))
        Move-Item (Join-Path $g2 'dinput8.chain.dll') "$dll.coop-original"
        Install-CoopFile $dll ([Text.Encoding]::ASCII.GetBytes('mdk2-coop bridge attached v3'))
        Require ((Get-CoopHash (Join-Path $g2 'dinput8.chain.dll')) -eq $fix -and -not (Test-Path "$dll.coop-original")) 'old .coop-original migrated to dinput8.chain.dll'
        Require (Remove-CoopFile $dll) 'uninstall after migration'
        Require ((Get-CoopHash $dll) -eq $fix) 'migrated original comes back'
        # Files of a known release without receipts: a mod manager's (hard-linked: Vortex). Play, not "Needs attention".
        $pkg2 = Join-Path $root 'pkg2'; $g3 = Join-Path $root 'game3'
        New-Item -ItemType Directory (Join-Path $pkg2 'coop'), (Join-Path $g3 'coop') | Out-Null
        [IO.File]::WriteAllText((Join-Path $pkg2 'dinput8.dll'), 'mdk2-coop bridge attached r1')
        [IO.File]::WriteAllText((Join-Path $pkg2 'coop\coop.lua'), 'lua-r1')
        $manifest = @{ schema=1; id='mdk2-coop'; version='v9.0.0'; files=@(
            @{ path='dinput8.dll'; sha256=(Get-CoopHash (Join-Path $pkg2 'dinput8.dll')); size=1 },
            @{ path='coop/coop.lua'; sha256=(Get-CoopHash (Join-Path $pkg2 'coop\coop.lua')); size=1 }) }
        [IO.File]::WriteAllText((Join-Path $pkg2 'coop-mod.json'), ($manifest | ConvertTo-Json -Depth 4))
        Require ((Get-CoopPackageFiles $pkg2) -join ',' -eq 'dinput8.dll,coop\coop.lua') 'file list read from coop-mod.json'
        Copy-Item (Join-Path $pkg2 'dinput8.dll') $g3; Copy-Item (Join-Path $pkg2 'coop\coop.lua') (Join-Path $g3 'coop')
        $st = Get-CoopInstallStatus $g3 $pkg2 $settings $data $false
        Require ($st.owner -eq 'manual' -and $st.actions.install -eq 'Install' -and -not $st.actions.uninstall -and -not $st.actions.blocked) 'a plain copy of a release: adoption enabled'
        $g4 = Join-Path $root 'game4'; New-Item -ItemType Directory (Join-Path $g4 'coop') | Out-Null
        New-Item -ItemType HardLink -Path (Join-Path $g4 'dinput8.dll') -Target (Join-Path $pkg2 'dinput8.dll') | Out-Null
        New-Item -ItemType HardLink -Path (Join-Path $g4 'coop\coop.lua') -Target (Join-Path $pkg2 'coop\coop.lua') | Out-Null
        $st = Get-CoopInstallStatus $g4 $pkg2 $settings $data $false
        Require ($st.owner -eq 'vortex' -and -not $st.actions.install -and -not $st.actions.uninstall) 'hard-linked release files: Vortex, left alone'
        Reject { Adopt-CoopInstall $g4 $pkg2 } 'Vortex cannot be adopted'
        Require (-not (Test-Path (Join-Path $g4 'dinput8.dll.coop-receipt.json'))) 'Vortex has no new receipts'
        Adopt-CoopInstall $g3 $pkg2
        Require ((Get-CoopOwner $g3 $pkg2) -eq 'setup') 'adopted manual install belongs to Setup'
        Require (-not (Read-CoopReceipt (Join-Path $g3 'dinput8.dll')).original) 'adoption has no co-op original to restore'
        Require (Remove-CoopFile (Join-Path $g3 'dinput8.dll')) 'adopted files uninstall normally'
        Require (-not (Test-Path (Join-Path $g3 'dinput8.dll'))) 'adopted mod removed on uninstall'
        Remove-Item (Join-Path $g3 'coop\coop.lua.coop-receipt.json')
        # An older hand copy is recognised by its installed release manifest and updated after adoption.
        $g5 = Join-Path $root 'game5'; New-Item -ItemType Directory (Join-Path $g5 'coop') | Out-Null
        Copy-Item (Join-Path $pkg2 'dinput8.dll') $g5; Copy-Item (Join-Path $pkg2 'coop\coop.lua') (Join-Path $g5 'coop')
        Copy-Item (Join-Path $pkg2 'coop-mod.json') $g5
        Remove-Item (Join-Path $pkg2 'dinput8.dll')
        [IO.File]::WriteAllText((Join-Path $pkg2 'dinput8.dll'), 'mdk2-coop bridge attached r2')
        $manifest.files[0].sha256 = Get-CoopHash (Join-Path $pkg2 'dinput8.dll')
        $manifest.version = 'v9.1.0'
        [IO.File]::WriteAllText((Join-Path $pkg2 'coop-mod.json'), ($manifest | ConvertTo-Json -Depth 4))
        Require ((Get-CoopOwner $g5 $pkg2) -eq 'manual') 'old manual release recognised by its own manifest'
        Adopt-CoopInstall $g5 $pkg2
        Install-CoopFile (Join-Path $g5 'dinput8.dll') ([IO.File]::ReadAllBytes((Join-Path $pkg2 'dinput8.dll')))
        Require ((Get-CoopHash (Join-Path $g5 'dinput8.dll')) -eq (Get-CoopHash (Join-Path $pkg2 'dinput8.dll'))) 'older hand copy updates after adoption'
        [IO.File]::WriteAllText((Join-Path $g3 'coop\coop.lua'), 'edited')
        Require ((Get-CoopInstallStatus $g3 $pkg2 $settings $data $false).owner -eq 'none') 'changed files are not a release'
    } finally { Remove-Item -LiteralPath $root -Recurse -Force }
}
Run-Checks $source
Write-Output 'PASS: pure install policy and disposable filesystem fixtures'
if ($Mutants) {
    # Decision mutants run game-free in test_install_policy.py. These mutants
    # exercise filesystem guards in this adapter, and must parse before running.
    $twins = @(
        @("if (`$o.status -in 'modified', 'conflict', 'unknown')", 'if ($false)'),
        @("if (`$o.status -in 'modified', 'conflict', 'unknown', 'unmanaged')", 'if ($false)'),
        @("if (-not `$r) { return `$false }", 'if ($false) { return $false }'),
        @("if (`$r.schema -ne 1 -or", 'if ($false -and'),
        @('if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)', 'if ($false)'),
        @('$keep = $chain      # another mod', '$keep = "$target.coop-original"      # another mod'),
        @('if (Read-CoopReceipt $target) { [void](Move-CoopOriginalToChain $target) }', ''),
        @('        if ($match) {', '        if ($false) {'),
        @('if ((Get-CoopLinkCount (Join-Path $game ($f.path.Replace(''/'', ''\'')))) -gt 1) { return ''vortex'' }', ''),
        @('if ($owner -eq ''manual'' -and -not $actions.blocked)', 'if ($false)')
    )
    $index = 0
    foreach ($twin in $twins) {
        $index++
        Require ($source.Contains($twin[0])) "mutant $index target exists"
        $code = $source.Replace($twin[0], $twin[1])
        [void][scriptblock]::Create($code) # compilation failures do not count as killed
        $failed = $false
        try { Run-Checks $code } catch { $failed = $true }
        Require $failed "mutant $index must fail"
        Write-Output "KILLED: installer mutant $index"
    }
}
