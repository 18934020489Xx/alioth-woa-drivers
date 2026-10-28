# build.ps1 - Compile the two WDM drivers for ARM64 without WDK MSBuild integration.
# Needs only: MSVC (VS2022/VS2026) + Microsoft.Windows.WDK.ARM64 NuGet package.
# Output: stage\ with .sys/.inf/.cat/.cer

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Pkg  = Join-Path $Root 'packages'
$Out  = Join-Path $Root 'stage'
New-Item -ItemType Directory -Force -Path $Out, (Join-Path $Root 'build') | Out-Null

# ---------- 1. locate MSVC ARM64 cross compiler ----------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio with VC tools found" }
$msvc = (Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$cl   = "$msvc\bin\Hostx64\arm64\cl.exe"
$link = "$msvc\bin\Hostx64\arm64\link.exe"
if (-not (Test-Path $cl)) { throw "cl.exe (Hostx64/arm64) not found under $msvc" }
Write-Host "MSVC   : $msvc"

# ---------- 2. locate WDK NuGet content (recursive search, layout-agnostic) ----------
$wdkPkg = (Get-ChildItem "$Pkg\Microsoft.Windows.WDK.ARM64*" -Directory | Select-Object -First 1).FullName
Write-Host "WDK pkg: $wdkPkg"

$wdmHeader  = Get-ChildItem "$wdkPkg" -Recurse -Filter "wdm.h" | Select-Object -First 1
$ntoskrnlLib = Get-ChildItem "$wdkPkg" -Recurse -Filter "ntoskrnl.lib" | Select-Object -First 1
if (-not $wdmHeader)   { Get-ChildItem "$wdkPkg" -Directory -Recurse -Depth 3 | Select FullName; throw "wdm.h not found in WDK package" }
if (-not $ntoskrnlLib) { Get-ChildItem "$wdkPkg" -Directory -Recurse -Depth 3 | Select FullName; throw "ntoskrnl.lib not found in WDK package" }

$wdkIncRoot = $wdmHeader.Directory.Parent.FullName      # ...\Include\<ver>
$wdkLibRoot = $ntoskrnlLib.Directory.Parent.FullName    # ...\Lib\<ver>\km\arm64
Write-Host "WDK inc: $wdkIncRoot"
Write-Host "WDK lib: $wdkLibRoot"

$incArgs = @(
    "$wdkIncRoot\km", "$wdkIncRoot\km\crt", "$wdkIncRoot\shared",
    "$msvc\include"
) | ForEach-Object { "/I`"$_`"" }

$libArgs = @(
    "$wdkLibRoot",
    "$msvc\lib\arm64"
) | ForEach-Object { "/LIBPATH:`"$_`"" }

$commonDefines = "/D_ARM64_","/DARM64","/D_WIN64_","/D_M_ARM64",
    "/D_WIN32_WINNT=0x0A00","/DWINVER=0x0A00","/DNTDDI_VERSION=0x0A00000B"

# ---------- 3. compile + link each driver ----------
$targets = @(
    @{ Name='qcfakepep';      Src='qcfakepep\qcfakepep.c';         Libs=@('ntoskrnl.lib','hal.lib') },
    @{ Name='aliothcpufreq';  Src='aliothcpufreq\aliothcpufreq.c'; Libs=@('ntoskrnl.lib','hal.lib') }
)

foreach ($t in $targets) {
    $name = $t.Name
    $obj  = Join-Path $Root "build\$name.obj"
    Write-Host "`n=== CL $name ==="
    & $cl /nologo /c /kernel /GS- /W4 /WX- /O2 /Oi @commonDefines $incArgs "/Fo$obj" (Join-Path $Root $t.Src)
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $name ($LASTEXITCODE)" }

    Write-Host "=== LINK $name ==="
    & $link /nologo /OUT:"$Out\$name.sys" /MACHINE:ARM64 /SUBSYSTEM:NATIVE,10.00 /DRIVER `
        /ENTRY:DriverEntry /NODEFAULTLIB /RELEASE /PDB:"$Out\$name.pdb" `
        $libArgs ($t.Libs) $obj
    if ($LASTEXITCODE -ne 0) { throw "link failed for $name ($LASTEXITCODE)" }
}

# ---------- 4. copy INFs ----------
Copy-Item "$Root\qcfakepep\qcfakepep.inf" $Out -Force
Copy-Item "$Root\aliothcpufreq\aliothcpufreq.inf" $Out -Force

# ---------- 5. inf2cat (best effort) ----------
$inf2cat = $null
foreach ($cand in @(
    (Get-ChildItem "$wdkPkg" -Recurse -Filter inf2cat.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName,
    (& $vswhere -latest -find "**\inf2cat.exe" | Select-Object -First 1)
)) { if ($cand) { $inf2cat = $cand; break } }

if ($inf2cat) {
    Write-Host "inf2cat: $inf2cat"
    & $inf2cat /driver:$Out /os:10_ARM64
    if ($LASTEXITCODE -ne 0) { Write-Warning "inf2cat exited $LASTEXITCODE - check INF issues" }
} else {
    Write-Warning "inf2cat.exe not found - catalogs will not be generated"
}

# ---------- 6. self-sign (testsigning trust) ----------
$signtool = & $vswhere -latest -find "**\signtool.exe" | Select-Object -First 1
if ($signtool) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Nova WOA Test" `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
    Export-Certificate -Cert $cert -FilePath "$Out\nova-woa-test.cer" | Out-Null
    $catFiles = (Get-ChildItem "$Out\*.cat" -ErrorAction SilentlyContinue).FullName
    & $signtool sign /fd sha256 /n "Nova WOA Test" "$Out\qcfakepep.sys" "$Out\aliothcpufreq.sys" @catFiles
    if ($LASTEXITCODE -ne 0) { Write-Warning "signtool exited $LASTEXITCODE" }
    Write-Host "Signed with CN=Nova WOA Test (thumbprint $($cert.Thumbprint))"
} else {
    Write-Warning "signtool not found - binaries left unsigned (testsigning still required)"
}

Write-Host "`n=== STAGE ==="
Get-ChildItem $Out | Select-Object Name, Length | Format-Table -AutoSize
