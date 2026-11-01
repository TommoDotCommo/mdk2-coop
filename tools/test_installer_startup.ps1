# Check the real native bootstrap and real form, without clicking any actions.
param([Parameter(Mandatory=$true)][string]$PackageDir,
      [string]$SingleFile)    # the single-file Setup.exe from tools/release.sh: no scripts beside it; unpacks into %LOCALAPPDATA%
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SetupWindows {
    delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            uint owner; GetWindowThreadProcessId(window, out owner);
            var title = new StringBuilder(256); GetWindowText(window, title, 256);
            if (owner == pid && IsWindowVisible(window) && title.ToString() == "MDK2 Co-op Setup") found = window;
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$child = $null
try {
    # A read-only status check must work without UAC in the unpacked package.
    . (Join-Path $PackageDir 'install-state.ps1')
    if ((Invoke-CoopPolicy @('actions', '2', '0')) -ne 1) { throw 'Unexpected policy result' }
    Write-Output 'PASS: packaged status helper runs without elevation'
    $setupExe = if ($SingleFile) { $SingleFile } else { Join-Path $PackageDir 'MDK2-Coop-Setup.exe' }
    if ($SingleFile) {
        # its working folder goes under a scratch profile, not the real one
        $env:LOCALAPPDATA = Join-Path (Split-Path -Parent $SingleFile) 'profile'
        New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
    }
    $bootstrap = Start-Process -FilePath $setupExe -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $window = [IntPtr]::Zero
    do {
        if (-not $child) {
            $record = Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" | Where-Object { $_.ParentProcessId -eq $bootstrap.Id } | Select-Object -First 1
            if ($record) {
                $child = Get-Process -Id $record.ProcessId -ErrorAction Stop
                [void]$child.Handle
            }
        }
        if ($child) { $window = [SetupWindows]::Find($child.Id) }
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'Native setup never displayed its real installer window' }
    [void][SetupWindows]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $child.WaitForExit(5000)) { throw 'Setup did not close normally' }
    if ($child.ExitCode -ne 0) { throw "Setup exited with an error: $($child.ExitCode)" }
    if ($SingleFile) {
        $unpacked = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'mdk2-coop\setup') -Directory | Select-Object -First 1
        if (-not $unpacked -or -not (Test-Path (Join-Path $unpacked.FullName 'installer-gui.ps1')) -or -not (Test-Path (Join-Path $unpacked.FullName 'dinput8.dll'))) { throw 'single-file Setup did not unpack its files' }
        Write-Output "PASS: single-file Setup unpacked into a fresh folder and showed the real installer window"
    } else {
    Write-Output 'PASS: unelevated package policy and visible real installer window; closed without installing'
    }
} finally {
    if ($child -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
}
