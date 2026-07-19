# Build the Main_MiSTer fork in docker.
# Compiles inside a named volume (fast, native fs) and copies bin/MiSTer back
# to Main_MiSTer\bin. First run builds the toolchain image (~600MB download).
#
#   .\tools\build.ps1            incremental build
#   .\tools\build.ps1 -Clean     wipe the object cache first
#
# PowerShell 5.1 notes, both learned the hard way:
#  - docker and git write progress to stderr. Under $ErrorActionPreference
#    = "Stop" that becomes a terminating NativeCommandError even on success,
#    so native calls run under "Continue" and we check $LASTEXITCODE instead.
#  - don't pipe make through grep under `set -e` inside the container: grep
#    exits 1 when it finds no warnings, which aborted the script before the
#    binary was copied out and silently shipped a stale build.
#  - use only SINGLE quotes inside the bash here-string. PowerShell strips
#    double quotes when passing an argument to a native command, so
#    grep -iE "a|b" reaches bash as an unquoted pipe and is a syntax error.

param([switch]$Clean)

$repo = Join-Path $PSScriptRoot "..\Main_MiSTer" | Resolve-Path
$ErrorActionPreference = "Continue"

docker build -t mister-armcc $PSScriptRoot
if ($LASTEXITCODE) { Write-Host "toolchain image build failed" -ForegroundColor Red; exit 1 }

if ($Clean) { docker volume rm -f mister-objcache | Out-Null }
docker volume create mister-objcache | Out-Null

docker run --rm -v "${repo}:/src" -v mister-objcache:/work mister-armcc bash -c @'
rsync -a --delete --exclude=.git --exclude=bin /src/ /work/src/
cd /work/src
if ! make -j$(nproc) > /tmp/build.log 2>&1; then tail -30 /tmp/build.log; exit 1; fi
grep -iE 'warning|error' /tmp/build.log || echo 'no warnings'
mkdir -p /src/bin
cp bin/MiSTer bin/MiSTer.elf /src/bin/
echo "== build ok: bin/MiSTer =="
'@

if ($LASTEXITCODE) { Write-Host "build failed" -ForegroundColor Red; exit 1 }
exit 0
