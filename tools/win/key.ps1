# Usage: key.ps1 <scancode> [count] [holdMs]  -- sends DirectInput-visible scancode presses to mdk2hd
param([int]$sc, [int]$count=1, [int]$hold=80, [int]$id=0)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class K {
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, sc; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KI ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int sz);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Send(ushort sc, bool up) { var i = new IN[1]; i[0].type = 1; i[0].ki.sc = sc; i[0].ki.flags = 0x8u | (up ? 0x2u : 0u); SendInput(1, i, Marshal.SizeOf(typeof(IN))); }
}
"@
$p = if ($id) { Get-Process -Id $id } else { Get-Process mdk2hd | Select-Object -First 1 }
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
for ($n=0; $n -lt $count; $n++) { [K]::Send($sc,$false); Start-Sleep -Milliseconds $hold; [K]::Send($sc,$true); Start-Sleep -Milliseconds 250 }
