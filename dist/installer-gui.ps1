# User-facing installer. Only the game-file operation is ever elevated.
param([string]$ProbePath, [ValidateSet('', 'install', 'uninstall')][string]$TestCompletion = '', [switch]$TestLayout, [string]$TestActionsPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$backend = Join-Path $PSScriptRoot 'mdk2-coop.ps1'
. (Join-Path $PSScriptRoot 'install-state.ps1')
$form = New-Object Windows.Forms.Form
$form.Text = 'MDK2 Co-op Setup'
$form.ClientSize = New-Object Drawing.Size(760, 540)
$form.FormBorderStyle = 'Sizable'
$form.MinimumSize = New-Object Drawing.Size(640, 520)
$form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [Drawing.ColorTranslator]::FromHtml('#0c171c')
$form.ForeColor = [Drawing.ColorTranslator]::FromHtml('#e9eef0')
$form.Font = New-Object Drawing.Font 'Segoe UI', 10
$exe = Join-Path $PSScriptRoot 'MDK2-Coop-Setup.exe'
if (Test-Path $exe) { try { $form.Icon = [Drawing.Icon]::ExtractAssociatedIcon($exe) } catch { } }
$header = New-Object Windows.Forms.Panel
$header.BackColor = [Drawing.ColorTranslator]::FromHtml('#142832')
$header.SetBounds(0, 0, 760, 91)
$header.Anchor = 'Top, Left, Right'
$brand = New-Object Windows.Forms.Label
$brand.Text = 'MDK2  /  CO-OP'
$brand.Font = New-Object Drawing.Font 'Arial', 19, ([Drawing.FontStyle]::Bold)
$brand.ForeColor = [Drawing.ColorTranslator]::FromHtml('#ff9025')
$brand.SetBounds(24, 13, 350, 37)
$header.Controls.Add($brand)
$subtitle = New-Object Windows.Forms.Label
$subtitle.Text = 'ONLINE CO-OP  //  SETUP'
$subtitle.Font = New-Object Drawing.Font 'Segoe UI', 9, ([Drawing.FontStyle]::Bold)
$subtitle.ForeColor = [Drawing.ColorTranslator]::FromHtml('#b5c6cd')
$subtitle.SetBounds(27, 53, 310, 24)
$header.Controls.Add($subtitle)
$stripe = New-Object Windows.Forms.Panel
$stripe.BackColor = [Drawing.ColorTranslator]::FromHtml('#ff9025')
$stripe.SetBounds(0, 88, 760, 3)
$stripe.Anchor = 'Top, Left, Right'
$header.Controls.Add($stripe)
$form.Controls.Add($header)
function Style-Button($button, $primary) {
    $button.FlatStyle = [Windows.Forms.FlatStyle]::Flat
    $button.UseVisualStyleBackColor = $false
    $button.Font = New-Object Drawing.Font 'Segoe UI', 10, ([Drawing.FontStyle]::Bold)
    if ($primary) {
        $button.BackColor = [Drawing.ColorTranslator]::FromHtml('#ff9025')
        $button.ForeColor = [Drawing.ColorTranslator]::FromHtml('#10191c')
        $button.FlatAppearance.BorderColor = [Drawing.ColorTranslator]::FromHtml('#ffb35b')
    } else {
        $button.BackColor = [Drawing.ColorTranslator]::FromHtml('#1c303b')
        $button.ForeColor = [Drawing.ColorTranslator]::FromHtml('#edf3f4')
        $button.FlatAppearance.BorderColor = [Drawing.ColorTranslator]::FromHtml('#57707a')
    }
}
$label = New-Object Windows.Forms.Label
$label.Text = 'Choose your MDK2 HD game folder, then install or uninstall co-op.'
$label.SetBounds(26, 113, 585, 30)
$form.Controls.Add($label)
$path = New-Object Windows.Forms.TextBox
$path.SetBounds(26, 154, 470, 28)
$path.BackColor = [Drawing.ColorTranslator]::FromHtml('#20333d')
$path.ForeColor = [Drawing.ColorTranslator]::FromHtml('#ffffff')
$path.BorderStyle = [Windows.Forms.BorderStyle]::FixedSingle
$form.Controls.Add($path)
$browse = New-Object Windows.Forms.Button
$browse.Text = 'Browse...'; $browse.SetBounds(505, 151, 108, 32)
Style-Button $browse $false
$form.Controls.Add($browse)
$status = New-Object Windows.Forms.Label
$status.Text = 'Close the game before installing. Your co-op saves and settings are kept on uninstall.'
$status.ForeColor = [Drawing.ColorTranslator]::FromHtml('#b2c2c9')
$status.SetBounds(26, 199, 585, 45)
$form.Controls.Add($status)
$install = New-Object Windows.Forms.Button
$install.Text = 'Install'; $install.SetBounds(430, 480, 143, 40)
$install.Anchor = 'Bottom, Right'
Style-Button $install $true
$form.Controls.Add($install)
$uninstall = New-Object Windows.Forms.Button
$uninstall.Text = 'Uninstall'; $uninstall.SetBounds(585, 480, 143, 40)
$uninstall.Anchor = 'Bottom, Right'
Style-Button $uninstall $false
$form.Controls.Add($uninstall)
$inventory = New-Object Windows.Forms.ListView
$inventory.View = 'Details'; $inventory.FullRowSelect = $true
$inventory.SetBounds(26, 305, 702, 155); $inventory.Anchor = 'Top, Bottom, Left, Right'
[void]$inventory.Columns.Add('Change', 210)
[void]$inventory.Columns.Add('Status', 100)
[void]$inventory.Columns.Add('Details', 800)
$form.Controls.Add($inventory)
$refresh = New-Object Windows.Forms.Button
$refresh.Text = 'Refresh'; $refresh.SetBounds(26, 480, 110, 40); $refresh.Anchor = 'Bottom, Left'
Style-Button $refresh $false
$form.Controls.Add($refresh)
$playExisting = New-Object Windows.Forms.Button
$playExisting.Name = 'PlayExisting'; $playExisting.Text = 'Play MDK2'
$playExisting.SetBounds(26, 250, 143, 40); $playExisting.Enabled = $false
Style-Button $playExisting $true
$form.Controls.Add($playExisting)
$shortcutExisting = New-Object Windows.Forms.Button
$shortcutExisting.Text = 'Create desktop shortcut'
$shortcutExisting.SetBounds(181, 250, 260, 40); $shortcutExisting.Enabled = $false
Style-Button $shortcutExisting $false
$form.Controls.Add($shortcutExisting)
function Start-Mdk2Game($dir) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) { throw 'Choose the folder containing mdk2hd.exe.' }
    Start-Process -FilePath (Join-Path $dir 'mdk2hd.exe') -WorkingDirectory $dir -ErrorAction Stop | Out-Null
}
function New-Mdk2DesktopShortcut($dir, $desktop = [Environment]::GetFolderPath('DesktopDirectory')) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) { throw 'Choose the folder containing mdk2hd.exe.' }
    $shell = New-Object -ComObject WScript.Shell
    try {
        $link = $shell.CreateShortcut((Join-Path $desktop 'MDK2 Co-op.lnk'))
        $link.TargetPath = Join-Path $dir 'mdk2hd.exe'
        $link.WorkingDirectory = $dir
        $link.IconLocation = (Join-Path $dir 'mdk2hd.exe') + ',0'
        $link.Description = 'Play MDK2 HD with the co-op launcher'
        $link.Save()
    } finally {
        if ($link) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($link) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}
function Play-Game($dir) {
    try { Start-Mdk2Game $dir; $form.Close() }
    catch { [Windows.Forms.MessageBox]::Show($form, "Could not start MDK2 HD. Keep Steam or the Beamdog client running.`n`n$($_.Exception.Message)", 'MDK2 Co-op', 'OK', 'Error') | Out-Null }
}
function Add-DesktopShortcut($dir) {
    try {
        New-Mdk2DesktopShortcut $dir
        [Windows.Forms.MessageBox]::Show($form, 'MDK2 Co-op shortcut added to your desktop.', 'MDK2 Co-op', 'OK', 'Information') | Out-Null
    } catch { [Windows.Forms.MessageBox]::Show($form, "Could not create the shortcut.`n`n$($_.Exception.Message)", 'MDK2 Co-op', 'OK', 'Error') | Out-Null }
}
$playExisting.Add_Click({ Play-Game ($path.Text.Trim().Trim('"')) })
$shortcutExisting.Add_Click({ Add-DesktopShortcut ($path.Text.Trim().Trim('"')) })
function Refresh-Status {
    if ($script:busy -or $script:refreshing) { return }
    $script:refreshing = $true
    $inventory.BeginUpdate()
    try {
        $inventory.Items.Clear(); $install.Enabled = $false; $uninstall.Enabled = $false
        $install.Text = 'Install'
        $playExisting.Enabled = $false; $shortcutExisting.Enabled = $false
        $status.Text = 'Choose a game folder to inspect its installation.'
        $dir = $path.Text.Trim().Trim('"')
        if (-not $dir -or -not (Test-Path -LiteralPath (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) { return }
        $playExisting.Enabled = $true; $shortcutExisting.Enabled = $true
        try {
            $snapshot = Get-CoopInstallStatus $dir $PSScriptRoot (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua') (Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop')
            foreach ($row in $snapshot.items) {
                $item = New-Object Windows.Forms.ListViewItem($row.item)
                [void]$item.SubItems.Add($row.status); [void]$item.SubItems.Add($row.detail)
                [void]$inventory.Items.Add($item)
            }
            $install.Enabled = [bool]$snapshot.actions.install
            $install.Text = if ($snapshot.actions.install) { $snapshot.actions.install } elseif ($snapshot.actions.blocked) { 'Needs attention' } else { 'Up to date' }
            $uninstall.Enabled = $snapshot.actions.uninstall
            if ($snapshot.actions.blocked) { $status.Text = 'Conflicting or unreadable files need attention. Modified files are kept.' }
            elseif (-not $snapshot.actions.install) { $status.Text = 'Co-op is already installed. Click Play MDK2 to open the launcher.' }
            else { $status.Text = 'You can play MDK2 now, or install co-op below. Keep Steam or the Beamdog client running.' }
        } catch { $status.Text = 'Could not inspect this folder. No changes were made.' }
    } finally {
        $inventory.EndUpdate()
        $script:refreshing = $false
    }
}
$refresh.Add_Click({ Refresh-Status })
$path.Add_TextChanged({ Refresh-Status })
$form.Add_Activated({ Refresh-Status })
$finish = New-Object Windows.Forms.Panel
$finish.SetBounds(0, 91, 760, 449)
$finish.Anchor = 'Top, Bottom, Left, Right'
$finishText = New-Object Windows.Forms.Label
$finishText.SetBounds(26, 35, 580, 100)
$finish.Controls.Add($finishText)
$close = New-Object Windows.Forms.Button
$close.Name = 'Close'; $close.Text = 'Close'; $close.SetBounds(470, 175, 143, 40)
Style-Button $close $false
$finish.Controls.Add($close)
$play = New-Object Windows.Forms.Button
$play.Name = 'Play'; $play.Text = 'Play MDK2'; $play.SetBounds(318, 175, 143, 40)
Style-Button $play $true
$script:installedDir = ''
$close.Add_Click({ $form.Close() })
$play.Add_Click({ Play-Game $script:installedDir })
$shortcut = New-Object Windows.Forms.Button
$shortcut.Text = 'Create desktop shortcut'; $shortcut.SetBounds(26, 175, 280, 40)
Style-Button $shortcut $false
$shortcut.Add_Click({ Add-DesktopShortcut $script:installedDir })
function Show-Completion($remove) {
    foreach ($control in @($label, $path, $browse, $status, $install, $uninstall, $inventory, $refresh, $playExisting, $shortcutExisting)) { $form.Controls.Remove($control) }
    $finish.Controls.Remove($play)
    $finish.Controls.Remove($shortcut)
    if ($remove) {
        $finishText.Text = 'Co-op has been uninstalled. Your co-op data was kept.'
    } else {
        $finishText.Text = 'Installation complete. Start MDK2 HD to open the co-op launcher. Keep Steam or the Beamdog client running.'
        $finish.Controls.Add($play)
        $finish.Controls.Add($shortcut)
    }
    $form.Controls.Add($finish)
    $form.AcceptButton = $close
}
if ($TestLayout) {
    foreach ($scale in @(1.0, 1.25, 1.5, 2.0)) {
        $factor = $scale / $(if ($script:previousScale) { $script:previousScale } else { 1.0 })
        $form.Scale((New-Object Drawing.SizeF($factor, $factor)))
        $script:previousScale = $scale
        $form.PerformLayout()
        foreach ($c in @($inventory, $refresh, $install, $uninstall, $playExisting, $shortcutExisting)) {
            if ($c.Left -lt 0 -or $c.Top -lt 0 -or $c.Right -gt $form.ClientSize.Width -or $c.Bottom -gt $form.ClientSize.Height) { throw 'Control outside setup client area' }
        }
        if ($playExisting.Right -gt $shortcutExisting.Left -or $shortcutExisting.Bottom -gt $inventory.Top) { throw 'Play controls overlap' }
        if ($inventory.Bottom -gt $install.Top -or $refresh.Right -gt $install.Left -or $install.Right -gt $uninstall.Left) { throw 'Setup controls overlap' }
    }
    Write-Output 'PASS: setup control geometry at 100, 125, 150 and 200 percent'
    $form.Dispose(); exit 0
}
if ($TestCompletion) {
    Show-Completion ($TestCompletion -eq 'uninstall')
    if (-not $finish.Controls.Contains($close) -or ($finish.Controls.Contains($play) -ne ($TestCompletion -eq 'install')) -or ($finish.Controls.Contains($shortcut) -ne ($TestCompletion -eq 'install'))) { exit 1 }
    Write-Output "completion $TestCompletion OK"
    $form.Dispose()
    exit 0
}
if ($TestActionsPath) {
    $path.Text = $TestActionsPath
    Refresh-Status
    if (-not $playExisting.Enabled -or -not $shortcutExisting.Enabled) { throw 'Existing game actions disabled' }
    function Start-Process($FilePath, $WorkingDirectory, $ErrorAction) {
        $script:startedFile = $FilePath; $script:startedDir = $WorkingDirectory
    }
    Start-Mdk2Game $TestActionsPath
    if ($script:startedFile -ne (Join-Path $TestActionsPath 'mdk2hd.exe') -or $script:startedDir -ne $TestActionsPath) { throw 'Wrong launch target' }
    New-Mdk2DesktopShortcut $TestActionsPath $TestActionsPath
    New-Mdk2DesktopShortcut $TestActionsPath $TestActionsPath
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path $TestActionsPath 'MDK2 Co-op.lnk'))
    if ($link.TargetPath -ne (Join-Path $TestActionsPath 'mdk2hd.exe') -or $link.WorkingDirectory -ne $TestActionsPath -or $link.IconLocation -ne ((Join-Path $TestActionsPath 'mdk2hd.exe') + ',0')) { throw 'Wrong shortcut target, working directory or icon' }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($link)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    $path.Text = Join-Path $TestActionsPath 'missing'
    Refresh-Status
    if ($playExisting.Enabled -or $shortcutExisting.Enabled) { throw 'Invalid folder actions enabled' }
    $rejected = $false
    try { New-Mdk2DesktopShortcut $path.Text $TestActionsPath } catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid shortcut accepted' }
    Write-Output 'PASS: existing game actions and desktop shortcut'
    $form.Dispose(); exit 0
}
$browse.Add_Click({
    $dialog = New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose the folder containing mdk2hd.exe'
    if ($dialog.ShowDialog($form) -eq 'OK') { $path.Text = $dialog.SelectedPath }
    $dialog.Dispose()
})

function Can-WriteGame($dir) {
    # Probe both directories and existing files; do not modify any existing file.
    try {
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return $false }
        foreach ($folder in @($dir, (Join-Path $dir 'coop'))) {
            if (-not (Test-Path $folder)) { continue }
            $probe = Join-Path $folder ([IO.Path]::GetRandomFileName())
            $stream = [IO.File]::Open($probe, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            $stream.Dispose(); [IO.File]::Delete($probe)
        }
        foreach ($file in @((Join-Path $dir 'dinput8.dll'), (Join-Path $dir 'coop\coop.lua'))) {
            if (Test-Path $file) {
                $stream = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
                $stream.Dispose()
            }
        }
        return $true
    } catch { return $false }
}
if ($ProbePath) {
    if (Can-WriteGame $ProbePath) { Write-Output 'writable'; exit 0 }
    Write-Output 'not writable'; exit 1
}

function Run-Backend($dir, $remove, $part, $elevate) {
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -GameDir "{1}" -Yes -{2}' -f $backend, $dir, $part
    $arguments += ' -PerUserData'
    if ($remove) { $arguments += ' -Uninstall' }
    $options = @{ FilePath = 'powershell.exe'; ArgumentList = $arguments; Wait = $true; PassThru = $true; WindowStyle = 'Hidden' }
    if ($elevate) {
        $options.Verb = 'RunAs'
        $process = Start-Process @options
    } else {
        $process = Invoke-CoopHiddenProcess 'powershell.exe' $arguments
    }
    if ($process.ExitCode -ne 0) { throw 'The installer could not complete this step. Check that the game is closed and the folder is writable.' }
}
$script:busy = $false
function Perform($remove) {
    if ($script:busy) { return }
    $dir = $path.Text.Trim().Trim('"')
    if (-not $dir -or -not (Test-Path (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) {
        [Windows.Forms.MessageBox]::Show($form, 'Choose the folder containing mdk2hd.exe.', 'MDK2 Co-op', 'OK', 'Warning') | Out-Null
        return
    }
    try {
        $preflight = Get-CoopInstallStatus $dir $PSScriptRoot (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua') (Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop')
        if ($preflight.actions.blocked) { throw 'Conflicting or unreadable files must be resolved before setup can continue.' }
        $script:busy = $true
        $form.ControlBox = $false
        foreach ($control in @($path, $browse, $install, $uninstall, $refresh, $playExisting, $shortcutExisting)) { $control.Enabled = $false }
        $status.ForeColor = [Drawing.Color]::White
        $status.Text = 'Working...'; $form.Refresh()
        if (-not $remove) { Run-Backend $dir $false 'PrepareDataOnly' $false }
        $needsAdmin = -not (Can-WriteGame $dir)
        if ($needsAdmin) { $status.Text = 'Windows will ask for administrator permission to change the game folder.'; $form.Refresh() }
        Run-Backend $dir $remove 'GameFilesOnly' $needsAdmin
        Run-Backend $dir $remove 'UserSettingsOnly' $false
        $script:installedDir = $dir
        Show-Completion $remove
    } catch {
        $status.Text = 'Setup did not complete. You can try again.'
        [Windows.Forms.MessageBox]::Show($form, $_.Exception.Message, 'MDK2 Co-op', 'OK', 'Error') | Out-Null
    } finally {
        foreach ($control in @($path, $browse, $install, $uninstall, $refresh, $playExisting, $shortcutExisting)) { $control.Enabled = $true }
        $form.ControlBox = $true
        $script:busy = $false
        Refresh-Status
        $form.Activate()
    }
}
$install.Add_Click({ Perform $false })
$uninstall.Add_Click({ Perform $true })
try {
    $found = Invoke-CoopHiddenProcess 'powershell.exe' ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -FindOnly -Yes' -f $backend)
    foreach ($line in ($found.Output -split '\r?\n')) { if ("$line" -match '^Game folder: (.+)$') { $path.Text = $Matches[1]; break } }
} catch { }
Refresh-Status
[void]$form.ShowDialog()
$form.Dispose()
