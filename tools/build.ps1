# Build the Main_MiSTer fork in docker.
# Compiles inside a named volume (fast, native fs) and copies bin/MiSTer back
# to Main_MiSTer\bin. First run builds the toolchain image (~600MB download).
#
#   .\tools\build.ps1            incremental build
#   .\tools\build.ps1 -Clean     wipe the object cache first

param([switch]$Clean)

$ErrorActionPreference = "Stop"
$repo = Join-Path $PSScriptRoot "..\Main_MiSTer" | Resolve-Path

docker build -t mister-armcc $PSScriptRoot
if ($LASTEXITCODE) { exit 1 }

if ($Clean) { docker volume rm -f mister-objcache | Out-Null }
docker volume create mister-objcache | Out-Null

docker run --rm -v "${repo}:/src" -v mister-objcache:/work mister-armcc bash -c @'
set -e
rsync -a --delete --exclude=.git --exclude=bin /src/ /work/src/
cd /work/src
make -j$(nproc)
mkdir -p /src/bin
cp bin/MiSTer bin/MiSTer.elf /src/bin/
echo "== build ok: bin/MiSTer =="
'@
exit $LASTEXITCODE
