# offline-deploy.ps1 - Offline deployment of qcfakepep + aliothcpufreq into the
# phone's Windows partition exposed via UEFI Mass Storage.
#
# Usage: .\offline-deploy.ps1 -DriveLetter F
#   where F is the drive letter of the phone's Windows (C:) partition.
#
# Steps:
#   1. DISM /Add-Driver /ForceUnsigned  (qcfakepep: PnP function driver for PEP0)
#   2. Offline SYSTEM hive: register aliothcpufreq kernel service (Start=1)
#   3. Import the self-signed cert locally, then copy its registry trust entries
#      (Root + TrustedPeople) into the offline SOFTWARE hive
#   4. Unload hives with verification (re-load + re-check after unload)
#
# All registry writes verified per G:\woa playbook (silent-drop protection).

param(
    [Parameter(Mandatory = $true)][string]$DriveLetter
)

$ErrorActionPreference = 'Stop'
$Win = "$($DriveLetter):"
$Stage = "G:\woa\woa-drivers\stage"
$CertFile = "$Stage\nova-woa-test.cer"
$ExpectedTP = 'DE3A67B9B17315C53D30A2E4D5DF1ECEE95A0DDF'   # CN=Nova WOA Test

if (-not (Test-Path "$Win\Windows\System32\config\SYSTEM")) { throw "$Win does not look like the phone's Windows partition (SYSTEM hive missing)" }
if (-not (Test-Path $CertFile)) { throw "Certificate file missing: $CertFile" }

function Verify-RegWrite {
    param($Path, $Name, $Expected)
    $v = (Get-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($v -ne $Expected) { throw "VERIFY FAILED: $Path\$Name = $v (expected $Expected)" }
    Write-Host "  verified: $Name = $v"
}

Write-Host "=== 1. DISM Add-Driver (qcfakepep) ==="
dism /Image:$Win /Add-Driver /Driver:$Stage\qcfakepep.inf /ForceUnsigned
if ($LASTEXITCODE -ne 0) { throw "DISM Add-Driver failed ($LASTEXITCODE)" }

Write-Host "`n=== 2. Offline service registration: aliothcpufreq ==="
reg load HKLM\ALIOTH_SYS "$Win\Windows\System32\config\SYSTEM" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg load SYSTEM failed ($LASTEXITCODE)" }
try {
    $svc = 'HKLM\ALIOTH_SYS\Services\aliothcpufreq'
    New-Item -Path $svc -Force | Out-Null
    New-ItemProperty -Path $svc -Name 'Type'        -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $svc -Name 'Start'       -Value 1 -PropertyType DWord -Force | Out-Null   # SYSTEM start
    New-ItemProperty -Path $svc -Name 'ErrorControl'-Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $svc -Name 'ImagePath'   -Value 'System32\drivers\aliothcpufreq.sys' -PropertyType ExpandString -Force | Out-Null
    Verify-RegWrite $svc 'Start' 1
    Verify-RegWrite $svc 'ImagePath' 'System32\drivers\aliothcpufreq.sys'
} finally {
    reg unload HKLM\ALIOTH_SYS | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg unload SYSTEM failed ($LASTEXITCODE) - do NOT proceed" }
}
# re-load verification (silent-drop protection, per playbook)
reg load HKLM\ALIOTH_SYS "$Win\Windows\System32\config\SYSTEM" | Out-Null
$v = (Get-ItemProperty 'HKLM\ALIOTH_SYS\Services\aliothcpufreq' -Name Start -ErrorAction SilentlyContinue).Start
reg unload HKLM\ALIOTH_SYS | Out-Null
if ($v -ne 1) { throw "POST-UNLOAD VERIFY FAILED: aliothcpufreq Start = $v" }
Write-Host "  post-unload re-verify: Start = 1 OK"

Write-Host "`n=== 3. Certificate trust injection ==="
# 3a. import locally (CurrentUser My already has it if this PC ran build.ps1)
Import-Certificate -FilePath $CertFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $CertFile -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null

# 3b. copy machine trust registry entries from this PC into the offline SOFTWARE hive
reg load HKLM\ALIOTH_SW "$Win\Windows\System32\config\SOFTWARE" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg load SOFTWARE failed ($LASTEXITCODE)" }
try {
    foreach ($store in @('Root', 'TrustedPeople')) {
        $src = "HKLM:\SOFTWARE\Microsoft\SystemCertificates\$store\Certificates\$ExpectedTP"
        if (-not (Test-Path $src)) {
            # fall back: find by thumbprint anywhere
            $src = (Get-ChildItem "HKLM:\SOFTWARE\Microsoft\SystemCertificates\$store\Certificates" -ErrorAction SilentlyContinue |
                    Where-Object { $_.PSChildName -eq $ExpectedTP }).PSPath
        }
        if (-not $src) { throw "cert $ExpectedTP not found in local $store store - did local import succeed?" }
        $dst = "HKLM:\ALIOTH_SW\Microsoft\SystemCertificates\$store\Certificates\$ExpectedTP"
        New-Item -Path $dst -Force | Out-Null
        Copy-ItemProperty -Path $src -Destination $dst
        if (-not (Test-Path $dst)) { throw "copy to offline hive failed: $dst" }
        Write-Host "  copied $store\$ExpectedTP"
    }
} finally {
    reg unload HKLM\ALIOTH_SW | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg unload SOFTWARE failed ($LASTEXITCODE) - do NOT proceed" }
}
# re-load verification
reg load HKLM\ALIOTH_SW "$Win\Windows\System32\config\SOFTWARE" | Out-Null
$ok = Test-Path 'HKLM:\ALIOTH_SW\Microsoft\SystemCertificates\TrustedPeople\Certificates\DE3A67B9B17315C53D30A2E4D5DF1ECEE95A0DDF'
reg unload HKLM\ALIOTH_SW | Out-Null
if (-not $ok) { throw "POST-UNLOAD VERIFY FAILED: TrustedPeople cert missing" }
Write-Host "  post-unload re-verify: TrustedPeople cert OK"

Write-Host "`n=== 4. Copy binaries as backup path ==="
# DISM keeps qcfakepep.sys in DriverStore; also drop copies for direct reference
New-Item -ItemType Directory -Force -Path "$Win\Windows\System32\drivers" | Out-Null
Copy-Item "$Stage\aliothcpufreq.sys" "$Win\Windows\System32\drivers\aliothcpufreq.sys" -Force
if (-not (Test-Path "$Win\Windows\System32\drivers\aliothcpufreq.sys")) { throw "sys copy failed" }
Write-Host "  aliothcpufreq.sys in place"

Write-Host "`n=== DONE. Safely eject the drive, exit Mass Storage, reboot the phone. ==="
Write-Host "After boot: check 7-zip benchmark (expect ~2.1x big-core) and Get-PnpDevice QCOM0519."
