#!/usr/bin/env bash
# Regenerates the Phase A settings golden fixtures from the real VoiceTyper serializer.
#
# Why this is a two-step (build on one runtime, generate on another):
#   * VoiceTyper.Core.csproj targets net10.0-windows and pulls NAudio/Whisper.net, which
#     reference Microsoft.WindowsDesktop.App.WindowsForms. Building the product project on a
#     non-Windows host fails with NETSDK1073, so the generator compiles the two real product
#     source files (AppSettings.cs, SettingsService.cs) directly instead of referencing the
#     project. The types and SettingsService.JsonOptions are the product's, unmodified.
#   * System.Text.Json WriteIndented emits Environment.NewLine. A real VoiceTyper on Windows
#     therefore writes CRLF, so the goldens are produced by running the generator on the
#     Windows runtime (CRLF). Running the same binary on Linux produces the same document
#     with LF endings and different hashes - do not commit that variant.
#
# Prerequisites:
#   * DOTNET      - .NET 10 SDK used to build (verified: SDK 10.0.401 on Arch Linux)
#   * WINDOWS_DOTNET - Windows .NET 10 runtime (verified: C:\Program Files\dotnet\dotnet.exe,
#                      Microsoft.NETCore.App 10.0.11; the Windows machine has no SDK)
#   * the staging directory must be visible to Windows, e.g. /mnt/c/Users/<you>/AppData/Local/Temp
#
# Usage: tools/generate-settings-fixtures/generate.sh [output-directory]
#        default output-directory: tests/fixtures/migration/settings
#
# Exit code 0 means all five files were written and verified; any failed step aborts.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
output_directory="${1:-${repo_root}/tests/fixtures/migration/settings}"
build_dotnet="${DOTNET:-dotnet}"
windows_dotnet="${WINDOWS_DOTNET:-/mnt/c/Program Files/dotnet/dotnet.exe}"

# The WSL user name usually differs from the Windows one, so probe the Windows profiles.
stage_root="${STAGE_DIR:-}"
if [ -z "${stage_root}" ]; then
    for candidate in /mnt/c/Users/*/AppData/Local/Temp; do
        if [ -d "${candidate}" ] && [ -w "${candidate}" ]; then
            stage_root="${candidate}"
            break
        fi
    done
fi

command -v "${build_dotnet}" >/dev/null 2>&1 || { echo "error: no .NET SDK: set DOTNET=<dotnet>" >&2; exit 2; }
[ -x "${windows_dotnet}" ] || { echo "error: no Windows dotnet.exe: set WINDOWS_DOTNET=<path>" >&2; exit 2; }
[ -d "${stage_root}" ] || { echo "error: staging dir not found: ${stage_root} (set STAGE_DIR to a /mnt/c path)" >&2; exit 2; }

work_dir="$(mktemp -d "${stage_root}/vt-settings-fixtures.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

# The generator project is temporary: nothing lands in the repository except the fixtures.
cat > "${work_dir}/generator.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="${script_dir}/Program.cs" Link="Program.cs" />
    <Compile Include="${repo_root}/VoiceTyper.Core/Models/AppSettings.cs" Link="Product/AppSettings.cs" />
    <Compile Include="${repo_root}/VoiceTyper.Core/Services/SettingsService.cs" Link="Product/SettingsService.cs" />
  </ItemGroup>
</Project>
EOF

echo "== build (host SDK: ${build_dotnet}) =="
"${build_dotnet}" build "${work_dir}/generator.csproj" -c Release -v minimal --nologo
echo "build exit=$?"

echo "== generate (Windows runtime: ${windows_dotnet}) =="
mkdir -p "${work_dir}/app" "${output_directory}"
cp "${work_dir}/bin/Release/net10.0/generator.dll" \
   "${work_dir}/bin/Release/net10.0/generator.runtimeconfig.json" \
   "${work_dir}/bin/Release/net10.0/generator.deps.json" \
   "${work_dir}/app/"
(cd "${work_dir}/app" && "${windows_dotnet}" generator.dll "$(wslpath -w "${output_directory}")")
echo "generate exit=$?"

echo "== sha256 =="
(cd "${output_directory}" && sha256sum ./*.json)

echo "== shape (crlf lines / total lines / bom / bytes) =="
(cd "${output_directory}" && for file in ./*.json; do
    printf '%s crlf=%s lines=%s bom=%s bytes=%s\n' \
        "$(basename "${file}")" \
        "$(grep -c $'\r$' "${file}" || true)" \
        "$(wc -l < "${file}")" \
        "$(head -c 3 "${file}" | od -An -tx1 | tr -d ' \n' | grep -c '^efbbbf$' || true)" \
        "$(wc -c < "${file}")"
done)
