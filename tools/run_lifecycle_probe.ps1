param(
    [int]$WaitForGameSeconds = 600,
    [int]$CaptureSeconds = 600
)

$ErrorActionPreference = 'Stop'
$serial = '192.168.5.102:5555'
$root = Split-Path -Parent $PSScriptRoot
$serverLocal = Join-Path $root 'tools\downloads\frida-17.17.0\frida-server'
$serverDevice = '/data/local/tmp/frida-server'
$probe = Join-Path $PSScriptRoot 'live_lifecycle_probe.js'
$log = Join-Path $root ('output\lifecycle_probe_' +
    (Get-Date -Format 'yyyyMMdd_HHmmss') + '.jsonl')

if (-not (Test-Path -LiteralPath $serverLocal)) { throw 'Frida server binary is missing' }
if (-not (Test-Path -LiteralPath $probe)) { throw 'Probe script is missing' }
if (-not (Get-Command frida -ErrorAction SilentlyContinue)) { throw 'Frida CLI is missing' }

adb connect $serial | Out-Null
$moduleVersion = adb -s $serial shell su -c cat /data/adb/modules/rt_shim/module.prop
if (-not ($moduleVersion -match 'versionCode=286')) {
    throw 'Device is not running the expected 2.8.4 module'
}
$payloadHash = adb -s $serial shell su -c sha256sum /data/adb/modules/rt_shim/payload/libgcloudsync.so
if (-not ($payloadHash -match '^a8a4bf1e221842769ab75c54c2f23fbba91e156838275e4530f666a3ce112b2d')) {
    throw 'Device payload does not match the probe RVAs'
}

$fridaPid = (adb -s $serial shell pidof frida-server 2>$null | Out-String).Trim()
$ownServer = $false
if (-not $fridaPid) {
    adb -s $serial push $serverLocal $serverDevice | Out-Null
    adb -s $serial shell su -c chmod 700 $serverDevice | Out-Null
    $launch = Start-Process -FilePath (Get-Command adb).Source -ArgumentList @(
        '-s', $serial, 'shell', 'su', '-c', $serverDevice,
        '-l', '127.0.0.1:27042', '-D') -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Seconds 1
        $fridaPid = (adb -s $serial shell pidof frida-server 2>$null | Out-String).Trim()
        if ($fridaPid) { break }
    }
    if (-not $launch.HasExited) { Stop-Process -Id $launch.Id -Force }
    if (-not $fridaPid) { throw 'Frida server did not start' }
    $ownServer = $true
}
adb -s $serial forward tcp:27042 tcp:27042 | Out-Null

try {
    $deadline = (Get-Date).AddSeconds($WaitForGameSeconds)
    $gamePid = ''
    while ((Get-Date) -lt $deadline) {
        $gamePid = (adb -s $serial shell pidof com.vnggames.cfl.crossfirelegends 2>$null |
            Out-String).Trim()
        if ($gamePid) { break }
        Start-Sleep -Seconds 2
    }
    if (-not $gamePid) { throw 'Game did not start before the timeout' }
    [pscustomobject]@{event='capture_start'; pid=$gamePid; log=$log} |
        ConvertTo-Json -Compress | Out-File -LiteralPath $log -Encoding utf8
    & frida -H 127.0.0.1:27042 -p $gamePid -q -l $probe --timeout $CaptureSeconds *>> $log
} finally {
    adb -s $serial forward --remove tcp:27042 | Out-Null
    if ($ownServer) {
        $fridaPid = (adb -s $serial shell pidof frida-server 2>$null | Out-String).Trim()
        if ($fridaPid) { adb -s $serial shell su -c kill $fridaPid | Out-Null }
    }
}
