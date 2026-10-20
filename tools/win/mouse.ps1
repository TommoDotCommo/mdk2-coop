# Usage: mouse.ps1 -id <pid> [-hold ms] [-count n]  -- left mouse button presses to mdk2hd
param([int]$id=0, [int]$hold=100, [int]$count=1)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class M {
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MI mi; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int sz);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Btn(bool up) { var i = new IN[1]; i[0].type = 0; i[0].mi.flags = up ? 0x4u : 0x2u; SendInput(1, i, Marshal.SizeOf(typeof(IN))); }
}
"@
$p = if ($id) { Get-Process -Id $id } else { Get-Process mdk2hd | Select-Object -First 1 }
[M]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
for ($n=0; $n -lt $count; $n++) { [M]::Btn($false); Start-Sleep -Milliseconds $hold; [M]::Btn($true); Start-Sleep -Milliseconds 200 }
