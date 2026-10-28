# alioth WOA 驱动部署与验证手册

> 配套仓库：`G:\woa\woa-drivers\`（qcfakepep + aliothcpufreq，ARM64）
> 前提：手机 Windows 已开启测试签名（`bcdedit /set testsigning on`，作者 phase17a 已验证可行）

---

## 一、CPU 满频 POC（零编译，kd 直接写 EPSS）

目标：先在真机上验证"写 EPSS PERF_STATE = 频率立即变化"这一核心假设，成功后再部署 aliothcpufreq 驱动固化。

### 前置（一次性）

1. **手机临时启动带 DBG2 的 UEFI**（不落盘）：
   ```
   fastboot flash boot_b G:\woa\K40_Win11\UEFI-Images\Mu-alioth-1-acpi-hid-phase17g-base-dbg2-20261022-151745.img
   fastboot reboot
   ```
   （该镜像 = Phase16 DSDT + 修复 DBG2，启动 Windows 与 v3.4 行为一致，额外多出 KDNET 调试口）
   注意：如果想保守，可以先 `fastboot boot` 一次性试启动；确认稳定后 flash。
2. **手机开 KDNET**（WinRM 远程执行，或 Mass Storage 离线改 BCD）：
   ```
   bcdedit /dbgsettings net hostip:192.168.137.1 port:50000 key:1.1.1.1
   bcdedit /set debug on
   ```
   （key 需为 4 段点分数字；hostip = PC 网口 IP）
3. **PC 端 WinDbg**：已装好，kd.exe 位于
   `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe`

### 执行 POC

1. PC 上先启动 kd 监听：
   ```
   kd.exe -k net:port=50000,key=1.1.1.1 -y "srv*C:\\symbols*https://msdl.microsoft.com/download/symbols"
   ```
2. 重启手机（等它连接 kd）。kd 显示 "Connected to target"。
3. 读两个域的频率表（LUT）：
   ```
   kd> dd /p 0x185910100 L28      ← 大核域（CPU0-3）LUT
   kd> dd /p 0x185920100 L28      ← 小核域（CPU4-7）LUT
   ```
   解析：每行 4 字节（小端），低 8 位 = L_VAL，频率 = L_VAL × 19.2 MHz。
   预期大核域最高档 ≈ 0xA6（166 × 19.2 = 3187 MHz）；当前 PERF_STATE 索引看：
   ```
   kd> dd /p 0x185910320 L1       ← 当前请求档（预期 = 一个低档）
   kd> dd /p 0x185920320 L1
   ```
4. 写最大档（假设大核 LUT 最后一行有效索引为 N，逐行验证后填入）：
   ```
   kd> ed /p 0x185910320 N
   kd> ed /p 0x185920320 M
   ```
5. **立即生效验证**（三选一）：
   - kd 回读 `dd /p 0x185910320 L1` 与 `dd /p 0x185920020`（domain_state）
   - PC 端跑 7-Zip Benchmark（先跑一次基线再对比，预期大核吞吐 ×~2.1）
   - 手机 RDP 里用任务管理器看"速度"列（显示值可能仍是 1.51 GHz 假象，以 benchmark 为准）
6. **回退**：重启即恢复原状（寄存器不持久化）。

### 判读

| 结果 | 结论 | 下一步 |
| --- | --- | --- |
| 写入后 benchmark 提升 ~2 倍、系统稳定 | 核心假设成立 | 部署 aliothcpufreq 固化 |
| 写入后死机/蓝屏 | 读到的 LUT 数据异常（偏移/域基址需复核） | kd 抓现场，回读 LUT 原始值分析 |
| 写入成功但频率没变 | 该域 enable 位/电源域异常 | 读 0x185910000（enable）与 0x185910020，对照 Linux 驱动逐寄存器核对 |

---

## 二、qcfakepep 部署（音频链解锁）

编译产物：`qcfakepep.sys + qcfakepep.inf + qcfakepep.cat`（GitHub Actions 产出）。

### 安装证书（测试签名信任，一次性）

手机上（管理员 PowerShell / WinRM）：
```powershell
Import-Certificate -FilePath C:\deploy\nova-test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath C:\deploy\nova-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

### 安装驱动

```powershell
# 手机上
pnputil /add-driver C:\deploy\qcfakepep.inf /install
# 触发设备重枚举（或重启）
shutdown /r /t 5
```

### 验证（按作者仓库判据，逐级推进）

```powershell
# 1. PEP0 绑定成功（设备管理器 / PowerShell）
Get-PnpDevice | Where-Object { $_.InstanceId -like "*QCOM0519*" -or $_.InstanceId -like "*QCOM2517*" }
# 预期 Status=OK，驱动 qcfakepep

# 2. 接口已发布
Test-Path "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}"
# 预期 True

# 3. SMMU 解锁（按作者 phase16b 配置：qcsmmu Start=3 + ImagePath=qcsmmu8280，
#    脚本：K40_Win11\Alioth-Engineering\tools\Set-ServiceStart-Offline-Admin.ps1）
Get-PnpDevice | Where-Object { $_.InstanceId -like "*QCOM2509*" }
# 预期 MMU0/MMU1 从 Code 31 → OK

# 4. 逐个恢复 qcsubsys（Start=3，按作者 phase 系列顺序），观察 ADSP 状态码变化
```

### 回退

```powershell
pnputil /delete-driver oemXX.inf /uninstall   # 卸载后重启即恢复
```
最坏情况：设备照旧 Code 31（无害），驱动卸载即干净回退。

---

## 三、aliothcpufreq 部署（固化 CPU 满频）

POC 成功后部署。产物：`aliothcpufreq.sys + aliothcpufreq.inf + aliothcpufreq.cat`。

```powershell
# 手机上（管理员）
pnputil /add-driver C:\deploy\aliothcpufreq.inf /install
# Root 设备枚举时自动启动服务；或手动：
sc query aliothcpufreq
```

验证：重启后 7-Zip benchmark 与 POC 结果一致即成功。
回退：`pnputil /delete-driver ... /uninstall` + 重启。

注意：常驻满频发热上升，K40 散热弱，长时间高负载建议配合散热背夹；驱动已保留硬件 LMH 热限流兜底。

---

## 四、文件清单

| 文件 | 说明 |
| --- | --- |
| `qcfakepep/qcfakepep.c/.inf/.vcxproj` | 桩 PEP（纯 WDM，{55beca03}+{a0e1dd36} 桩） |
| `aliothcpufreq/aliothcpufreq.c/.inf/.vcxproj` | EPSS 拉频（legacy，LUT 扫描+写 max 档） |
| `.github/workflows/build.yml` | GitHub Actions 云编译（WDK NuGet + MSVC ARM64） |
| `PC 端 kd.exe` | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe`（已装） |
