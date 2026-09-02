param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'VoiceTyper.App\Assets'),
    [string]$Master = (Join-Path (Split-Path $PSScriptRoot -Parent) 'icon.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Master)) {
    throw "Master icon not found: $Master"
}

$Script:MasterImage = [System.Drawing.Image]::FromFile($Master)

function Resize([int]$Px) {
    $bmp = [System.Drawing.Bitmap]::new($Px, $Px)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($Script:MasterImage, 0, 0, $Px, $Px)
    $g.Dispose()
    return $bmp
}

function Save-Png([string]$Path, [int]$Px) {
    $out = Resize $Px
    $out.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose()
}

# Собирает многокадровый .ico (PNG-кадры) из исходного icon.png.
function New-Icon([string]$Path) {
    $sizes = @(256, 64, 48, 32, 16)
    $frames = @()
    foreach ($sz in $sizes) {
        $out = Resize $sz
        $ms = [System.IO.MemoryStream]::new()
        $out.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += ,@{ Size = $sz; Bytes = $ms.ToArray() }
        $ms.Dispose(); $out.Dispose()
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
New-Icon (Join-Path $OutDir 'voiceTyper.ico')

$Script:MasterImage.Dispose()

Write-Host "Icons written to $OutDir from $Master"
