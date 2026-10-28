# Exercise real setup controls and capture only its window without taking focus.
# PackageDir is an unpacked release; TestRoot must be a disposable Windows folder.
param([Parameter(Mandatory=$true)][string]$PackageDir,
      [Parameter(Mandatory=$true)][string]$TestRoot)
$Package=$PackageDir
$Root=Join-Path $TestRoot ("setup-window-"+[guid]::NewGuid().ToString())
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force "$Root\profile", "$Root\temp", "$Root\game\coop" | Out-Null
$env:LOCALAPPDATA="$Root\profile"; $env:TEMP="$Root\temp"; $env:TMP=$env:TEMP
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing @'
using System;
using System.Windows.Forms;
using System.Runtime.InteropServices;
public class CoopPreview : Form {
 protected override bool ShowWithoutActivation { get { return true; } }
 protected override CreateParams CreateParams { get { var p=base.CreateParams; p.ExStyle |= 0x08000000; return p; } }
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
$source=[IO.File]::ReadAllText((Join-Path $Package 'installer-gui.ps1'))
$source=$source.Replace("`r`n","`n")
$source=$source.Substring($source.IndexOf('$ErrorActionPreference'))
$source=$source.Substring(0,$source.IndexOf('try {'+"`n"+'    $found ='))
$source=$source.Replace('$PSScriptRoot',("'"+$Package.Replace("'","''")+"'"))
$source=$source.Replace('New-Object Windows.Forms.Form','New-Object CoopPreview')
. ([scriptblock]::Create($source))
[IO.File]::WriteAllText("$Root\game\mdk2hd.exe",'fixture, never executed')
$game="$Root\game"; $settings="$env:LOCALAPPDATA\MDK2HD\settings.lua"
New-Item -ItemType Directory -Force (Split-Path $settings) | Out-Null
$path.Text=$game
$screen=[Windows.Forms.Screen]::AllScreens | Sort-Object Primary | Select-Object -First 1
$form.StartPosition='Manual'; $form.Location=New-Object Drawing.Point(($screen.WorkingArea.X+20),($screen.WorkingArea.Y+20))
$foreground=[CoopPreview]::GetForegroundWindow()
$form.Show(); [Windows.Forms.Application]::DoEvents()
function Snapshot($name,$button,$enabled,$remove) {
 Refresh-Status; [Windows.Forms.Application]::DoEvents()
 if ($install.Text -ne $button -or $install.Enabled -ne $enabled -or $uninstall.Enabled -ne $remove) { throw "Buttons $name : $($install.Text) $($install.Enabled) $($uninstall.Enabled)" }
 $bmp=New-Object Drawing.Bitmap($form.Width,$form.Height)
 $g=[Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc()
 try { if (-not [CoopPreview]::PrintWindow($form.Handle,$dc,2)) { throw 'PrintWindow failed' } } finally { $g.ReleaseHdc($dc); $g.Dispose() }
 $bmp.Save("$Root\$name.png"); $bmp.Dispose()
 if ([CoopPreview]::GetForegroundWindow() -ne $foreground) { throw 'Foreground changed' }
 Write-Output "PASS: $name window capture and buttons"
}
try {
 Snapshot 'fresh' 'Install' $true $false
 Install-CoopFile "$game\dinput8.dll" ([Text.Encoding]::ASCII.GetBytes('old dll'))
 Install-CoopFile "$game\coop\coop.lua" ([IO.File]::ReadAllBytes("$Package\coop\coop.lua"))
 Install-CoopFile "$game\coop\per-user-data" ([byte[]]@())
 [IO.File]::WriteAllText($settings,"-- >>> mdk2-coop >>>`r`nif (coop_init) then coop_init() end`r`n-- <<< mdk2-coop <<<`r`n")
 Snapshot 'outdated' 'Update' $true $true
 Remove-Item "$game\coop\coop.lua"
 Snapshot 'partial-update' 'Repair' $true $true
 Install-CoopFile "$game\coop\coop.lua" ([IO.File]::ReadAllBytes("$Package\coop\coop.lua"))
 Install-CoopFile "$game\dinput8.dll" ([IO.File]::ReadAllBytes("$Package\dinput8.dll"))
 Snapshot 'current' 'Up to date' $false $true
 Remove-Item "$game\coop\coop.lua"
 Snapshot 'missing' 'Repair' $true $true
 [IO.File]::WriteAllText("$game\dinput8.dll",'foreign change')
 Snapshot 'modified' 'Needs attention' $false $false
 $form.Scale((New-Object Drawing.SizeF(2,2)))
 Snapshot 'modified-200' 'Needs attention' $false $false
} finally { $form.Close(); $form.Dispose() }

Write-Output "PASS: setup window evidence: $Root"
