# List (and with -Close, dismiss) dialog boxes owned by game processes running from the given copies.
# One line per dialog: pid|window handle|title|text. A dialog from a game copy is always an error in a
# test run (the Overhaul build shows "Jinkies! There was an error..." and waits for a click).
param([string]$Dirs = '', [switch]$Close)
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Dlg {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<IntPtr> Dialogs() {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "#32770" && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  public static uint Pid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
  public static string Text(IntPtr h) {
    var parts = new List<string>();
    EnumChildWindows(h, (c, l) => { var s = new StringBuilder(1024); GetWindowText(c, s, 1024);
      if (s.Length > 0) parts.Add(s.ToString()); return true; }, IntPtr.Zero);
    return string.Join(" / ", parts);
  }
  public static void Dismiss(IntPtr h) {
    // IDNO first: the "press Yes to debug, No to create a crash dump" box has no close button
    foreach (int id in new int[] { 7, 2, 1 }) {
      SendMessage(h, 0x0111, (IntPtr)id, IntPtr.Zero);
      System.Threading.Thread.Sleep(300);
      if (!IsWindow(h)) return;
    }
    SendMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero);
  }
}
"@
$roots = @($Dirs -split ',' | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\').ToLower() + '\' })
foreach ($h in [Dlg]::Dialogs()) {
  $id = [Dlg]::Pid($h)
  $p = Get-Process -Id $id -ErrorAction SilentlyContinue
  if (-not $p -or $p.ProcessName -ne 'mdk2hd') { continue }
  $path = ''
  try { $path = $p.Path } catch {}
  $mine = $false
  foreach ($r in $roots) { if ($path -and $path.ToLower().StartsWith($r)) { $mine = $true } }
  if (-not $mine) { continue }
  '{0}|{1}|{2}|{3}' -f $id, $h.ToInt64(), [Dlg]::Title($h), [Dlg]::Text($h)
  if ($Close) { [Dlg]::Dismiss($h) }
}
