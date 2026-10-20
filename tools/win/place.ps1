# Usage: place.ps1 -id <pid> -x <px> -y <px>  -- move a game window (physical pixels)
param([int]$id, [int]$x, [int]$y)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class P {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
}
"@
[P]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Id $id
for ($i = 0; $i -lt 50 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
$r = New-Object P+R; [P]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
[P]::MoveWindow($p.MainWindowHandle, $x, $y, $r.Rt - $r.L, $r.B - $r.T, $true) | Out-Null
