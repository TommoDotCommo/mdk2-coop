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
# Hidden window style alone can still create a console briefly. Suppress console
# creation and capture both streams so callers never borrow the desktop.
function Invoke-CoopHiddenProcess([string]$file, [string]$arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $file
    $info.Arguments = $arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{ ExitCode=$process.ExitCode; Output=$output.Result; Error=$errors.Result }
    } finally { $process.Dispose() }
}
function Invoke-CoopPolicy([string[]]$values) {
    # The policy protocol contains only words, numbers, hashes and hex text.
    foreach ($value in $values) {
        if ($value -notmatch '^[a-zA-Z0-9-]+$') { throw 'Invalid setup policy argument.' }
    }
    $result = Invoke-CoopHiddenProcess $script:CoopPolicy ($values -join ' ')
    if ($result.ExitCode -ne 0 -or $result.Output.Trim() -notmatch '^\d+$') { throw 'Setup policy helper failed; unpack the whole release again.' }
    return [int]$result.Output.Trim()
}
# ---- which game is this? (read from the exe on disk, never run) ----
# The native helper applies the same test the DLL uses at every start (bridge/build_sigs.h): the builtin table entry of
# "copytagmethods" must point at that string. Known = the mod will switch on for this exe.
$script:CoopUnknownBuildWarning = "This game build is not one the co-op mod knows, so co-op will stay off when you start it " +
    "(the game itself plays as before). Supported: MDK2 HD on Steam (build 3902) and the Overhaul Games build. " +
    "If your game is the Steam version, verify its files in Steam (Properties > Installed Files) and opt out of betas " +
    "(Properties > Betas > None), then run Setup again."
function Get-CoopGameBuild([string]$exe) {
    $result = Invoke-CoopHiddenProcess $script:CoopPolicy ('build "{0}"' -f $exe)
    $lines = @($result.Output -split '\r?\n' | Where-Object { $_ })
    if ($result.ExitCode -eq 0 -and $lines.Count -ge 2 -and $lines[0] -match '^-?\d+$' -and [int]$lines[0] -ge 0) {
        return [pscustomobject]@{ Known = $true; Name = $lines[1]; Text = "MDK2 HD $($lines[1])" }
    }
    try { $hash = Get-CoopHash $exe } catch { $hash = $null }
    if (-not $hash) { return [pscustomobject]@{ Known = $false; Name = 'unreadable'; Text = 'unknown build (the game file cannot be read)' } }
    return [pscustomobject]@{ Known = $false; Name = 'unknown'; Text = "unknown build (SHA-256 $hash)" }
}
# The question to ask before installing: $null for a known build (and for uninstalling), else the plain-words warning.
function Get-CoopBuildWarning($build, [bool]$remove) {
    if ($remove -or $build.Known) { return $null }
    return $script:CoopUnknownBuildWarning
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
# Another mod's dinput8.dll (an ASI loader, a widescreen fix) is kept as dinput8.chain.dll, which the co-op DLL loads after
# itself; anything else Setup replaces is kept as <file>.coop-original. The receipt's "original" hash says which file is the
# original either way, so the receipt format is the same.
$script:CoopMark = 'mdk2-coop bridge attached'
function Test-CoopOurDll($file) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $false }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file)).Contains($script:CoopMark)
}
function Get-CoopChainPath($target) {
    if ((Split-Path -Leaf $target) -ne 'dinput8.dll') { return $null }
    return Join-Path (Split-Path -Parent $target) 'dinput8.chain.dll'
}
function Get-CoopKeptPath($target) {
    $chain = Get-CoopChainPath $target
    if ($chain -and -not (Test-Path -LiteralPath "$target.coop-original") -and (Test-Path -LiteralPath $chain)) { return $chain }
    return "$target.coop-original"
}
# Setup before this release kept another mod's dinput8.dll as dinput8.dll.coop-original, where nothing loaded it: move it to
# dinput8.chain.dll (verified) so it works again. A copy of this mod stays where it is (chaining it would run the mod twice).
function Move-CoopOriginalToChain($target) {
    $chain = Get-CoopChainPath $target
    $old = "$target.coop-original"
    if (-not $chain -or -not (Test-Path -LiteralPath $old -PathType Leaf) -or (Test-Path -LiteralPath $chain)) { return $false }
    Assert-CoopRegularPath $old; Assert-CoopRegularPath $chain
    if (Test-CoopOurDll $old) { return $false }
    $hash = Get-CoopHash $old
    Copy-Item -LiteralPath $old -Destination $chain -ErrorAction Stop
    if ((Get-CoopHash $chain) -ne $hash) { Remove-Item -LiteralPath $chain; throw 'Chain copy verification failed' }
    Remove-Item -LiteralPath $old
    return $true
}

# coop-mod.json (tools/mod_manifest.py): the files the package puts into the game folder, with their hashes. Setup reads its
# file list from the package's copy; a copy in the game folder (a mod manager installed the whole package) names that release.
$script:CoopDefaultFiles = @('dinput8.dll', 'coop\coop.lua')      # the layout before coop-mod.json
function Read-CoopManifest($folder) {
    $file = Join-Path $folder 'coop-mod.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $null }
    try { $m = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { return $null }
    if ($m.schema -ne 1 -or -not $m.files) { return $null }
    foreach ($f in $m.files) { if ("$($f.path)" -notmatch '^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$' -or "$($f.sha256)" -notmatch '^[a-f0-9]{64}$') { return $null } }
    return $m
}
function Get-CoopPackageFiles($package) {
    $m = Read-CoopManifest $package
    if (-not $m) { return $script:CoopDefaultFiles }
    return @($m.files | ForEach-Object { "$($_.path)".Replace('/', '\') })
}
# Who installed the files: 'setup' (receipts), 'vortex' (no receipt, release files hard-linked), 'mod-manager' (no receipt,
# the files of a known release copied in), or 'none' (not installed, or files Setup cannot place).
function Get-CoopLinkCount($file) {
    try {
        $out = (& fsutil.exe hardlink list $file 2>$null)
        if ($LASTEXITCODE -eq 0 -and $out) { return @($out).Count }
    } catch { }
    return 1
}
function Get-CoopOwner($game, $package) {
    $manifests = @(@(Read-CoopManifest $package; Read-CoopManifest $game) | Where-Object { $_ })
    $files = Get-CoopPackageFiles $package
    foreach ($relative in $files) { if (Test-Path -LiteralPath (Join-Path $game "$relative.coop-receipt.json")) { return 'setup' } }
    foreach ($m in $manifests) {
        $match = $true
        foreach ($f in $m.files) {
            $target = Join-Path $game ("$($f.path)".Replace('/', '\'))
            try { $h = Get-CoopHash $target } catch { $h = $null }
            if ($h -ne "$($f.sha256)") { $match = $false; break }
        }
        if ($match) {
            if ((Get-CoopLinkCount (Join-Path $game 'dinput8.dll')) -gt 1) { return 'vortex' }
            return 'mod-manager'
        }
    }
    return 'none'
}

function Get-CoopObservation($target, $expected) {
    try {
        Assert-CoopRegularPath $target
        if (Test-Path -LiteralPath "$target.coop-backup") { throw 'Legacy backup conflict' }
        Assert-CoopRegularPath "$target.coop-receipt.json"
        $kept = Get-CoopKeptPath $target
        Assert-CoopRegularPath $kept
        $receipt = Read-CoopReceipt $target
        $observed = Get-CoopHash $target
        $backup = Get-CoopHash $kept
        $state = Get-CoopFileState $expected $observed $receipt $backup
        return [pscustomobject]@{ path=$target; status=$state; receipt=$receipt; observed=$observed; expected=$expected; backup=$backup; kept=$kept }
    } catch {
        return [pscustomobject]@{ path=$target; status='unknown'; receipt=$null; observed=$null; expected=$expected; backup=$null; kept=$null }
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
    if (Read-CoopReceipt $target) { [void](Move-CoopOriginalToChain $target) }
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
            $keep = "$target.coop-original"
            $chain = Get-CoopChainPath $target
            if ($chain -and -not (Test-CoopOurDll $target)) {
                if (Test-Path -LiteralPath $chain) { throw "$chain already exists. Keep the file and resolve it first." }
                $keep = $chain      # another mod's dinput8.dll: the co-op DLL loads it from there
            }
            Assert-CoopRegularPath $keep
            Copy-Item -LiteralPath $target -Destination $keep -ErrorAction Stop
            if ((Get-CoopHash $keep) -ne $o.observed) { throw 'Original backup verification failed' }
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
        $kept = Get-CoopKeptPath $target
        Copy-Item -LiteralPath $kept -Destination "$target.coop-stage" -Force
        Move-Item -LiteralPath "$target.coop-stage" -Destination $target -Force
        if ((Get-CoopHash $target) -ne $r.original) { throw 'Restoration verification failed' }
    } elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    Remove-Item -LiteralPath "$target.coop-receipt.json"
    if ($r.original -and (Test-Path -LiteralPath $kept)) { Remove-Item -LiteralPath $kept }
    elseif (Test-Path -LiteralPath "$target.coop-original") { Remove-Item -LiteralPath "$target.coop-original" }
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
    $files = @(Get-CoopPackageFiles $package)
    $owner = Get-CoopOwner $game $package
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
                $isChain = $o.kept -and (Split-Path -Leaf $o.kept) -eq 'dinput8.chain.dll'
                $rows += [pscustomobject]@{ item=$(if ($isChain) { 'dinput8.chain.dll' } else { "$relative.coop-original" }); status=$(if ($o.backup -eq $o.receipt.original) { 'current' } else { 'conflict' })
                    detail=$(if ($isChain) { "Another mod's dinput8.dll: the co-op DLL loads it; put back on uninstall" } else { 'Verified original; kept until restoration succeeds' }) }
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
    if ($owner -in 'mod-manager', 'vortex') {
        # a mod manager's files: it updates and removes them; Setup shows that and offers Play
        $actions = [pscustomobject]@{ install=''; uninstall=$false; blocked=$false }
    }
    $iniPath = if ($perUser) { Join-Path $data 'coop.ini' } else { Join-Path $game 'coop\coop.ini' }
    $rows += [pscustomobject]@{ item='coop.ini'; status=$(if (Test-Path -LiteralPath $iniPath) { 'present' } else { 'default on launch' }); detail='Existing co-op settings preserved; only explicit CLI flags edit [net] and [launcher] mode' }
    $rows += [pscustomobject]@{ item='settings.lua.coop-backup'; status=$(if (Test-Path -LiteralPath "$settings.coop-backup") { 'present' } else { 'will create' }); detail='First settings snapshot, retained for manual recovery' }
    $rows += [pscustomobject]@{ item='Registry, shortcuts, game executable, existing saves'; status='untouched'; detail='No registry, shortcut or executable changes; original saves never overwritten or removed' }
    return [pscustomobject]@{ schema=1; owner=$owner; items=$rows; actions=$actions }
}
