$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$isoPath = Join-Path $root 'build\book8.iso'
$qemuLog = Join-Path $root 'qemu.log'

if (-not (Test-Path $isoPath)) {
    throw "Missing BOOK8 ISO: $isoPath. Build it first with bash ./build.sh or the project build script."
}

Remove-Item $qemuLog -ErrorAction SilentlyContinue

$qemuArgs = @(
    '-M', 'pc',
    '-m', '512',
    '-no-reboot',
    '-no-shutdown',
    '-d', 'int,cpu_reset',
    '-D', $qemuLog,
    '-serial', 'stdio',
    '-cdrom', $isoPath
)

& qemu-system-x86_64 @qemuArgs
exit $LASTEXITCODE
