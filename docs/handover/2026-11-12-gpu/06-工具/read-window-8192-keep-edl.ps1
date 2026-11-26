# Read the full 8192-block boot-log window over EDL - the window decode-thisboot.py needs.
# Does NOT reset the device: it leaves the tablet in EDL, so a second read costs no keypress.
#
# This is the portable copy: it takes no absolute path from the author's machine. Point -Toolbox at a
# Huawei/Qualcomm toolbox that provides tools\fastboot.exe, tools\qsaharaserver.exe, tools\fh_loader.exe
# and unlock\Huawei865870_devprg.elf; the XML descriptor is read from .\edl\ next to this script.
#
#   pwsh -File read-window-8192-keep-edl.ps1 -Toolbox 'C:\path\to\toolbox' -OutDir .\edl-log-s1
#
param(
    [Parameter(Mandatory = $true)][string]$Toolbox,
    [string]$OutDir = (Join-Path $PSScriptRoot 'edl-log')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$fb     = Join-Path $Toolbox 'tools\fastboot.exe'
$sahara = Join-Path $Toolbox 'tools\qsaharaserver.exe'
$fh     = Join-Path $Toolbox 'tools\fh_loader.exe'
$elf    = Join-Path $Toolbox 'unlock\Huawei865870_devprg.elf'
$xml    = Join-Path $PSScriptRoot 'edl\readback-window.xml'
foreach ($p in @($fb, $sahara, $fh, $elf, $xml)) {
    if (-not (Test-Path $p)) { throw "missing input: $p" }
}

function Get-EdlPort {
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match 'QDLoader|9008' } | Select-Object -First 1
    if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

$port = Get-EdlPort
if (-not $port) {
    Write-Host '== asking fastboot for EDL =='
    & $fb oem edl 2>&1 | ForEach-Object { Write-Host "   $_" }
    for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Seconds 2; $port = Get-EdlPort; if ($port) { break } }
}
if (-not $port) { Write-Host 'NO EDL PORT'; exit 1 }
Write-Host "port = $port"

Write-Host '== sahara programmer =='
$sp = Start-Process -FilePath $sahara -ArgumentList @('-p', "\\.\$port", '-s', "13:`"$elf`"") `
    -RedirectStandardOutput (Join-Path $OutDir 'sahara.log') `
    -RedirectStandardError  (Join-Path $OutDir 'sahara.err') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 12
if (-not $sp.HasExited) { Write-Host '   killing sahara so fh_loader can open the port'; try { $sp.Kill() } catch {} }
Start-Sleep -Seconds 2

Write-Host '== fh_loader whole window =='
Push-Location $OutDir
$o = & $fh "--port=\\.\$port" "--sendxml=$xml" "--search_path=$OutDir" --noprompt `
      --convertprogram2read --zlpawarehost=1 --memoryname=ufs --loglevel=info 2>&1
$rc = $LASTEXITCODE
Pop-Location
Write-Host "   fh_loader exit=$rc"
$o | Select-String -Pattern 'read>|Finished|ERROR' | ForEach-Object { Write-Host "   $($_.Line.Trim())" }

Get-ChildItem $OutDir -Filter '*.img' | ForEach-Object { Write-Host ("   {0}  {1} B" -f $_.Name, $_.Length) }
Write-Host 'DONE (device left in EDL)'
