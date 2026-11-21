param(
    [string]$Iso = (Join-Path $PSScriptRoot '..\build\book8.iso'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\runtime-validation')
)

$ErrorActionPreference = 'Stop'

$VBoxManage = (Get-Command VBoxManage -ErrorAction SilentlyContinue).Source
if (-not $VBoxManage) {
    $candidate = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
    if (Test-Path -LiteralPath $candidate) {
        $VBoxManage = $candidate
    } else {
        throw 'VBoxManage was not found in PATH or its standard install location.'
    }
}

$Iso = (Resolve-Path -LiteralPath $Iso).Path
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$name = 'BOOK8-Graphics-Smoke-' + [guid]::NewGuid().ToString('N')
$vmRoot = Join-Path $OutputDirectory 'vbox-graphics-vm'
$vmDirectory = Join-Path $vmRoot $name
$serial = Join-Path $OutputDirectory 'vbox-graphics-input.serial.log'
$registered = $false

function Invoke-VBox {
    param(
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $VBoxManage @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "VBoxManage $($Arguments -join ' ') failed: $output"
    }
    return $output
}

function Wait-SerialText {
    param(
        [string]$Expected,
        [int]$TimeoutSeconds = 90,
        [switch]$CompleteLine
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $serial) {
            $reader = $null
            try {
                $stream = [System.IO.File]::Open(
                    $serial, [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::Read,
                    [System.IO.FileShare]::ReadWrite
                )
                $reader = New-Object System.IO.StreamReader($stream)
                $text = $reader.ReadToEnd()
            } catch [System.IO.IOException] {
                $text = ''
            } finally {
                if ($reader) {
                    $reader.Dispose()
                }
            }
            $position = $text.IndexOf($Expected)
            $lineComplete = -not $CompleteLine -or (
                $position -ge 0 -and $text.IndexOf("`n", $position) -ge 0
            )
            if ($position -ge 0 -and $lineComplete) {
                return $text
            }
        } else {
            $text = ''
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for '$Expected' in $serial"
}

try {
    [void](Invoke-VBox @(
        'createvm', '--name', $name, '--ostype', 'Other_64',
        '--basefolder', $vmRoot, '--register'
    ))
    $registered = $true
    [void](Invoke-VBox @(
        'modifyvm', $name, '--memory', '512', '--chipset', 'piix3',
        '--ioapic', 'on', '--firmware', 'bios',
        '--graphicscontroller', 'vboxvga', '--vram', '16',
        '--mouse', 'ps2', '--keyboard', 'ps2',
        '--usb-ohci', 'on', '--usb-ehci', 'on'
    ))
    [void](Invoke-VBox @('storagectl', $name, '--name', 'IDE', '--add', 'ide'))
    [void](Invoke-VBox @(
        'storageattach', $name, '--storagectl', 'IDE', '--port', '0',
        '--device', '0', '--type', 'dvddrive', '--medium', $Iso
    ))
    Remove-Item -LiteralPath $serial -Force -ErrorAction SilentlyContinue
    [void](Invoke-VBox @(
        'modifyvm', $name, '--uart1', '0x3f8', '4',
        '--uartmode1', 'file', $serial
    ))
    [void](Invoke-VBox @('startvm', $name, '--type', 'headless'))

    $log = Wait-SerialText 'BOOK8 x86-64 command interpreter'
    $framebuffer = [regex]::Match(
        $log, 'BOOK8 framebuffer handoff:\s*(\d+)x(\d+)x(24|32)'
    )
    if (-not $framebuffer.Success -or $log.Contains('BOOK8: VGA fallback active')) {
        throw 'VirtualBox did not hand off a supported framebuffer.'
    }
    $width = [int]$framebuffer.Groups[1].Value
    $height = [int]$framebuffer.Groups[2].Value
    Write-Output "[vbox] framebuffer ${width}x${height}x$($framebuffer.Groups[3].Value): PASS"

    Start-Sleep -Seconds 1
    [void](Invoke-VBox @(
        'controlvm', $name, 'keyboardputstring', "book8gui`n"
    ))
    [void](Wait-SerialText 'Launching BOOK8GUI.')

    $targetX = $width - 48
    $targetY = 52
    $remainingX = $targetX - [int]($width / 2)
    $remainingY = [int]($height / 2) - $targetY
    $mouseScript = Join-Path $PSScriptRoot 'vbox_mouse_inject.ps1'
    while ($remainingX -ne 0 -or $remainingY -ne 0) {
        $stepX = [Math]::Sign($remainingX) * [Math]::Min(80, [Math]::Abs($remainingX))
        $stepY = [Math]::Sign($remainingY) * [Math]::Min(80, [Math]::Abs($remainingY))
        & $mouseScript -VmName $name -DeltaX $stepX -DeltaY $stepY
        $remainingX -= $stepX
        $remainingY -= $stepY
    }
    & $mouseScript -VmName $name -DeltaX 0 -DeltaY 0 -ClickLeft

    [void](Invoke-VBox @(
        'controlvm', $name, 'keyboardputscancode', '01', '81'
    ))
    $log = Wait-SerialText 'BOOK8GUI closed.'
    [void](Invoke-VBox @(
        'controlvm', $name, 'keyboardputstring', "inputstat`n"
    ))
    $log = Wait-SerialText 'QUEUE_DEPTH=' -CompleteLine
    $inputLine = [regex]::Match($log, '(?m)^BOOK8-INPUT:.*$')
    if (-not $inputLine.Success) {
        throw 'VirtualBox input statistics were not printed.'
    }
    Write-Output "[vbox] $($inputLine.Value)"

    $values = @{}
    foreach ($match in [regex]::Matches($inputLine.Value, '([A-Z0-9_]+)=(\d+)')) {
        $values[$match.Groups[1].Value] = [int]$match.Groups[2].Value
    }
    if ($values['IRQ12'] -eq 0 -or $values['PS2_PACKETS'] -eq 0 `
            -or $values['PS2_QUEUED'] -eq 0 `
            -or $values['PS2_QUEUE_DROPS'] -ne 0) {
        throw 'VirtualBox PS/2 mouse events were not queued cleanly.'
    }
    Write-Output '[vbox] PS/2 IRQ12 mouse events delivered without queue drops: PASS'
    Write-Output "Serial log: $serial"
} finally {
    if ($registered) {
        [void](Invoke-VBox @('controlvm', $name, 'poweroff') -AllowFailure)
        Start-Sleep -Milliseconds 500
        [void](Invoke-VBox @('unregistervm', $name) -AllowFailure)
        if (Test-Path -LiteralPath $vmDirectory) {
            Remove-Item -LiteralPath $vmDirectory -Recurse -Force
        }
        if ((Test-Path -LiteralPath $vmRoot) `
                -and @(Get-ChildItem -LiteralPath $vmRoot -Force).Count -eq 0) {
            Remove-Item -LiteralPath $vmRoot -Force
        }
    }
}
