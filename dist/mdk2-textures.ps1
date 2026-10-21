# mdk2-coop: AI-upscaled textures for MDK2 HD. Windows PowerShell 5.1; downloads its two tools.
# Double-click "Upscale Textures.cmd" / "Restore Textures.cmd", or:
#   powershell -ExecutionPolicy Bypass -File mdk2-textures.ps1 [-Restore] [-GameDir <folder>] [-WorkDir <folder>] [-Yes]
#
# The level textures of the HD release are mostly the original 64-512 pixel ones (the HD
# release added normal and specular maps next to them). This upscales those 4x with Real-ESRGAN
# (on your GPU, through Vulkan) up to 1024 pixels, compresses them (BC3/DXT5, with mipmaps: about
# the size of the uncompressed originals) and writes a new mdk2.tar with them in. The game only
# reads textures from mdk2.tar, so the original is kept next to it as mdk2.tar.original and
# every run starts from that; -Restore puts it back. Nothing is uploaded or shared: the
# textures are made from your own copy of the game.
#
# Tools (downloaded once into <WorkDir>\tools, checked against their SHA-256):
#   realesrgan-ncnn-vulkan 20220424 (github.com/xinntao/Real-ESRGAN, BSD-3-Clause)
#   texconv may2026 (github.com/microsoft/DirectXTex, MIT)
param(
    [switch]$Restore,
    [string]$GameDir,
    [string]$WorkDir = (Join-Path $env:LOCALAPPDATA 'mdk2-coop'),
    [switch]$KeepWork,      # keep the intermediate images (for looking at them)
    [switch]$Yes            # no questions
)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Tools = @{
    esrgan = @{ Url = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
                Sha = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D' }
    texconv = @{ Url = 'https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe'
                 Sha = 'DCFDEC10244E02CF5037FBA089C55FB7E1326B1C8181742D77D15FA5CB5EEF06' }
}
$MaxSide = 1024        # upscaled textures stop here (the HD release's own big textures are 1024-2048)
$Pad     = 8           # source pixels wrapped around each edge, so tiling textures upscale without seams
# UI and HUD pictures keep their size: their layouts may count pixels
$Skip    = 'textbox|optionbox|icon|cross|select|font|logo|health|inv_|inv$|menu|button|target|panel|page'

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }
function Fail($msg) { Say "`n$msg" Red; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit 1 }

# ---- the game ----

function Resolve-Game {
    if ($GameDir) { $d = $GameDir.Trim('"', ' ') }
    else {
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Here 'mdk2-coop.ps1') -FindOnly -Yes 2>&1
        $line = $out | Where-Object { "$_" -like 'Game folder: *' } | Select-Object -First 1
        if (-not $line) { Fail 'MDK2 HD was not found. Give the game folder with -GameDir.' }
        $d = "$line".Substring(13)
    }
    if (-not (Test-Path (Join-Path $d 'mdk2.tar'))) { Fail "No mdk2.tar in '$d'." }
    $running = Get-Process mdk2hd -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($d, 'OrdinalIgnoreCase') }
    if ($running) { Fail 'MDK2 HD is running from that folder. Close the game and run this again.' }
    return (Resolve-Path $d).Path
}

# ---- tools ----

function Get-Tool($name, $dir) {
    $t = $Tools[$name]
    $file = Join-Path $dir (Split-Path $t.Url -Leaf)
    if (-not (Test-Path $file)) {
        Say "Downloading $(Split-Path $t.Url -Leaf) ..."
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        (New-Object Net.WebClient).DownloadFile($t.Url, $file)
    }
    $h = (Get-FileHash -Algorithm SHA256 $file).Hash
    if ($h -ne $t.Sha) { Remove-Item $file; Fail "$(Split-Path $file -Leaf) is not the expected download (SHA-256 $h). Nothing was changed." }
    return $file
}

# ---- mdk2.tar: plain tar, 512-byte headers ----

function Read-Octal([byte[]]$b, [int]$off, [int]$len) {
    $s = [Text.Encoding]::ASCII.GetString($b, $off, $len).Trim([char]0, ' ')
    if (-not $s) { return 0 }
    return [Convert]::ToInt64($s, 8)
}

# every member: name, data offset, size (regular files only)
function Read-TarIndex($path) {
    $list = New-Object System.Collections.ArrayList
    $fs = [IO.File]::OpenRead($path)
    try {
        $h = New-Object byte[] 512
        while ($fs.Read($h, 0, 512) -eq 512) {
            if ($h[0] -eq 0) { break }
            $name = [Text.Encoding]::ASCII.GetString($h, 0, 100).TrimEnd([char]0)
            $size = Read-OctalSize $h
            $type = [char]$h[156]
            $data = $fs.Position
            if ($type -eq '0' -or $type -eq [char]0) { [void]$list.Add([pscustomobject]@{ Name = $name; Offset = $data; Size = $size }) }
            $fs.Position = $data + [long]([Math]::Ceiling($size / 512.0) * 512)
        }
    } finally { $fs.Close() }
    return $list
}

function Read-OctalSize([byte[]]$h) { return Read-Octal $h 124 12 }

function Read-Bytes($path, [long]$offset, [int]$count) {
    $fs = [IO.File]::OpenRead($path)
    try { $b = New-Object byte[] $count; $fs.Position = $offset; [void]$fs.Read($b, 0, $count); return $b } finally { $fs.Close() }
}

# A copy of src with some members replaced: same order, the other headers and data byte for byte
function Write-Tar($src, $dst, $replace) {
    $in = [IO.File]::OpenRead($src)
    $out = [IO.File]::Create($dst)
    $buf = New-Object byte[] (1MB)
    try {
        $h = New-Object byte[] 512
        while ($in.Read($h, 0, 512) -eq 512) {
            if ($h[0] -eq 0) { break }
            $name = [Text.Encoding]::ASCII.GetString($h, 0, 100).TrimEnd([char]0)
            $size = Read-OctalSize $h
            $padded = [long]([Math]::Ceiling($size / 512.0) * 512)
            $file = $replace[$name.ToLower()]
            if ($file) {
                $bytes = [IO.File]::ReadAllBytes($file)
                $sz = [Text.Encoding]::ASCII.GetBytes([Convert]::ToString([long]$bytes.Length, 8).PadLeft(11, '0') + [char]0)
                [Array]::Copy($sz, 0, $h, 124, 12)
                for ($i = 148; $i -lt 156; $i++) { $h[$i] = 32 }
                $sum = 0; foreach ($x in $h) { $sum += $x }
                $ck = [Text.Encoding]::ASCII.GetBytes([Convert]::ToString($sum, 8).PadLeft(6, '0') + [char]0 + ' ')
                [Array]::Copy($ck, 0, $h, 148, 8)
                $out.Write($h, 0, 512)
                $out.Write($bytes, 0, $bytes.Length)
                $rest = ([Math]::Ceiling($bytes.Length / 512.0) * 512) - $bytes.Length
                if ($rest) { $out.Write((New-Object byte[] $rest), 0, $rest) }
                $in.Position += $padded
            } else {
                $out.Write($h, 0, 512)
                $left = $padded
                while ($left -gt 0) {
                    $n = $in.Read($buf, 0, [int][Math]::Min($buf.Length, $left))
                    if ($n -le 0) { throw "mdk2.tar ends early at $name" }
                    $out.Write($buf, 0, $n); $left -= $n
                }
            }
        }
        $out.Write((New-Object byte[] 1024), 0, 1024)
    } finally { $in.Close(); $out.Close() }
}

# ---- which textures ----

# DDS header: height, width, uncompressed 32-bit?
function Dds-Info([byte[]]$b) {
    if ($b.Length -lt 128 -or [Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'DDS ') { return $null }
    $flags = [BitConverter]::ToUInt32($b, 80)
    [pscustomobject]@{
        H = [BitConverter]::ToInt32($b, 12); W = [BitConverter]::ToInt32($b, 16)
        Rgb32 = (($flags -band 4) -eq 0) -and ([BitConverter]::ToInt32($b, 88) -eq 32)
    }
}

function Pick-Textures($tar, $index) {
    $names = @{}; foreach ($m in $index) { $names[$m.Name.ToLower()] = $m }
    $pick = @()
    foreach ($m in $index) {
        $n = $m.Name.ToLower()
        if (-not $n.EndsWith('.dds') -or $n -match '_(n|s|d)\.dds$') { continue }
        $base = $n.Substring(0, $n.Length - 4)
        if ($base -match $Skip) { continue }
        # level textures: the HD release gave them a normal map; and the skies
        if (-not $names.ContainsKey("${base}_n.dds") -and $base -notmatch 'sky') { continue }
        $info = Dds-Info (Read-Bytes $tar $m.Offset 128)
        if (-not $info -or -not $info.Rgb32) { continue }
        $side = [Math]::Max($info.W, $info.H)
        if ($side -lt 64 -or $side -gt 512) { continue }
        $k = [Math]::Min(4.0, $MaxSide / $side)
        $pick += [pscustomobject]@{ Member = $m; Base = $base; W = $info.W; H = $info.H
                                    TW = [int]($info.W * $k); TH = [int]($info.H * $k) }
    }
    return $pick
}

# ---- images (System.Drawing) ----

Add-Type -AssemblyName System.Drawing

# the picture with Pad pixels of its own opposite edges around it
function Pad-Wrap($srcPng, $dstPng) {
    $img = [Drawing.Bitmap]::FromFile($srcPng)
    try {
        $w = $img.Width; $h = $img.Height; $p = [Math]::Min($Pad, [Math]::Min($w, $h))
        $out = New-Object Drawing.Bitmap ($w + 2 * $p), ($h + 2 * $p), ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [Drawing.Graphics]::FromImage($out)
        $g.CompositingMode = 'SourceCopy'; $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
        foreach ($dy in -1, 0, 1) { foreach ($dx in -1, 0, 1) {
            $g.DrawImage($img, (New-Object Drawing.Rectangle ($p + $dx * $w), ($p + $dy * $h), $w, $h))
        } }
        $g.Dispose(); $out.Save($dstPng, [Drawing.Imaging.ImageFormat]::Png); $out.Dispose()
        return $p
    } finally { $img.Dispose() }
}

function Crop($srcPng, $dstPng, $x, $y, $w, $h) {
    $img = [Drawing.Bitmap]::FromFile($srcPng)
    try {
        $c = $img.Clone((New-Object Drawing.Rectangle $x, $y, $w, $h), [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $c.Save($dstPng, [Drawing.Imaging.ImageFormat]::Png); $c.Dispose()
    } finally { $img.Dispose() }
}

function Run($exe, [string[]]$arguments, $what) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path -Parent $exe) -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput (Join-Path $work "$what.out.txt") -RedirectStandardError (Join-Path $work "$what.err.txt")
    if ($p.ExitCode -ne 0) { Fail "$what failed (exit $($p.ExitCode)); see $work\$what.err.txt" }
}

# ---- main ----

Say 'mdk2-coop textures' Cyan
$game = Resolve-Game
Say "Game folder: $game"
$tar = Join-Path $game 'mdk2.tar'
$orig = Join-Path $game 'mdk2.tar.original'

if ($Restore) {
    if (-not (Test-Path $orig)) { Fail 'No mdk2.tar.original here: the textures were never changed (or were restored already).' }
    Remove-Item $tar
    Move-Item $orig $tar
    Say 'Original textures back.' Green
    if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
    exit 0
}

$work = Join-Path $WorkDir 'textures-work'
$toolDir = Join-Path $WorkDir 'tools'
New-Item -ItemType Directory -Force $toolDir | Out-Null
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
foreach ($d in 'src', 'png', 'padded', 'up', 'crop', 'dds') { New-Item -ItemType Directory -Force (Join-Path $work $d) | Out-Null }

$zip = Get-Tool 'esrgan' $toolDir
$texconv = Get-Tool 'texconv' $toolDir
$esrDir = Join-Path $toolDir 'realesrgan'
if (-not (Test-Path (Join-Path $esrDir 'realesrgan-ncnn-vulkan.exe'))) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $esrDir)
}
$esrgan = Join-Path $esrDir 'realesrgan-ncnn-vulkan.exe'

# the original archive: every run starts from it; it becomes mdk2.tar.original only once the new
# one is written, so a run that stops half way leaves the game as it was
$src = if (Test-Path $orig) { $orig } else { $tar }
$need = (Get-Item $src).Length * 1.1
$free = (Get-PSDrive ((Resolve-Path $game).Drive.Name)).Free
if ($free -lt $need) { Fail ("Not enough free space next to the game: {0:N1} GB needed." -f ($need / 1GB)) }

$t0 = Get-Date
$index = Read-TarIndex $src
$pick = Pick-Textures $src $index
Say ("{0} textures to upscale (of {1} files in the archive)" -f $pick.Count, $index.Count)

foreach ($t in $pick) {
    [IO.File]::WriteAllBytes((Join-Path $work "src\$($t.Base).dds"), (Read-Bytes $src $t.Member.Offset ([int]$t.Member.Size)))
}
Say 'Reading them ...'
Run $texconv @('-nologo', '-y', '-ft', 'png', '-o', "`"$work\png`"", "`"$work\src\*.dds`"") 'texconv-read'
Say 'Padding the edges ...'
foreach ($t in $pick) {
    $p = Pad-Wrap (Join-Path $work "png\$($t.Base).png") (Join-Path $work "padded\$($t.Base).png")
    $t | Add-Member -NotePropertyName Pad -NotePropertyValue $p
}
Say 'Upscaling 4x with Real-ESRGAN (this is the slow part) ...'
Run $esrgan @('-i', "`"$work\padded`"", '-o', "`"$work\up`"", '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png') 'esrgan'
Say 'Cropping and compressing ...'
$groups = @{}
foreach ($t in $pick) {
    $sub = "crop\$($t.TW)x$($t.TH)"
    New-Item -ItemType Directory -Force (Join-Path $work $sub) | Out-Null
    Crop (Join-Path $work "up\$($t.Base).png") (Join-Path $work "$sub\$($t.Base).png") (4 * $t.Pad) (4 * $t.Pad) (4 * $t.W) (4 * $t.H)
    $groups["$($t.TW)x$($t.TH)"] = @($t.TW, $t.TH)
}
foreach ($g in $groups.Keys) {
    Run $texconv @('-nologo', '-y', '--ignore-srgb', '-f', 'BC3_UNORM', '-m', '0', '-if', 'FANT',
                   '-w', $groups[$g][0], '-h', $groups[$g][1], '-o', "`"$work\dds`"", "`"$work\crop\$g\*.png`"") "texconv-$g"
}
$replace = @{}
foreach ($t in $pick) {
    $f = Join-Path $work "dds\$($t.Base).dds"
    if (-not (Test-Path $f)) { Fail "$($t.Base).dds was not made; see the logs in $work" }
    $replace[$t.Member.Name.ToLower()] = $f
}
Say 'Writing the new mdk2.tar ...'
Write-Tar $src "$tar.tmp" $replace
if (-not (Test-Path $orig)) { Move-Item $tar $orig } else { Remove-Item $tar }
Move-Item "$tar.tmp" $tar
if (-not $KeepWork) { Remove-Item -Recurse -Force $work }
Say ("Done: {0} textures upscaled in {1:N0} min. Restore Textures.cmd puts the originals back." -f $pick.Count, ((Get-Date) - $t0).TotalMinutes) Green
if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
