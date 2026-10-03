# Generates res\app.ico: a rounded blue-to-teal tile with four white activity bars.
# Every size is drawn natively (bars snapped to whole pixels) rather than scaled
# down from 256 px, so the 16/20/24 px tray and taskbar icons stay crisp.
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    if ($r -le 0) { $p.AddRectangle((New-Object Drawing.RectangleF $x, $y, $w, $h)); return $p }
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Bars with rounded tops only.
function BarPath([float]$x, [float]$top, [float]$w, [float]$bottom, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    if ($r -le 0) { $p.AddRectangle((New-Object Drawing.RectangleF $x, $top, $w, ($bottom - $top))); return $p }
    $d = 2 * $r
    $p.AddArc($x, $top, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $top, $d, $d, 270, 90)
    $p.AddLine(($x + $w), ($top + $r), ($x + $w), $bottom)
    $p.AddLine(($x + $w), $bottom, $x, $bottom)
    $p.CloseFigure()
    return $p
}

foreach ($s in $sizes) {
    $bmp = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)

    # Tile: full bleed at small sizes, a little inset at large ones (like Win11 icons).
    $inset = if ($s -ge 48) { [math]::Round($s * 0.04) } else { 0 }
    $tile = $s - 2 * $inset
    $radius = [math]::Max(2, $tile * 0.22)
    $bg = RoundedRect $inset $inset $tile $tile $radius
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush (
        (New-Object Drawing.PointF $inset, $inset),
        (New-Object Drawing.PointF ($inset + $tile), ($inset + $tile)),
        ([Drawing.Color]::FromArgb(255, 37, 117, 252)),   # #2575FC
        ([Drawing.Color]::FromArgb(255, 20, 196, 178)))   # #14C4B2
    $g.FillPath($grad, $bg)

    # Four uneven bars (an activity chart, not a signal meter), snapped to whole pixels.
    $bw = [math]::Max(2, [math]::Round($tile * 0.13))
    $gap = [math]::Max(1, [math]::Round($tile * 0.06))
    $total = 4 * $bw + 3 * $gap
    $x0 = $inset + [math]::Floor(($tile - $total) / 2)
    $bottom = $inset + $tile - [math]::Max(3, [math]::Round($tile * 0.2))
    $heights = 0.40, 0.62, 0.30, 0.68 | ForEach-Object { [math]::Max(2, [math]::Round($tile * $_)) }
    $barRadius = if ($s -ge 32) { $bw * 0.35 } else { 0 }
    $white = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 255, 255, 255))
    for ($i = 0; $i -lt 4; $i++) {
        $x = $x0 + $i * ($bw + $gap)
        $g.FillPath($white, (BarPath $x ($bottom - $heights[$i]) $bw $bottom $barRadius))
    }

    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    if ($s -eq 256) { $bmp.Save("$PSScriptRoot\..\docs\images\icon.png") }  # for the readme
    $g.Dispose(); $bmp.Dispose()
}

# ICO container with PNG-compressed entries (supported since Windows Vista).
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
[IO.File]::WriteAllBytes("$PSScriptRoot\app.ico", $out.ToArray())
"wrote app.ico ($($out.Length) bytes, sizes $($sizes -join ', '))"
