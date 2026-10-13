# Draw the reference-inspired artwork and export native little-endian RGB565.
# Run from any directory with Windows PowerShell. No build-time dependency.
Add-Type -AssemblyName System.Drawing
$dest = Join-Path $PSScriptRoot '../main/assets'
$bmp = New-Object System.Drawing.Bitmap 480,480
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.TextRenderingHint = 'AntiAliasGridFit'
function Brush($hex) { New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($hex)) }
function Label($text, $font, $size, $style, $color, $x, $y) {
    $f = New-Object System.Drawing.Font $font,$size,([System.Drawing.FontStyle]$style),([System.Drawing.GraphicsUnit]::Pixel)
    $b = Brush $color
    $g.DrawString($text,$f,$b,[single]$x,[single]$y)
    $b.Dispose(); $f.Dispose()
}
function Wave($color,$offset) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddBezier(0,(320+$offset),140,(315+$offset),260,(495+$offset),480,(426+$offset))
    $p.AddLine(480,(426+$offset),480,480); $p.AddLine(480,480,0,480); $p.CloseFigure()
    $b=Brush $color; $g.FillPath($b,$p); $b.Dispose(); $p.Dispose()
}
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0,140)),([System.Drawing.Point]::new(480,480)),([System.Drawing.Color]::White),([System.Drawing.ColorTranslator]::FromHtml('#d8f1ff'))
$g.FillRectangle($bg,0,0,480,480)
$p=New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddBezier(0,110,40,50,105,12,164,0); $p.AddLine(164,0,0,0); $p.CloseFigure()
$b=Brush '#c6e7fc'; $g.FillPath($b,$p); $b.Dispose(); $p.Dispose()
Wave '#86d1ff' 0
Wave '#ffffff' 9
Wave '#05a9f4' 17
Wave '#0450b4' 34
Label 'Bonri' 'Arial' 94 'Bold' '#063bac' 67 57
Label 'x' 'Arial' 94 'Bold' '#ff701a' 310 57
Label ([char]0x00ae) 'Arial' 20 'Regular' '#202d3d' 396 68
Label 'Embedded Innovation' 'Arial' 29 'Bold' '#202d3d' 72 155
Label 'IDEAS  |  DEVICES  |  SMART SOLUTIONS' 'Arial' 12 'Regular' '#202d3d' 110 195
Label 'Welcome!' 'Brush Script MT' 82 'Italic' '#063bac' 91 218
$pen=New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml('#ff701a')),4
$g.DrawBezier($pen,174,317,231,303,310,302,356,307); $pen.Dispose()
Label 'Glad to have you here' 'Arial' 22 'Regular' '#202d3d' 136 327
Label 'Innovating' 'Brush Script MT' 25 'Italic' '#ffffff' 20 397
Label 'for a Smarter Tomorrow' 'Brush Script MT' 25 'Italic' '#ffffff' 24 421
Label 'EMBED | CONNECT | CREATE' 'Arial' 10 'Regular' '#20344b' 286 411
$pen=New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml('#acdfff')),2
for($i=0;$i -lt 5;$i++) {
    $x=328+$i*27; $y=358-$i*7
    $g.DrawLine($pen,$x,$y+39,$x+20,$y+19); $g.DrawLine($pen,$x+20,$y+19,$x+20,$y-8)
    $g.DrawEllipse($pen,$x+16,$y-16,8,8)
}; $pen.Dispose()
$pen=New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml('#0754bc')),4
foreach($r in @(19,13,7)) { $g.DrawArc($pen,(441-$r),(40-$r),($r*2),($r*2),220,100) }
$b=Brush '#0754bc'; $g.FillEllipse($b,438,37,6,6); $b.Dispose(); $pen.Dispose()
$bmp.Save((Join-Path $dest 'welcome.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$stream=[System.IO.File]::Create((Join-Path $dest 'welcome.rgb565'))
$writer=New-Object System.IO.BinaryWriter $stream
for($y=0;$y -lt 480;$y++) { for($x=0;$x -lt 480;$x++) {
    $c=$bmp.GetPixel($x,$y)
    $writer.Write([uint16]((([int]$c.R -shr 3) -shl 11) -bor (([int]$c.G -shr 2) -shl 5) -bor ([int]$c.B -shr 3)))
} }
$writer.Dispose(); $g.Dispose(); $bg.Dispose(); $bmp.Dispose()
