# User-facing installer. Only the game-file operation is ever elevated.
param([string]$ProbePath, [ValidateSet('', 'install', 'uninstall')][string]$TestCompletion = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$backend = Join-Path $PSScriptRoot 'mdk2-coop.ps1'
$form = New-Object Windows.Forms.Form
$form.Text = 'MDK2 Co-op Setup'
$form.ClientSize = New-Object Drawing.Size(640, 330)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [Drawing.ColorTranslator]::FromHtml('#0c171c')
$form.ForeColor = [Drawing.ColorTranslator]::FromHtml('#e9eef0')
$form.Font = New-Object Drawing.Font 'Segoe UI', 10
$exe = Join-Path $PSScriptRoot 'MDK2-Coop-Setup.exe'
if (Test-Path $exe) { try { $form.Icon = [Drawing.Icon]::ExtractAssociatedIcon($exe) } catch { } }
$header = New-Object Windows.Forms.Panel
$header.BackColor = [Drawing.ColorTranslator]::FromHtml('#142832')
$header.SetBounds(0, 0, 640, 91)
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
$stripe.SetBounds(0, 88, 640, 3)
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
$install.Text = 'Install / Repair'; $install.SetBounds(318, 266, 143, 40)
Style-Button $install $true
$form.Controls.Add($install)
$uninstall = New-Object Windows.Forms.Button
$uninstall.Text = 'Uninstall'; $uninstall.SetBounds(470, 266, 143, 40)
Style-Button $uninstall $false
$form.Controls.Add($uninstall)
$finish = New-Object Windows.Forms.Panel
$finish.SetBounds(0, 91, 640, 239)
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
$play.Add_Click({
    try {
        Start-Process -FilePath (Join-Path $script:installedDir 'mdk2hd.exe') -WorkingDirectory $script:installedDir -ErrorAction Stop | Out-Null
        $form.Close()
    } catch {
        [Windows.Forms.MessageBox]::Show($form, "Could not start MDK2 HD. Start it from Steam or the Beamdog client instead.`n`n$($_.Exception.Message)", 'MDK2 Co-op', 'OK', 'Error') | Out-Null
    }
})
function Show-Completion($remove) {
    foreach ($control in @($label, $path, $browse, $status, $install, $uninstall)) { $form.Controls.Remove($control) }
    $finish.Controls.Remove($play)
    if ($remove) {
        $finishText.Text = 'Co-op has been uninstalled. Your co-op data was kept.'
    } else {
        $finishText.Text = 'Installation complete. Start MDK2 HD to open the co-op launcher. Keep Steam or the Beamdog client running.'
        $finish.Controls.Add($play)
    }
    $form.Controls.Add($finish)
    $form.AcceptButton = $close
}
if ($TestCompletion) {
    Show-Completion ($TestCompletion -eq 'uninstall')
    if (-not $finish.Controls.Contains($close) -or ($finish.Controls.Contains($play) -ne ($TestCompletion -eq 'install'))) { exit 1 }
    Write-Output "completion $TestCompletion OK"
    $form.Dispose()
    exit 0
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
    if ($elevate) { $options.Verb = 'RunAs' }
    $process = Start-Process @options
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
        $script:busy = $true
        $form.ControlBox = $false
        foreach ($control in @($path, $browse, $install, $uninstall)) { $control.Enabled = $false }
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
        foreach ($control in @($path, $browse, $install, $uninstall)) { $control.Enabled = $true }
        $form.ControlBox = $true
        $script:busy = $false
        $form.Activate()
    }
}
$install.Add_Click({ Perform $false })
$uninstall.Add_Click({ Perform $true })
try {
    $found = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $backend -FindOnly -Yes 2>&1
    foreach ($line in $found) { if ("$line" -match '^Game folder: (.+)$') { $path.Text = $Matches[1]; break } }
} catch { }
[void]$form.ShowDialog()
$form.Dispose()
