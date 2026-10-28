# alioth-woa-drivers

Windows-on-ARM (ARM64) kernel drivers for Redmi K40 / POCO F3 / Mi 11X (`alioth`, Qualcomm SM8250).

Two experimental drivers, built on the reverse-engineering results of the
[K40_Win11 / Alioth-Engineering](https://github.com/zhishixuebao-0791/K40_Win11) project:

## qcfakepep — Stub PEP driver

Every downstream Qualcomm driver (SMMU, subsystem loader, GPU, serial, camera,
scm ...) queries the PEP0 ACPI device (`QCOM0519` / `QCOM2517`) for:

- device interface `{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}`
- `QueryInterface` `{a0e1dd36-dfb9-4c0e-bbaa-78f0746be181}` (size `0x28`, version 1, one function)

Neither official `qcpep8250` (writes SM8150 LMH registers) nor `qcpep8280`
(writes SC8280XP-only TSENS controllers) survives `AddDevice` on SM8250 — both
BugCheck `0x101`. The vendor's own "stubbed PEP for presilicon" mode answers
the `{a0e1dd36}` function with unconditional `STATUS_SUCCESS`; this driver
reproduces that semantics with **zero MMIO access**. Unlocks the whole audio
chain (SMMU → qcsubsys → ADSP) and lifts the PEP dependency from the GPU stack.

## aliothcpufreq — EPSS max-frequency driver

alioth ACPI has no `_PSS`/`_CPC`/`_PPC` and no working PEP, so Windows runs at
whatever clock AOP/UEFI left in EPSS (1517 MHz reported as the default value).
SM8250 frequency control is plain MMIO (verified against Linux
`qcom-cpufreq-hw.c`, EPSS variant): write the max LUT row index to
`0x185910320` / `0x185920320` (`PERF_STATE`), voltage applied by AOP, LMH
thermal limiting still enforced in hardware. This legacy driver scans the LUT
at load and requests the top row in both domains.

## Build

GitHub Actions builds both drivers for ARM64 (WDK via NuGet + MSVC ARM64 cross
tools). Artifacts under the workflow run page.

**Status: experimental.** Target requires `bcdedit /set testsigning on`.
Deploy/verification steps documented in the project docs (see `DEPLOY.md` in
the local engineering workspace).
