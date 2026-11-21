param([switch]$Diagnostic, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    throw 'WSL is required to build the BOOK8 ISO. Install WSL and Ubuntu, then install the packages listed in README.md.'
}

$distributions = (& $wsl.Source --list --quiet 2>$null) -join "`n"
$distributions = $distributions -replace "`0", ''
if ($LASTEXITCODE -ne 0 -or $distributions -notmatch '(?im)^\s*Ubuntu\s*$') {
    throw 'An installed WSL Ubuntu distribution is required. Run "wsl --install --distribution Ubuntu", restart Windows if prompted, and try again.'
}

$linuxProject = (& $wsl.Source --exec wslpath -a $PSScriptRoot).Trim()
if ($LASTEXITCODE -ne 0 -or -not $linuxProject) {
    throw 'Could not resolve the BOOK8 project path inside WSL.'
}

$diagFlags = $env:BOOK8_DIAG_CFLAGS
# Forward network/push build settings only when set, so build.sh defaults apply.
$forward = @('BOOK8_NET_IP', 'BOOK8_NET_MASK', 'BOOK8_NET_GATEWAY', 'BOOK8_TELEMETRY_TARGET',
    'BOOK8_PUSH', 'BOOK8_SKIP_RING3_SMOKE')
$exports = ''
if ($Diagnostic) { $exports += "BOOK8_DIAGNOSTIC=1 " }
if ($BuildOnly) { $exports += "BOOK8_BUILD_ONLY=1 " }
foreach ($name in $forward) {
    $item = Get-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        if ($item.Value -notmatch '^[0-9A-Za-z.]*$') {
            throw "$name contains unsupported characters: $($item.Value)"
        }
        $exports += "$name='$($item.Value)' "
    }
}
$command = "cd '$linuxProject' && BOOK8_DIAG_CFLAGS='$diagFlags' $exports bash ./build.sh"
& $wsl.Source --distribution Ubuntu --user root --exec bash -lc $command
if ($LASTEXITCODE -ne 0) {
    throw "BOOK8 ISO build failed with exit code $LASTEXITCODE."
}
