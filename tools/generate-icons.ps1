# Regenerates the raster product icons from the current artwork:
#
#   assets\icon-256.png   256x256 PNG, the header image of README.md / README.ru.md
#   assets\voiceTyper.ico multi-frame icon for the executable (assets\voiceTyper.rc)
#
# The master is assets\voiceTyper.png (1536x1536, the Qt resource used by the window
# and the tray). It is a product asset in its own right and is NEVER overwritten by
# this script.
#
# History: the old defaults pointed at VoiceTyper.App\Assets (the .NET tree, removed in
# commit 76df03e) and at the root icon.png, which carries the obsolete .NET-era blue
# tile of commit 385a0f2 - neither matches the microphone artwork the product ships.
#
# Windows-only: System.Drawing is not available in PowerShell on Linux.
param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets'),
    [string]$Master = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\voiceTyper.png')
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

# Собирает многокадровый .ico (PNG-кадры) из мастер-файла $Master.
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

# The 256 px PNG is the README header; the .ico is the file icon of the executable.
# The master itself ($Master) is not an output: writing it here would replace the
# 1536x1536 product artwork with a 256x256 resize of itself.
Save-Png (Join-Path $OutDir 'icon-256.png') 256
New-Icon (Join-Path $OutDir 'voiceTyper.ico')

$Script:MasterImage.Dispose()

Write-Host "Icons written to $OutDir from $Master"
