# Usage: winshot.ps1 <out.png>  -- captures the mdk2hd window
param([string]$out, [int]$id=0)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); }
"@
[W]::SetProcessDPIAware() | Out-Null
$p = if ($id) { Get-Process -Id $id } else { Get-Process mdk2hd | Select-Object -First 1 }
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 400
$r = New-Object W+R; [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w=$r.Rt-$r.L; $h=$r.B-$r.T
$bmp = New-Object System.Drawing.Bitmap $w,$h
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size)
$bmp.Save($out); $g.Dispose(); $bmp.Dispose()
