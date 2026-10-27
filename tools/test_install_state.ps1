# Game-free: pure policy, disposable filesystem fixtures and source mutants.
# Windows PowerShell 5.1: powershell -NoProfile -File tools/test_install_state.ps1 -Mutants
param([switch]$Mutants)
$ErrorActionPreference = 'Stop'
$module = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist/install-state.ps1'
$source = [IO.File]::ReadAllText($module)
# scriptblock mutants have no module PSScriptRoot; pin only the helper location.
$helper = Join-Path (Split-Path -Parent $module) 'install-policy.exe'
$source = $source.Replace("Join-Path `$PSScriptRoot 'install-policy.exe'", "'$($helper.Replace("'", "''"))'")
function Require($value, $message) { if (-not $value) { throw "ASSERT: $message" } }
function Reject($operation, $message) {
    $rejected = $false
    try { & $operation | Out-Null } catch { $rejected = $true }
    Require $rejected $message
}
function Run-Checks($code) {
    . ([scriptblock]::Create($code))
    $root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
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
        # Sibling saves must be byte-identical across every install/uninstall operation.
        $save = Join-Path $root 'save.dat'; [IO.File]::WriteAllBytes($save, ([byte[]]@(0,255,12,34)))
        $saved = Get-CoopHash $save
        Remove-Item "$target.coop-receipt.json"
        Install-CoopFile $target ([byte[]]@(8,7,6))
        Require (Remove-CoopFile $target) 'sibling fixture uninstall'
        Require ((Get-CoopHash $save) -eq $saved) 'save unchanged'
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
        @('if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)', 'if ($false)')
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
