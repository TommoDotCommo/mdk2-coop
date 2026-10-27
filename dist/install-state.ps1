# Portable installer policy and file ownership journal. No UI or registry writes.
# Receipts are beside each managed file so elevated and user steps stay independent.
function Get-CoopHash($file) {
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Expected a regular file' }
    return (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Pure decisions run in a small native helper built from install-policy.h.
# No path, address or player data is sent anywhere; this process has no networking.
$script:CoopPolicy = Join-Path $PSScriptRoot 'install-policy.exe'
$script:CoopStates = @('current', 'missing', 'outdated', 'modified', 'conflict', 'unknown', 'unmanaged')
function Invoke-CoopPolicy([string[]]$values) {
    $result = & $script:CoopPolicy @values
    if ($LASTEXITCODE -ne 0 -or "$result" -notmatch '^\d+$') { throw 'Setup policy helper failed; unpack the whole release again.' }
    return [int]$result
}
function Get-CoopFileState($expected, $observed, $receipt, $backup, $unknown = $false) {
    $schema = if ($receipt) { if ($receipt.schema -eq 1) { 1 } else { -1 } } else { 0 }
    $values = @('file', "$expected", "$observed", "$schema", "$($receipt.installed)", "$($receipt.original)", "$($receipt.pending)", "$backup", "$([int][bool]$unknown)")
    for ($i = 1; $i -lt $values.Count; $i++) { if (-not $values[$i]) { $values[$i] = '-' } }
    return $script:CoopStates[(Invoke-CoopPolicy $values)]
}

function Read-CoopReceipt($target) {
    $file = "$target.coop-receipt.json"
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    $r = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if ($r.schema -ne 1 -or "$($r.installed)" -notmatch '^[a-f0-9]{64}$' -or
        ($r.original -and "$($r.original)" -notmatch '^[a-f0-9]{64}$') -or
        ($r.pending -and "$($r.pending)" -notmatch '^[a-f0-9]{64}$')) { throw 'Invalid ownership receipt' }
    return $r
}
function Write-CoopReceipt($target, $receipt) {
    $temp = "$target.coop-receipt.json.tmp"
    [IO.File]::WriteAllText($temp, ($receipt | ConvertTo-Json -Compress))
    Move-Item -LiteralPath $temp -Destination "$target.coop-receipt.json" -Force
}
function Get-CoopObservation($target, $expected) {
    try {
        Assert-CoopRegularPath $target
        if (Test-Path -LiteralPath "$target.coop-backup") { throw 'Legacy backup conflict' }
        Assert-CoopRegularPath "$target.coop-receipt.json"
        Assert-CoopRegularPath "$target.coop-original"
        $receipt = Read-CoopReceipt $target
        $observed = Get-CoopHash $target
        $backup = Get-CoopHash "$target.coop-original"
        $state = Get-CoopFileState $expected $observed $receipt $backup
        return [pscustomobject]@{ path=$target; status=$state; receipt=$receipt; observed=$observed; expected=$expected; backup=$backup }
    } catch {
        return [pscustomobject]@{ path=$target; status='unknown'; receipt=$null; observed=$null; expected=$expected; backup=$null }
    }
}
function Assert-CoopRegularPath($target) {
    # Refuse junctions/symlinks, including parent directories, before any write.
    $p = [IO.Path]::GetFullPath($target)
    while ($p) {
        if (Test-Path -LiteralPath $p) {
            if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path refused: $p" }
        }
        $parent = Split-Path -Parent $p
        if ($parent -eq $p) { break }
        $p = $parent
    }
}
function Assert-CoopNoRecoveryArtifacts($target) {
    foreach ($suffix in '.coop-stage', '.coop-receipt.json.tmp') {
        if (Test-Path -LiteralPath "$target$suffix") { throw "Recovery artifact preserved: $target$suffix. Review before retrying." }
    }
}
function Install-CoopFile($target, [byte[]]$bytes) {
    Assert-CoopNoRecoveryArtifacts $target
    Assert-CoopRegularPath $target
    if (Test-Path -LiteralPath "$target.coop-backup") { throw "Legacy backup preserved: $target.coop-backup. Resolve this before changing the file." }
    foreach ($suffix in '.coop-original', '.coop-receipt.json', '.coop-receipt.json.tmp', '.coop-stage') { Assert-CoopRegularPath "$target$suffix" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $expected = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose() }
    $o = Get-CoopObservation $target $expected
    if ($o.status -in 'modified', 'conflict', 'unknown') { throw "Refusing to replace $target ($($o.status)). Keep the file and resolve its ownership first." }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    $r = $o.receipt
    if (-not $r) {
        # Journal intent before replacing anything. A crash leaves the original in place.
        # Preserve even an identical unowned file; equality does not establish ownership.
        if ($o.observed) {
            Copy-Item -LiteralPath $target -Destination "$target.coop-original" -ErrorAction Stop
            if ((Get-CoopHash "$target.coop-original") -ne $o.observed) { throw 'Original backup verification failed' }
        }
        $r = [pscustomobject]@{ schema=1; original=$o.observed; installed=$expected; pending=$null }
    }
    $r.pending = $expected
    Write-CoopReceipt $target $r
    [IO.File]::WriteAllBytes("$target.coop-stage", $bytes)
    if ((Get-CoopHash "$target.coop-stage") -ne $expected) { throw 'Staged file verification failed' }
    Move-Item -LiteralPath "$target.coop-stage" -Destination $target -Force
    $r.installed = $expected; $r.pending = $null
    Write-CoopReceipt $target $r
}
function Remove-CoopFile($target) {
    Assert-CoopNoRecoveryArtifacts $target
    Assert-CoopRegularPath $target
    if (Test-Path -LiteralPath "$target.coop-backup") { throw "Legacy backup preserved: $target.coop-backup. Resolve this before changing the file." }
    foreach ($suffix in '.coop-original', '.coop-receipt.json', '.coop-stage') { Assert-CoopRegularPath "$target$suffix" }
    $r = Read-CoopReceipt $target
    if (-not $r) { return $false } # legacy files require an explicit migration decision
    $o = Get-CoopObservation $target $r.installed
    if ($o.status -in 'modified', 'conflict', 'unknown', 'unmanaged') { throw "Refusing to remove $target ($($o.status)); user changes were preserved." }
    if ($r.original) {
        # Keep the backup until restoration is complete, permitting retry after interruption.
        Copy-Item -LiteralPath "$target.coop-original" -Destination "$target.coop-stage" -Force
        Move-Item -LiteralPath "$target.coop-stage" -Destination $target -Force
        if ((Get-CoopHash $target) -ne $r.original) { throw 'Restoration verification failed' }
    } elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    Remove-Item -LiteralPath "$target.coop-receipt.json"
    if (Test-Path -LiteralPath "$target.coop-original") { Remove-Item -LiteralPath "$target.coop-original" }
    return $true
}

function Get-CoopLoaderState($text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    if ($bytes.Length -gt 8192 -or $text.Contains([string][char]0)) { return 'unknown' }
    $hex = ([BitConverter]::ToString($bytes)).Replace('-', '')
    # Hex keeps command-line quoting independent of untrusted settings text.
    if (-not $hex) { return 'missing' }
    return $script:CoopStates[(Invoke-CoopPolicy @('loader', $hex))]
}
function Get-CoopActions($states, $owned) {
    $mask = 0
    foreach ($state in $states) {
        $index = [array]::IndexOf($script:CoopStates, $state)
        if ($index -lt 0) { $index = 5 }
        $mask = $mask -bor (1 -shl $index)
    }
    $decision = Invoke-CoopPolicy @('actions', "$mask", "$([int][bool]$owned)")
    return [pscustomobject]@{ install=@('', 'Install', 'Update', 'Repair')[$decision -band 15]; uninstall=[bool]($decision -band 16); blocked=[bool]($decision -band 32) }
}
# Shared read-only migration plan: UI and writer enumerate exactly the same files.
function Get-CoopDataPlan($game, $data) {
    Assert-CoopRegularPath $data
    $old = Join-Path $game 'coop'
    Assert-CoopRegularPath $old
    if (-not (Test-Path -LiteralPath $old)) { return @() }
    foreach ($file in Get-ChildItem -LiteralPath $old -File -Force -ErrorAction Stop) {
        if ($file.Name -in 'coop.lua', 'per-user-data' -or $file.Name -match '\.coop-(original|receipt|stage|backup)') { continue }
        Assert-CoopRegularPath $file.FullName
        $target = Join-Path $data $file.Name
        Assert-CoopRegularPath $target
        $exists = Test-Path -LiteralPath $target
        if ($exists -and -not (Test-Path -LiteralPath $target -PathType Leaf)) { throw 'Runtime target is not a file' }
        [pscustomobject]@{ name=$file.Name; source=$file.FullName; target=$target; copy=(-not $exists) }
    }
}
function Get-CoopInstallStatus($game, $package, $settings, $data, $perUser = $true) {
    $rows = @(); $owned = $false
    $files = @('dinput8.dll', 'coop\coop.lua')
    if ($perUser) { $files += 'coop\per-user-data' }
    foreach ($relative in $files) {
        $target = Join-Path $game $relative
        try {
            $expected = if ($relative -eq 'coop\per-user-data') { 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' } else { Get-CoopHash (Join-Path $package $relative) }
            if (-not $expected) { throw 'Missing package payload' }
            $o = Get-CoopObservation $target $expected
            if ($o.receipt) { $owned = $true }
            $rows += [pscustomobject]@{ item=$relative; status=$o.status; detail='Deployed payload; original restored on uninstall' }
            $rows += [pscustomobject]@{ item="$relative.coop-receipt.json"; status=$(if ($o.receipt) { 'current' } elseif ($o.status -eq 'unknown') { 'unknown' } else { 'missing' }); detail='Ownership journal, created before replacement; retired on uninstall' }
            if ($o.receipt -and $o.receipt.original) {
                $rows += [pscustomobject]@{ item="$relative.coop-original"; status=$(if ($o.backup -eq $o.receipt.original) { 'current' } else { 'conflict' }); detail='Verified original; kept until restoration succeeds' }
            }
            foreach ($suffix in '.coop-backup', '.coop-stage', '.coop-receipt.json.tmp') {
                if (Test-Path -LiteralPath "$target$suffix") {
                    $rows += [pscustomobject]@{ item="$relative$suffix"; status='conflict'; detail='Recovery artifact preserved; review before continuing' }
                }
            }
        } catch { $rows += [pscustomobject]@{ item=$relative; status='unknown'; detail='Could not inspect payload or installed file' } }
    }
    try {
        Assert-CoopRegularPath $settings
        Assert-CoopRegularPath "$settings.coop-backup"
        $text = if (Test-Path -LiteralPath $settings) { [IO.File]::ReadAllText($settings) } else { '' }
        $loader = Get-CoopLoaderState $text
    } catch { $loader = 'unknown' }
    if ($loader -eq 'current') { $owned = $true }
    $rows += [pscustomobject]@{ item='settings.lua loader'; status=$loader; detail='Only the exact loader block is removed; player options kept' }
    if ($perUser) {
        try {
            foreach ($entry in @(Get-CoopDataPlan $game $data)) {
                $rows += [pscustomobject]@{ item="User data: $($entry.name)"; status=$(if ($entry.copy) { 'missing' } else { 'current' }); detail=$(if ($entry.copy) { 'Copy missing runtime file; retained on uninstall' } else { 'Existing user file preserved; never overwritten or removed' }) }
            }
        } catch { $rows += [pscustomobject]@{ item='User data migration'; status='unknown'; detail='Could not inspect all runtime files; no action is safe' } }
    }
    $actions = Get-CoopActions @($rows | ForEach-Object { $_.status }) $owned
    $rows += [pscustomobject]@{ item='settings.lua.coop-backup'; status=$(if (Test-Path -LiteralPath "$settings.coop-backup") { 'present' } else { 'will create' }); detail='First settings snapshot, retained for manual recovery' }
    $rows += [pscustomobject]@{ item='Registry, shortcuts, game executable, existing saves'; status='untouched'; detail='No registry, shortcut or executable changes; original saves never overwritten or removed' }
    return [pscustomobject]@{ schema=1; items=$rows; actions=$actions }
}
