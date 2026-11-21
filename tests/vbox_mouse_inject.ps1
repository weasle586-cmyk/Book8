param(
    [Parameter(Mandatory = $true)]
    [string]$VmName,
    [int]$DeltaX = 120,
    [int]$DeltaY = 80,
    [int]$Wheel = 0,
    [switch]$ClickLeft
)

$ErrorActionPreference = 'Stop'

$virtualBox = New-Object -ComObject VirtualBox.VirtualBox
$machine = $virtualBox.FindMachine($VmName)
if ($machine.State -ne 6) {
    throw "VirtualBox VM '$VmName' must be running; current machine state is $($machine.State)."
}

$session = New-Object -ComObject VirtualBox.Session
$locked = $false
try {
    $machine.LockMachine($session, 1)
    $locked = $true
    $mouse = $session.Console.Mouse
    $mouse.PutMouseEvent($DeltaX, $DeltaY, 0, 0, 0)
    if ($Wheel -ne 0) {
        $mouse.PutMouseEvent(0, 0, $Wheel, 0, 0)
    }
    if ($ClickLeft) {
        $mouse.PutMouseEvent(0, 0, 0, 0, 1)
        $mouse.PutMouseEvent(0, 0, 0, 0, 0)
    }
} finally {
    if ($locked) {
        $session.UnlockMachine()
    }
}

Write-Output "Sent relative mouse input to VirtualBox VM '$VmName'."
