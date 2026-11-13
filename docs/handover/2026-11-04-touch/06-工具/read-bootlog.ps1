# Read the official boot-log header back over EDL (read-only) and decode it.
# Starts from whatever state: if fastboot is present it issues `oem edl`; if EDL is already there it
# just uses it. Ends by leaving EDL with the Firehose power reset.
param(
    [string]$Xml    = 'C:\Users\HW\Documents\DSH2ZONE\work\verify\edl\readback-bootlog-header.xml',
    [string]$OutDir = 'C:\Users\HW\Documents\DSH2ZONE\evidence\edl-log'
)
$ErrorActionPreference = 'Stop'
Set-Location 'C:\Users\HW\Documents\DSH2ZONE'
# fh_loader is invoked after Push-Location $OutDir, so a relative -Xml would resolve against the
# output directory and fail with "'...xml' not found" (hit on 2026-11-02). Anchor both paths now.
$Xml    = (Resolve-Path $Xml).Path
$OutDir = (New-Item -ItemType Directory -Force -Path $OutDir).FullName

$toolbox = 'C:\Users\HW\Desktop\华为高通工具箱'
$fb      = Join-Path $toolbox 'tools\fastboot.exe'
$sahara  = Join-Path $toolbox 'tools\qsaharaserver.exe'
$fh      = Join-Path $toolbox 'tools\fh_loader.exe'
$elf     = Join-Path $toolbox 'unlock\Huawei865870_devprg.elf'

function Get-EdlPort {
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match 'QDLoader|9008' } | Select-Object -First 1
    if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

$port = Get-EdlPort
if (-not $port) {
    Write-Host '== not in EDL; asking fastboot for it =='
    & $fb oem edl 2>&1 | ForEach-Object { Write-Host "   $_" }
    for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Seconds 2; $port = Get-EdlPort; if ($port) { break } }
}
if (-not $port) { Write-Host 'NO EDL PORT'; exit 1 }
Write-Host "port = $port"

Write-Host '== sahara programmer =='
$sp = Start-Process -FilePath $sahara -ArgumentList @("-p", "\\.\$port", "-s", "13:`"$elf`"") `
    -RedirectStandardOutput (Join-Path $OutDir 'sahara.log') `
    -RedirectStandardError (Join-Path $OutDir 'sahara.log.err') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 12
if (-not $sp.HasExited) { Write-Host '   sahara still running; killing it so fh_loader can open the port'; try { $sp.Kill() } catch {} }
Start-Sleep -Seconds 2

Write-Host '== fh_loader readback (16 blocks, read-only) =='
Push-Location $OutDir
$o = & $fh "--port=\\.\$port" "--sendxml=$Xml" "--search_path=$OutDir" --noprompt `
      --convertprogram2read --zlpawarehost=1 --memoryname=ufs --loglevel=info 2>&1
$rc = $LASTEXITCODE
Pop-Location
Write-Host "   fh_loader exit=$rc"
$o | Select-String -Pattern 'read>|Overall|Finished|ERROR' | ForEach-Object { Write-Host "   $($_.Line.Trim())" }

$img = Join-Path $OutDir 'bootlog-header.img'
if (Test-Path $img) {
    Write-Host '== decode =='
    $b = [System.IO.File]::ReadAllBytes($img)
    $magic = [System.Text.Encoding]::ASCII.GetString($b, 0, 8)
    Write-Host ("   magic          : {0}" -f $magic)
    if ($magic -eq 'SHLOG001') {
        $version   = [BitConverter]::ToUInt32($b, 8)
        $firstLba  = [BitConverter]::ToUInt64($b, 40)
        $winBlocks = [BitConverter]::ToUInt32($b, 48)
        Write-Host ("   version        : {0}" -f $version)
        Write-Host ("   first_lba      : 0x{0:X}" -f $firstLba)
        Write-Host ("   window_blocks  : {0}" -f $winBlocks)
        Write-Host '   (full field map is in package/BootLog.c struct boot_log_header)'
    } else {
        Write-Host '   not a boot-log header: the writer did not run or the region is untouched'
        Write-Host ("   first 64 bytes : {0}" -f (($b[0..63] | ForEach-Object { $_.ToString('X2') }) -join ' '))
    }
} else {
    Write-Host '   no image produced'
}

Write-Host '== leave EDL =='
& $fh "--port=\\.\$port" '--power=reset,1' --noprompt --memoryname=ufs --loglevel=info 2>&1 |
    Select-String -Pattern 'Finished|ERROR' | ForEach-Object { Write-Host "   $($_.Line.Trim())" }
Write-Host 'DONE'
