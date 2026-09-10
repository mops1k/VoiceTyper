<#PSScriptInfo
Сборка нативной parakeet.dll (mudler/parakeet.cpp) для VoiceTyper.
MinGW (WinLibs gcc) + Ninja, статическая линковка CRT (DLL самодостаточна).

Использование:
  .\tools\build-parakeet-native.ps1 -Ref <SHA или ветка> [-OutDir .\tmpbuild]

Артефакты в -OutDir: parakeet.dll, BUILD.txt.
#>
param(
    [string]$Ref = "master",
    [string]$OutDir = "$PSScriptRoot\..\artifacts\parakeet-native"
)

$ErrorActionPreference = "Stop"

$ToolchainDir = "C:\Users\Kvintilyanov\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$env:PATH = "$ToolchainDir;$env:PATH"

$RepoUrl = "https://github.com/mudler/parakeet.cpp"
$TmpRoot = "$env:TEMP\voicetyper-parakeet-build"
$SrcDir = "$TmpRoot\parakeet.cpp"
$BuildDir = "$TmpRoot\build"

if (-not (Test-Path $TmpRoot)) { New-Item -ItemType Directory -Path $TmpRoot | Out-Null }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

if (Test-Path $SrcDir) {
    git -C $SrcDir fetch --all --tags 2>$null
} else {
    git clone --recursive $RepoUrl $SrcDir
}
if ($LASTEXITCODE -ne 0) { throw "git clone/fetch failed" }

git -C $SrcDir checkout $Ref
if ($LASTEXITCODE -ne 0) { throw "git checkout $Ref failed" }
git -C $SrcDir submodule update --init --recursive
git -C $SrcDir rev-parse HEAD | Set-Content -Path "$TmpRoot\commit.txt"

# Патч MinGW: backend.hpp использует int64_t без #include <cstdint>.
$BackendHpp = "$SrcDir\src\backend.hpp"
$HppText = Get-Content $BackendHpp -Raw -Encoding utf8
if ($HppText -notmatch "#include <cstdint>") {
    $Patched = $HppText -replace "(#include <string>)", "`$1`r`n#include <cstdint>"
    [System.IO.File]::WriteAllText($BackendHpp, $Patched)
    Write-Host "Патч применён: src/backend.hpp += #include <cstdint>"
}

cmake -S $SrcDir -B $BuildDir -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DPARAKEET_SHARED=ON -DPARAKEET_BUILD_CLI=OFF `
  -DPARAKEET_BUILD_SERVER=OFF -DPARAKEET_BUILD_TESTS=OFF `
  -DGGML_NATIVE=OFF -DGGML_LLAMAFILE=ON -DGGML_OPENMP=OFF `
  -DCMAKE_SHARED_LIBRARY_PREFIX="" `
  -DCMAKE_SHARED_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

# Найти артефакт (Ninja на MinGW приводит имя к libparakeet.dll)
$Dll = Get-ChildItem -Path $BuildDir -Recurse -Filter "*parakeet.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $Dll) { throw "parakeet.dll not found in $BuildDir" }

$Commit = (Get-Content "$TmpRoot\commit.txt").Trim()
$GccVersion = (gcc --version | Select-Object -First 1)
$CmakeVersion = (cmake --version | Select-Object -First 1)
$Date = (Get-Date -Format "yyyy-MM-dd")
$Exports = (objdump -p $Dll.FullName | Select-String "parakeet_capi_")
$Dependents = (objdump -p $Dll.FullName | Select-String -Pattern "DLL Name")

$FinalDll = Join-Path $OutDir "parakeet.dll"
Copy-Item $Dll.FullName $FinalDll -Force

@"
Repo: $RepoUrl
Pinned commit: $Commit
Date: $Date
Toolchain: $GccVersion
           $CmakeVersion
CMake flags: PARAKEET_SHARED=ON, PARAKEET_BUILD_CLI/SERVER/TESTS=OFF,
  GGML_NATIVE=OFF, GGML_LLAMAFILE=ON, GGML_OPENMP=OFF
  CMAKE_SHARED_LINKER_FLAGS=-static -static-libgcc -static-libstdc++

--- exports (parakeet_capi_*) ---
$Exports

--- dependents ---
$Dependents
"@ | Set-Content -Path "$OutDir\BUILD.txt" -Encoding utf8

Write-Host "OK: $($Dll.FullName) -> $OutDir"
