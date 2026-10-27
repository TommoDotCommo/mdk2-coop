# Regenerate dist/installer.ico with Windows PowerShell 5.1 and System.Drawing.
param([Parameter(Mandatory=$true)][string]$OutputPath)
Add-Type -AssemblyName System.Drawing
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $bmp = New-Object Drawing.Bitmap $size, $size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([Drawing.Color]::Transparent)
    $orange = New-Object Drawing.SolidBrush ([Drawing.ColorTranslator]::FromHtml('#ff9025'))
    $dark = New-Object Drawing.SolidBrush ([Drawing.ColorTranslator]::FromHtml('#10232d'))
    $white = New-Object Drawing.SolidBrush ([Drawing.ColorTranslator]::FromHtml('#ffffff'))
    $g.FillEllipse($orange, 0, 0, $size, $size)
    $inset = [single]($size * 0.115)
    $g.FillEllipse($dark, $inset, $inset, [single]($size - 2 * $inset), [single]($size - 2 * $inset))
    $font = New-Object Drawing.Font 'Arial', ([single]($size * 0.72)), ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object Drawing.StringFormat
    $fmt.Alignment = [Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [Drawing.StringAlignment]::Center
    $g.DrawString('2', $font, $white, (New-Object Drawing.RectangleF 0, 0, $size, $size), $fmt)
    $png = New-Object IO.MemoryStream
    $bmp.Save($png, [Drawing.Imaging.ImageFormat]::Png)
    $images += ,$png.ToArray()
    $png.Dispose(); $fmt.Dispose(); $font.Dispose(); $orange.Dispose(); $dark.Dispose(); $white.Dispose(); $g.Dispose(); $bmp.Dispose()
}
$file = [IO.File]::Open($OutputPath, [IO.FileMode]::Create)
$writer = New-Object IO.BinaryWriter $file
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($bytes in $images) { $writer.Write([byte[]]$bytes) }
$writer.Close()
