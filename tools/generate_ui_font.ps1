# Generate antialiased ASCII font masks used by the native RGB565 renderer.
Add-Type -AssemblyName System.Drawing
$dest = Join-Path $PSScriptRoot '../main/assets/ui_font.bin'
$writer = [System.IO.BinaryWriter]::new([System.IO.File]::Create($dest))
foreach ($spec in @(@(18,22,16,0), @(26,32,25,1), @(20,24,17,1), @(16,20,13,0))) {
    $w=$spec[0]; $h=$spec[1]; $size=$spec[2]
    $bmp=[System.Drawing.Bitmap]::new($w,$h)
    $g=[System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint='AntiAliasGridFit'
    $font=[System.Drawing.Font]::new('Segoe UI',$size,[System.Drawing.FontStyle]$spec[3],[System.Drawing.GraphicsUnit]::Pixel)
    $format=[System.Drawing.StringFormat]::GenericTypographic.Clone()
    $format.FormatFlags=$format.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
    # Advance widths precede each font's fixed-size glyph masks.
    for ($code=32;$code -le 126;$code++) {
        $extent=$g.MeasureString([string][char]$code,$font,1000,$format)
        $writer.Write([byte][Math]::Min($w,[Math]::Ceiling($extent.Width)))
    }
    for ($code=32;$code -le 126;$code++) {
        $g.Clear([System.Drawing.Color]::Black)
        $g.DrawString([string][char]$code,$font,[System.Drawing.Brushes]::White,[single]0,[single]0,$format)
        for ($y=0;$y -lt $h;$y++) { for ($x=0;$x -lt $w;$x++) { $writer.Write([byte]$bmp.GetPixel($x,$y).R) } }
    }
    $format.Dispose(); $font.Dispose(); $g.Dispose(); $bmp.Dispose()
}
$writer.Dispose()
