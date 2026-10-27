# Portable installer policy and file ownership journal. No UI or registry writes.
# Receipts are beside each managed file so elevated and user steps stay independent.
function Get-CoopHash($file) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Pure policy: observations are hashes, not paths. Unknown observation fails closed.
function Get-CoopFileState($expected, $observed, $receipt, $backup, $unknown = $false) {
    if ($unknown) { return 'unknown' }
    if ($receipt) {
        if ($receipt.schema -ne 1 -or -not $receipt.installed) { return 'unknown' }
        if ($receipt.original -and $backup -ne $receipt.original) { return 'conflict' }
        if (-not $observed) { return 'missing' }
        if ($observed -eq $expected) { return 'current' }
        if ($observed -eq $receipt.installed -or $observed -eq $receipt.pending) { return 'outdated' }
        if ($observed -eq $receipt.original) { return 'missing' } # interrupted before replacement
        return 'modified'
    }
    if ($backup) { return 'conflict' } # orphan backup: never overwrite it
    if (-not $observed) { return 'missing' }
    if ($observed -eq $expected) { return 'current' }
    return 'unmanaged' # first install may back this up, never silently delete it
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
function Install-CoopFile($target, [byte[]]$bytes) {
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
        if ($o.observed -eq $expected) { $o.observed = $null } # adopt identical unmanaged payload
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
    $begin = '-- >>> mdk2-coop >>>'; $end = '-- <<< mdk2-coop <<<'
    if (-not $text.Contains($begin) -and -not $text.Contains($end)) { return 'missing' }
    $pattern = '(?m)^' + [regex]::Escape($begin) + '\r?\nif \(coop_init\) then coop_init\(\) end\r?\n' + [regex]::Escape($end)
    if ([regex]::Matches($text, [regex]::Escape($begin)).Count -ne 1 -or
        [regex]::Matches($text, [regex]::Escape($end)).Count -ne 1 -or
        -not [regex]::IsMatch($text, $pattern)) { return 'conflict' }
    return 'current'
}
function Get-CoopActions($states, $owned) {
    $blocked = @($states | Where-Object { $_ -in 'unknown', 'conflict', 'modified' }).Count -gt 0
    $missing = $states -contains 'missing'
    $unmanaged = $states -contains 'unmanaged'
    $outdated = $states -contains 'outdated'
    $action = ''
    if (-not $blocked) {
        if (-not $owned -and ($missing -or $unmanaged)) { $action = 'Install' }
        elseif ($outdated) { $action = 'Update' }
        elseif ($missing -or $unmanaged) { $action = 'Repair' }
    }
    return [pscustomobject]@{ install=$action; uninstall=[bool]$owned; blocked=$blocked }
}
function Get-CoopInstallStatus($game, $package, $settings, $data) {
    $rows = @(); $owned = $false
    foreach ($relative in 'dinput8.dll', 'coop\coop.lua', 'coop\per-user-data') {
        $target = Join-Path $game $relative
        try {
            $expected = if ($relative -eq 'coop\per-user-data') { 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' } else { Get-CoopHash (Join-Path $package $relative) }
            if (-not $expected) { throw 'Missing package payload' }
            $o = Get-CoopObservation $target $expected
            if ($o.receipt) { $owned = $true }
            $rows += [pscustomobject]@{ item=$relative; status=$o.status; detail='File; ownership receipt and original backup beside it' }
            if ($o.receipt -and $o.receipt.original) {
                $rows += [pscustomobject]@{ item="$relative.coop-original"; status=$(if ($o.backup -eq $o.receipt.original) { 'current' } else { 'conflict' }); detail='Original file restored on uninstall' }
            }
            if (Test-Path -LiteralPath "$target.coop-backup") {
                $rows += [pscustomobject]@{ item="$relative.coop-backup"; status='conflict'; detail='Legacy backup preserved; resolve manually before changing this file' }
            }
        } catch { $rows += [pscustomobject]@{ item=$relative; status='unknown'; detail='Could not inspect payload or installed file' } }
    }
    try {
        $text = if (Test-Path -LiteralPath $settings) { [IO.File]::ReadAllText($settings) } else { '' }
        $loader = Get-CoopLoaderState $text
    } catch { $loader = 'unknown' }
    if ($loader -eq 'current') { $owned = $true }
    $rows += [pscustomobject]@{ item='settings.lua loader'; status=$loader; detail='Only the delimited loader is removed; player options are kept' }
    $actions = Get-CoopActions @($rows | ForEach-Object { $_.status }) $owned
    $rows += [pscustomobject]@{ item='settings.lua.coop-backup'; status=$(if (Test-Path -LiteralPath "$settings.coop-backup") { 'present' } else { 'not created' }); detail='First settings snapshot; retained for manual recovery' }
    $rows += [pscustomobject]@{ item='coop.ini and existing runtime data'; status='preserved'; detail='GUI copies missing runtime files to the user folder; never overwrites or removes them. Explicit CLI network options edit only net keys and launcher mode.' }
    $rows += [pscustomobject]@{ item='Registry, shortcuts, game executable'; status='untouched'; detail='No registry entries, shortcuts or game executable patches are created' }
    $rows += [pscustomobject]@{ item='Existing saves'; status='preserved'; detail='No save is removed or overwritten; older runtime data may be copied to the user folder' }
    return [pscustomobject]@{ schema=1; items=$rows; actions=$actions }
}
