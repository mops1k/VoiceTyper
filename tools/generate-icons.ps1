param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'VoiceTyper.App\Assets')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$S = 1024          # master render size

function New-RoundedRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2.0 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap {
    param([float]$size)

    $bmp = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    $scale = $size / $S

    # ---- Background: rounded-square tile with a blue gradient ----
    $radius = 0.214 * $size
    $bgPath = New-RoundedRectPath 0 0 $size $size $radius
    $c1 = [System.Drawing.Color]::FromArgb(255, 91, 148, 245)
    $c2 = [System.Drawing.Color]::FromArgb(255, 39, 100, 224)
    $grad = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.PointF]::new(0, 0),
        [System.Drawing.PointF]::new($size, $size),
        $c1, $c2)
    $g.FillPath($grad, $bgPath)

    $inner = $size * 0.04
    $hiPath = New-RoundedRectPath $inner $inner ($size - 2*$inner) ($size - 2*$inner) ($radius - $inner)
    $hiBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(28, 255, 255, 255))
    $g.FillPath($hiBrush, $hiPath)
    $hiBrush.Dispose()

    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)

    # ---- Glyph stroke style ----
    $stroke = [System.Drawing.Pen]::new($white, 30 * $scale)
    $stroke.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $stroke.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $stroke.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $thick = [System.Drawing.Pen]::new($white, 42 * $scale)
    $thick.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $thick.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $thick.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $bar = [System.Drawing.Pen]::new($white, 30 * $scale)
    $bar.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $bar.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    $g.ScaleTransform($scale, $scale)

    # ---- Microphone (left) ----
    $mc = New-RoundedRectPath 272 245 160 315 80
    $g.DrawPath($stroke, $mc)
    $mc.Dispose()

    foreach ($yy in @(315, 375, 435, 495)) {
        $g.DrawLine($bar, 302, $yy, 404, $yy)
    }

    # holder cradle (open U around the lower part of the capsule)
    $cradle = New-Object System.Drawing.Drawing2D.GraphicsPath
    $cradle.AddLine(247, 468, 247, 575)
    $cradle.AddArc(247, 515, 210, 120, 180, 180)
    $cradle.AddLine(457, 575, 457, 468)
    $g.DrawPath($thick, $cradle)
    $cradle.Dispose()

    # base
    $g.DrawLine($thick, 282, 648, 422, 648)

    # sound waves (two arcs opening toward the mic, upper-left)
    $w1 = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w1.AddArc(110, 245, 185, 185, 100, 160)
    $g.DrawPath($stroke, $w1)
    $w1.Dispose()
    $w2 = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w2.AddArc(158, 292, 120, 120, 105, 150)
    $g.DrawPath($stroke, $w2)
    $w2.Dispose()

    # ---- Document (right) with folded corner ----
    $dl, $dt, $dw, $dh = 585, 240, 315, 540
    $fold = 88
    $doc = New-Object System.Drawing.Drawing2D.GraphicsPath
    $doc.StartFigure()
    $doc.AddLine($dl, $dt + 20, $dl, $dt + $dh - 20)
    $doc.AddArc($dl, $dt + $dh - 40, 40, 40, 180, -90)
    $doc.AddLine($dl, $dt + $dh, $dl + $dw - 40, $dt + $dh)
    $doc.AddArc($dl + $dw - 40, $dt + $dh - 40, 40, 40, 270, -90)
    $doc.AddLine($dl + $dw, $dt + $dh - 40, $dl + $dw, $dt + $fold)
    $doc.AddLine($dl + $dw, $dt + $fold, $dl + $dw - $fold, $dt)
    $doc.AddLine($dl + $dw - $fold, $dt, $dl + 30, $dt)
    $doc.AddArc($dl, $dt, 40, 40, 90, -90)
    $g.DrawPath($stroke, $doc)
    $doc.Dispose()

    $flap = New-Object System.Drawing.Drawing2D.GraphicsPath
    $flap.AddLine($dl + $dw - $fold, $dt, $dl + $dw, $dt + $fold)
    $flap.AddLine($dl + $dw, $dt + $fold, $dl + $dw - $fold, $dt + $fold)
    $flap.CloseFigure()
    $g.DrawPath($stroke, $flap)
    $flap.Dispose()

    foreach ($yy in @(375, 480, 585)) {
        $g.DrawLine($bar, 638, $yy, 850, $yy)
    }
    $g.DrawLine($bar, 638, 680, 775, 680)

    # ---- Connector arrow (mic -> document), entering from below ----
    $cone = New-Object System.Drawing.Drawing2D.GraphicsPath
    $cone.AddLine(352, 655, 352, 845)
    $cone.AddLine(352, 845, 640, 845)
    $cone.AddLine(640, 845, 640, 812)
    $g.DrawPath($stroke, $cone)
    $cone.Dispose()

    $ah = New-Object System.Drawing.Drawing2D.GraphicsPath
    $ah.AddLine(640, 782, 597, 828)
    $ah.AddLine(597, 828, 683, 828)
    $ah.CloseFigure()
    $fillWh = [System.Drawing.SolidBrush]::new($white)
    $g.FillPath($fillWh, $ah)
    $fillWh.Dispose()
    $ah.Dispose()

    $stroke.Dispose(); $thick.Dispose(); $bar.Dispose()
    $grad.Dispose(); $bgPath.Dispose(); $hiPath.Dispose()
    $g.Dispose()
    return $bmp
}

function Save-Png([string]$Path, [int]$Px) {
    $master = New-IconBitmap $S
    $out = [System.Drawing.Bitmap]::new($Px, $Px)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($master, 0, 0, $Px, $Px)
    $g.Dispose()
    $out.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose(); $master.Dispose()
}

function New-Icon([string]$Path) {
    $sizes = @(256, 64, 48, 32, 16)
    $frames = @()
    foreach ($sz in $sizes) {
        $master = New-IconBitmap $S
        $out = [System.Drawing.Bitmap]::new($sz, $sz)
        $g = [System.Drawing.Graphics]::FromImage($out)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($master, 0, 0, $sz, $sz)
        $g.Dispose()
        $ms = [System.IO.MemoryStream]::new()
        $out.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += ,@{ Size = $sz; Bytes = $ms.ToArray() }
        $ms.Dispose(); $out.Dispose(); $master.Dispose()
    }

    $headerSize = 6
    $entrySize = 16
    $offset = $headerSize + ($entrySize * $frames.Count)
    $fs = [System.IO.File]::Create($Path)
    $bw = [System.IO.BinaryWriter]::new($fs)

    $bw.Write([uint16]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]$frames.Count)

    foreach ($f in $frames) {
        $bw.Write([byte]($(if ($f.Size -ge 256) { 0 } else { $f.Size })))
        $bw.Write([byte]($(if ($f.Size -ge 256) { 0 } else { $f.Size })))
        $bw.Write([byte]0)
        $bw.Write([byte]0)
        $bw.Write([uint16]1)
        $bw.Write([uint16]32)
        $bw.Write([uint32]$f.Bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $f.Bytes.Length
    }
    foreach ($f in $frames) {
        $bw.Write([byte[]]$f.Bytes)
    }
    $bw.Dispose(); $fs.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Save-Png (Join-Path $OutDir 'voiceTyper.png') 256
Save-Png (Join-Path $OutDir 'voiceTyper_dark.png') 256
Save-Png (Join-Path $OutDir 'voiceTyper_light.png') 256
New-Icon (Join-Path $OutDir 'voiceTyper.ico')
New-Icon (Join-Path $OutDir 'voiceTyper_dark.ico')
New-Icon (Join-Path $OutDir 'voiceTyper_light.ico')

Write-Host "Icons written to $OutDir"