# PNG -> JPEG for the showcase page, optionally with parts hidden (addresses, invite codes).
#   powershell -File showcase_jpeg.ps1 -In shot.png -Out shot.jpg [-Quality 82] [-Hide "x,y,w,h;x,y,w,h"] [-Width 1280]
# Each -Hide rectangle (pixels of the input) is pixelated in 12-pixel blocks, then covered with a dark veil,
# so the text under it cannot be read back.
param([string]$In, [string]$Out, [int]$Quality = 82, [string]$Hide = "", [int]$Width = 0)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($In)
$w0 = $src.Width; $h0 = $src.Height
if ($Width -gt 0 -and $Width -lt $w0) { $h0 = [int]($h0 * $Width / $w0); $w0 = $Width }
$bmp = New-Object System.Drawing.Bitmap($w0, $h0, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($src, 0, 0, $w0, $h0)
$g.Dispose(); $src.Dispose()
if ($Hide -ne "") {
    foreach ($r in $Hide.Split(";")) {
        $v = $r.Split(",") | ForEach-Object { [int]$_ }
        $x0 = $v[0]; $y0 = $v[1]; $w = $v[2]; $h = $v[3]; $b = 12
        for ($y = $y0; $y -lt $y0 + $h; $y += $b) {
            for ($x = $x0; $x -lt $x0 + $w; $x += $b) {
                $rr = 0; $gg = 0; $bb = 0; $n = 0
                for ($j = $y; $j -lt [Math]::Min($y + $b, $y0 + $h); $j++) { for ($i = $x; $i -lt [Math]::Min($x + $b, $x0 + $w); $i++) {
                    $c = $bmp.GetPixel($i, $j); $rr += $c.R; $gg += $c.G; $bb += $c.B; $n++ } }
                $col = [System.Drawing.Color]::FromArgb([int]($rr / $n * 0.6), [int]($gg / $n * 0.6), [int]($bb / $n * 0.6))
                for ($j = $y; $j -lt [Math]::Min($y + $b, $y0 + $h); $j++) { for ($i = $x; $i -lt [Math]::Min($x + $b, $x0 + $w); $i++) { $bmp.SetPixel($i, $j, $col) } }
            }
        }
    }
}
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)
$bmp.Save($Out, $codec, $ep)
$bmp.Dispose()
