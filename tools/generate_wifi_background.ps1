# Code-drawn 480x480 background inspired by the supplied Wi-Fi screen.
Add-Type -AssemblyName System.Drawing
$dest=Join-Path $PSScriptRoot '../main/assets'
$bmp=[System.Drawing.Bitmap]::new(480,480)
$g=[System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode='AntiAlias'; $g.TextRenderingHint='AntiAliasGridFit'
function Color($hex) { [System.Drawing.ColorTranslator]::FromHtml($hex) }
function FillPath($path,$color) {
    $b=[System.Drawing.SolidBrush]::new((Color $color));$g.FillPath($b,$path);$b.Dispose();$path.Dispose()
}
$bg=[System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(120,80),[System.Drawing.Point]::new(420,480),(Color '#ffffff'),(Color '#e4f5ff'))
$g.FillRectangle($bg,0,0,480,480);$bg.Dispose()
$p=[System.Drawing.Drawing2D.GraphicsPath]::new()
$p.AddBezier(0,108,32,49,96,19,176,0);$p.AddLine(176,0,0,0);$p.CloseFigure();FillPath $p '#def3ff'
$p=[System.Drawing.Drawing2D.GraphicsPath]::new()
$p.AddBezier(0,70,60,24,124,6,151,0);$p.AddLine(151,0,0,0);$p.CloseFigure();FillPath $p '#90dcfb'
$p=[System.Drawing.Drawing2D.GraphicsPath]::new()
$p.AddBezier(0,37,45,10,83,4,97,0);$p.AddLine(97,0,0,0);$p.CloseFigure();FillPath $p '#08acf0'
$pen=[System.Drawing.Pen]::new((Color '#b7e7fa'),1.5)
for($i=0;$i -lt 5;$i++) {
    $x=342+$i*25;$y=438-$i*3
    $g.DrawLine($pen,$x,$y,$x+12,$y-12);$g.DrawLine($pen,$x+12,$y-12,$x+12,$y-29)
    $g.DrawEllipse($pen,$x+9,$y-35,6,6)
};$pen.Dispose()
foreach($band in @(@(0,'#8edafa'),@(5,'#ffffff'),@(10,'#0cb4f3'),@(17,'#005ac8'))) {
    $offset=[int]$band[0];$p=[System.Drawing.Drawing2D.GraphicsPath]::new()
    $p.AddBezier(0,(413+$offset),146,(411+$offset),270,(481+$offset),480,(429+$offset))
    $p.AddLine(480,(429+$offset),480,480);$p.AddLine(480,480,0,480);$p.CloseFigure()
    $b=[System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(0,0),[System.Drawing.Point]::new(480,0),(Color $band[1]),(Color $(if($offset -eq 17){'#08b7ef'}else{$band[1]})))
    $g.FillPath($b,$p);$b.Dispose();$p.Dispose()
}
$font=[System.Drawing.Font]::new('Segoe Print',15,[System.Drawing.FontStyle]::Italic,[System.Drawing.GraphicsUnit]::Pixel)
$g.DrawString('Connecting for a smarter tomorrow',$font,[System.Drawing.Brushes]::White,[single]17,[single]449)
$font.Dispose();$g.Dispose()
$bmp.Save((Join-Path $dest 'wifi_background.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$writer=[System.IO.BinaryWriter]::new([System.IO.File]::Create((Join-Path $dest 'wifi_background.rgb565')))
for($y=0;$y -lt 480;$y++){for($x=0;$x -lt 480;$x++){
    $c=$bmp.GetPixel($x,$y)
    $writer.Write([uint16]((([int]$c.R -shr 3) -shl 11) -bor (([int]$c.G -shr 2) -shl 5) -bor ([int]$c.B -shr 3)))
}}
$writer.Dispose();$bmp.Dispose()
