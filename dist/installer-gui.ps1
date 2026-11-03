# User-facing installer. Only the game-file operation is ever elevated.
param([string]$ProbePath, [ValidateSet('', 'install', 'uninstall')][string]$TestCompletion = '', [switch]$TestLayout, [string]$TestActionsPath, [string]$TestBuildGate, [string]$SetupExe,
      [string]$TestOwnerPath, [switch]$TestNoOptions)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$backend = Join-Path $PSScriptRoot 'mdk2-coop.ps1'
. (Join-Path $PSScriptRoot 'install-state.ps1')
$form = New-Object Windows.Forms.Form
$form.Text = 'MDK2 Co-op Setup'
$form.ClientSize = New-Object Drawing.Size(760, 680)
$form.FormBorderStyle = 'Sizable'
$form.MinimumSize = New-Object Drawing.Size(776, 719)
$form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [Drawing.ColorTranslator]::FromHtml('#0c171c')
$form.ForeColor = [Drawing.ColorTranslator]::FromHtml('#e9eef0')
$form.Font = New-Object Drawing.Font 'Segoe UI', 10
$exe = if ($SetupExe -and (Test-Path -LiteralPath $SetupExe -PathType Leaf)) { $SetupExe } else { Join-Path $PSScriptRoot 'MDK2-Coop-Setup.exe' }   # the single-file Setup unpacks elsewhere and passes its own path
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
$label.SetBounds(26, 223, 702, 24)
$form.Controls.Add($label)
$path = New-Object Windows.Forms.TextBox
$path.SetBounds(26, 253, 470, 28)
$path.BackColor = [Drawing.ColorTranslator]::FromHtml('#20333d')
$path.ForeColor = [Drawing.ColorTranslator]::FromHtml('#ffffff')
$path.BorderStyle = [Windows.Forms.BorderStyle]::FixedSingle
$form.Controls.Add($path)
$browse = New-Object Windows.Forms.Button
$browse.Text = 'Browse...'; $browse.SetBounds(505, 250, 108, 32)
Style-Button $browse $false
$form.Controls.Add($browse)
$buildLabel = New-Object Windows.Forms.Label
$buildLabel.Name = 'BuildLabel'; $buildLabel.Text = ''
$buildLabel.ForeColor = [Drawing.ColorTranslator]::FromHtml('#b2c2c9')
$buildLabel.Font = New-Object Drawing.Font 'Segoe UI', 9
$buildLabel.SetBounds(26, 285, 702, 22)
$form.Controls.Add($buildLabel)
$status = New-Object Windows.Forms.Label
$status.Text = 'Close the game before installing. Your co-op saves and settings are kept on uninstall.'
$status.ForeColor = [Drawing.ColorTranslator]::FromHtml('#b2c2c9')
$status.SetBounds(26, 310, 702, 48)
$form.Controls.Add($status)
$install = New-Object Windows.Forms.Button
$install.Text = 'Install'; $install.SetBounds(430, 620, 143, 40)
$install.Anchor = 'Bottom, Right'
Style-Button $install $true
$form.Controls.Add($install)
$uninstall = New-Object Windows.Forms.Button
$uninstall.Text = 'Uninstall'; $uninstall.SetBounds(585, 620, 143, 40)
$uninstall.Anchor = 'Bottom, Right'
Style-Button $uninstall $false
$form.Controls.Add($uninstall)
$inventory = New-Object Windows.Forms.ListView
$inventory.View = 'Details'; $inventory.FullRowSelect = $true
$inventory.SetBounds(26, 403, 702, 100); $inventory.Anchor = 'Top, Left, Right'
[void]$inventory.Columns.Add('Change', 210)
[void]$inventory.Columns.Add('Status', 100)
[void]$inventory.Columns.Add('Details', 800)
$form.Controls.Add($inventory)
$refresh = New-Object Windows.Forms.Button
$refresh.Text = 'Refresh'; $refresh.SetBounds(26, 620, 110, 40); $refresh.Anchor = 'Bottom, Left'
Style-Button $refresh $false
$form.Controls.Add($refresh)
$playExisting = New-Object Windows.Forms.Button
$playExisting.Name = 'PlayExisting'; $playExisting.Text = 'Play MDK2'
$playExisting.SetBounds(26, 360, 143, 32); $playExisting.Enabled = $false
Style-Button $playExisting $true
$form.Controls.Add($playExisting)
$shortcutExisting = New-Object Windows.Forms.Button
$shortcutExisting.Text = 'Create desktop shortcut'
$shortcutExisting.SetBounds(181, 360, 260, 32); $shortcutExisting.Enabled = $false
Style-Button $shortcutExisting $false
$form.Controls.Add($shortcutExisting)
# Every detected install gets a selectable card, including unsupported builds.
$cards = New-Object Windows.Forms.FlowLayoutPanel
$cards.SetBounds(26, 103, 702, 110)
$cards.Anchor = 'Top, Left, Right'; $cards.AutoScroll = $true
$cards.FlowDirection = 'TopDown'; $cards.WrapContents = $false
$form.Controls.Add($cards)
$desktopOption = New-Object Windows.Forms.CheckBox
$desktopOption.Text = 'Desktop shortcut'
$hostingOption = New-Object Windows.Forms.CheckBox
$hostingOption.Text = 'Allow hosting through Windows Firewall (Private and Public)'
$inviteOption = New-Object Windows.Forms.CheckBox
$inviteOption.Text = 'Open invite links in the launcher'
$optionY = 515
foreach ($option in @($desktopOption, $hostingOption, $inviteOption)) {
    $option.Checked = $true; $option.SetBounds(26, $optionY, 702, 29)
    $form.Controls.Add($option); $optionY += 29
}
function Start-Mdk2Game($dir) {
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) { throw 'Choose the folder containing mdk2hd.exe.' }
    Start-Process -FilePath (Join-Path $dir 'mdk2hd.exe') -WorkingDirectory $dir -ErrorAction Stop | Out-Null
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
        $buildLabel.Text = ''
        $dir = $path.Text.Trim().Trim('"')
        if (-not $dir -or -not (Test-Path -LiteralPath (Join-Path $dir 'mdk2hd.exe') -PathType Leaf)) { return }
        try {
            $build = Get-CoopGameBuild (Join-Path $dir 'mdk2hd.exe')
            $buildLabel.Text = "Found: $($build.Text)"
            $buildLabel.ForeColor = [Drawing.ColorTranslator]::FromHtml($(if ($build.Known) { '#7fd19a' } else { '#ffb35b' }))
        } catch { $buildLabel.Text = 'Found: the game build could not be identified' }
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
            if ($snapshot.owner -ne 'vortex' -and -not $snapshot.actions.blocked -and -not $snapshot.actions.install -and
                (($desktopOption.Checked -and -not $snapshot.options.desktop_shortcut) -or
                 ($hostingOption.Checked -and -not $snapshot.options.allow_hosting) -or
                 ($inviteOption.Checked -and -not $snapshot.options.invite_links))) {
                $install.Enabled = $true; $install.Text = 'Repair'
            }
            if ($snapshot.owner -eq 'vortex') {
                # not Setup's files: the mod manager updates and removes them; playing works as it is
                $install.Text = 'By Vortex'; $install.Enabled = $false; $uninstall.Enabled = $false
                $status.Text = 'Installed by Vortex: update or remove it there. Click Play MDK2 to play.'
            }
            elseif ($snapshot.owner -eq 'manual') { $status.Text = 'Copied by hand. Setup will look after these files from now on.' }
            elseif ($snapshot.actions.blocked) { $status.Text = 'Conflicting or unreadable files need attention. Modified files are kept.' }
            elseif (-not $snapshot.actions.install) { $status.Text = 'Co-op is already installed. Click Play MDK2 to open the launcher.' }
            else { $status.Text = 'You can play MDK2 now, or install co-op below. Keep Steam or the Beamdog client running.' }
        } catch { $status.Text = 'Could not inspect this folder. No changes were made.' }
    } finally {
        $inventory.EndUpdate()
        $script:refreshing = $false
    }
}
$refresh.Add_Click({ Refresh-Status })
foreach ($option in @($desktopOption, $hostingOption, $inviteOption)) { $option.Add_CheckedChanged({ Refresh-Status }) }
$path.Add_TextChanged({ Refresh-Status })
$form.Add_Activated({ Refresh-Status })
$finish = New-Object Windows.Forms.Panel
$finish.SetBounds(0, 91, 760, 589)
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
    foreach ($control in @($label, $path, $browse, $buildLabel, $status, $install, $uninstall, $inventory, $refresh, $playExisting, $shortcutExisting, $cards, $desktopOption, $hostingOption, $inviteOption)) { $form.Controls.Remove($control) }
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
    $form.AcceptButton = if ($remove) { $close } else { $play }
}
if ($TestBuildGate) {
    # What Install would show for this game folder: the "Found:" line and, for an unknown build, the warning it must ask about.
    $build = Get-CoopGameBuild (Join-Path $TestBuildGate 'mdk2hd.exe')
    Write-Output "Found: $($build.Text)"
    $warning = Get-CoopBuildWarning $build $false
    Write-Output $(if ($warning) { "WARNING: $warning" } else { 'NO WARNING' })
    $form.Dispose(); exit 0
}
if ($TestLayout) {
    if (-not $desktopOption.Checked -or -not $hostingOption.Checked -or -not $inviteOption.Checked) { throw 'All three install options must start ticked' }
    foreach ($scale in @(1.0, 1.25, 1.5, 2.0)) {
        $factor = $scale / $(if ($script:previousScale) { $script:previousScale } else { 1.0 })
        $form.Scale((New-Object Drawing.SizeF($factor, $factor)))
        $script:previousScale = $scale
        $form.PerformLayout()
        foreach ($c in @($inventory, $refresh, $install, $uninstall, $playExisting, $shortcutExisting, $cards, $desktopOption, $hostingOption, $inviteOption)) {
            if ($c.Left -lt 0 -or $c.Top -lt 0 -or $c.Right -gt $form.ClientSize.Width -or $c.Bottom -gt $form.ClientSize.Height) { throw "Control outside setup client area at $scale : $($c.Text) $($c.Bounds) / $($form.ClientSize)" }
        }
        if ($playExisting.Right -gt $shortcutExisting.Left -or $shortcutExisting.Bottom -gt $inventory.Top) { throw 'Play controls overlap' }
        if ($cards.Bottom -gt $label.Top -or $inventory.Bottom -gt $desktopOption.Top -or $inviteOption.Bottom -gt $install.Top) { throw 'Cards or options overlap' }
        if ($inventory.Bottom -gt $install.Top -or $refresh.Right -gt $install.Left -or $install.Right -gt $uninstall.Left) { throw 'Setup controls overlap' }
    }
    Write-Output 'PASS: setup control geometry at 100, 125, 150 and 200 percent'
    $form.Dispose(); exit 0
}
if ($TestCompletion) {
    Show-Completion ($TestCompletion -eq 'uninstall')
    if (-not $finish.Controls.Contains($close) -or ($finish.Controls.Contains($play) -ne ($TestCompletion -eq 'install')) -or ($finish.Controls.Contains($shortcut) -ne ($TestCompletion -eq 'install'))) { exit 1 }
    if ($TestCompletion -eq 'install' -and $form.AcceptButton -ne $play) { throw 'Play must be the primary completion action' }
    Write-Output "completion $TestCompletion OK"
    $form.Dispose()
    exit 0
}
if ($TestOwnerPath) {     # tools/test_install_state.ps1: what Setup shows for a folder, read back from the controls
    $path.Text = $TestOwnerPath
    if ($TestNoOptions) { $desktopOption.Checked = $false; $hostingOption.Checked = $false; $inviteOption.Checked = $false }
    Refresh-Status
    Write-Output ("button=$($install.Text)|enabled=$($install.Enabled)|uninstall=$($uninstall.Enabled)|play=$($playExisting.Enabled)|status=$($status.Text)")
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
    if ($script:acceptUnknown) { $arguments += ' -AcceptUnknownBuild' }
    if ($remove) { $arguments += ' -Uninstall' }
    if (-not $remove) {
        if ($part -eq 'GameFilesOnly' -and $hostingOption.Checked) { $arguments += ' -AllowHosting' }
        if ($part -eq 'UserSettingsOnly') {
            if ($desktopOption.Checked) { $arguments += ' -DesktopShortcut' }
            if ($inviteOption.Checked) { $arguments += ' -InviteLinks' }
        }
    }
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
$script:acceptUnknown = $false
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
        $script:acceptUnknown = $false
        $warning = Get-CoopBuildWarning (Get-CoopGameBuild (Join-Path $dir 'mdk2hd.exe')) $remove
        if ($warning) {
            $answer = [Windows.Forms.MessageBox]::Show($form, "$warning`n`nInstall anyway?", 'MDK2 Co-op: unknown game build', 'YesNo', 'Warning', 'Button2')
            if ($answer -ne 'Yes') { $status.Text = 'Not installed. Co-op needs a supported game build.'; return }
            $script:acceptUnknown = $true
        }
        $script:busy = $true
        $form.ControlBox = $false
        foreach ($control in @($path, $browse, $install, $uninstall, $refresh, $playExisting, $shortcutExisting, $cards, $desktopOption, $hostingOption, $inviteOption)) { $control.Enabled = $false }
        $status.ForeColor = [Drawing.Color]::White
        $status.Text = 'Working...'; $form.Refresh()
        if (-not $remove) { Run-Backend $dir $false 'PrepareDataOnly' $false }
        $needsAdmin = (-not (Can-WriteGame $dir)) -or ((-not $remove) -and $hostingOption.Checked) -or ($remove -and @(Get-CoopFirewallRules $dir).Count -gt 0)
        if ($needsAdmin) { $status.Text = 'Windows will ask for administrator permission to change the game folder.'; $form.Refresh() }
        Run-Backend $dir $remove 'GameFilesOnly' $needsAdmin
        Run-Backend $dir $remove 'UserSettingsOnly' $false
        $script:installedDir = $dir
        Show-Completion $remove
    } catch {
        $status.Text = 'Setup did not complete. You can try again.'
        [Windows.Forms.MessageBox]::Show($form, $_.Exception.Message, 'MDK2 Co-op', 'OK', 'Error') | Out-Null
    } finally {
        foreach ($control in @($path, $browse, $install, $uninstall, $refresh, $playExisting, $shortcutExisting, $cards, $desktopOption, $hostingOption, $inviteOption)) { $control.Enabled = $true }
        $form.ControlBox = $true
        $script:busy = $false
        Refresh-Status
        $form.Activate()
    }
}
$install.Add_Click({ Perform $false })
$uninstall.Add_Click({ Perform $true })
function Refresh-Cards {
    $cards.Controls.Clear()
    $selected = $null; $first = $null
    foreach ($entry in @(Get-CoopInstallCards $PSScriptRoot (Join-Path $env:LOCALAPPDATA 'MDK2HD\settings.lua') (Join-Path $env:LOCALAPPDATA 'mdk2-coop\coop'))) {
        $dir = $entry.path; $build = $entry.build; $snapshot = $entry.state
        $card = New-Object Windows.Forms.RadioButton
        $card.Appearance = 'Button'; $card.AutoSize = $false; $card.Size = New-Object Drawing.Size(665, 60)
        $state = if ($snapshot.owner -eq 'none') { 'Co-op not installed' } elseif ($snapshot.owner -eq 'manual') { 'Co-op copied by hand' } else { "Co-op: $($snapshot.owner)" }
        $action = if ($snapshot.actions.blocked) { "; needs attention" } elseif ($snapshot.actions.install) { "; $($snapshot.actions.install) available" } elseif ($snapshot.owner -ne 'none') { '; up to date' } else { '' }
        $cardBuild = if ($build.Known) { $build.Text } else { 'MDK2 HD unknown build' }
        $card.Text = "$cardBuild  /  $state$action`r`n$dir"; $card.Tag = $dir
        $card.Add_CheckedChanged({ if ($this.Checked) { $path.Text = [string]$this.Tag } })
        $cards.Controls.Add($card)
        if (-not $first) { $first = $card }
        if ($entry.selected) { $selected = $card }
    }
    if (-not $selected) { $selected = $first } # Steam is enumerated first.
    if ($selected) { $selected.Checked = $true }
}
try { Refresh-Cards } catch { $status.Text = 'Some installs could not be inspected. Browse to your game folder.' }
Refresh-Status
[void]$form.ShowDialog()
$form.Dispose()
