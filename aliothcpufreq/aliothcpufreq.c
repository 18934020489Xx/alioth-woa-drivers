/*
 * aliothcpufreq.c - Boot-time EPSS max-frequency driver for SM8250 (alioth)
 *
 * Problem:
 *   alioth's ACPI (DSDT/SSDT/PPTT) contains no _PSS/_CPC/_PPC and no PEP driver
 *   can run on SM8250 (both official qcpep builds access nonexistent hardware).
 *   Windows therefore reports its default 1517 MHz and no frequency management
 *   exists; the actual clocks are whatever AOP/UEFI left in EPSS at boot.
 *
 * Mechanism (verified against Linux v6.12 drivers/cpufreq/qcom-cpufreq-hw.c,
 * "qcom,cpufreq-epss" variant used by SM8250):
 *   - Domain 0 (CPU0-3, Cortex-A77): base 0x18591000
 *   - Domain 1 (CPU4-7, Cortex-A55): base 0x18592000
 *   - reg_freq_lut  = base + 0x100, 4-byte rows, up to 40 rows
 *       row: LUT_L_VAL = bits 7:0  -> frequency = L_VAL * 19.2 MHz (XO)
 *   - reg_perf_state = base + 0x320 : write the LUT row INDEX to request it.
 *     Pure MMIO, no SCM call, enable bit already set by bootloader.
 *   - Voltage rails are applied by AOP automatically per LUT row.
 *   - LMH thermal limiting remains active in hardware (EPSS-integrated, AOP
 *     managed) even though the OS is unaware.
 *
 * Behavior:
 *   DriverEntry maps both domains, scans the LUT, requests the highest valid
 *   row in each domain, logs the table. Mapping is kept for the driver
 *   lifetime; the driver has no unload path (remove via reboot).
 *
 *   There is nobody else writing PERF_STATE on this system (no PEP, no CPPC),
 *   so the setting persists until reboot.
 *
 * Target: ARM64, WDM legacy (non-PnP), Windows 10/11 (testsigning)
 */

#include <ntddk.h>

#define EPSS_DOMAIN_COUNT   2
#define EPSS_D0_PHYS        0x0000000185910000ULL
#define EPSS_D1_PHYS        0x0000000185920000ULL
#define EPSS_MAP_SIZE       0x1000

#define REG_FREQ_LUT        0x100
#define REG_PERF_STATE      0x320
#define REG_DOMAIN_STATE    0x020

#define LUT_MAX_ROWS        40
#define LUT_L_VAL_MASK      0x000000FFu
#define XO_HZ               19200000ULL

typedef struct _EPSS_DOMAIN {
    ULONGLONG   PhysBase;
    PVOID       MappedBase;
    ULONG       MaxIndex;
    ULONG       MaxFreqKHz;
    ULONG       Rows;
} EPSS_DOMAIN, *PEPSS_DOMAIN;

static EPSS_DOMAIN g_Domains[EPSS_DOMAIN_COUNT] = {
    { EPSS_D0_PHYS, NULL, 0, 0, 0 },   /* CPU0-3 big cluster  */
    { EPSS_D1_PHYS, NULL, 0, 0, 0 }    /* CPU4-7 little cluster */
};

static ULONG
ReadReg32(
    _In_ PVOID MappedBase,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)MappedBase + Offset));
}

static VOID
WriteReg32(
    _In_ PVOID MappedBase,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)MappedBase + Offset), Value);
}

static VOID
SetDomainMaxFrequency(
    _Inout_ PEPSS_DOMAIN Domain
    )
{
    ULONG   i;
    ULONG   row;
    ULONG   lval;
    ULONG   bestIdx = 0;
    ULONG   bestLval = 0;
    ULONG   freqKHz;

    /* Scan the LUT: find the row with the highest L_VAL. Rows are written by
     * firmware; a row of 0 (or all-zero tail) is invalid. Clamp to 39. */
    for (i = 0; i < LUT_MAX_ROWS; i++) {
        row = ReadReg32(Domain->MappedBase, REG_FREQ_LUT + i * 4);
        lval = row & LUT_L_VAL_MASK;
        if (lval == 0) {
            continue;
        }
        if (lval >= bestLval) {
            bestLval = lval;
            bestIdx = i;
            Domain->Rows = i + 1;
        }
    }

    if (bestLval == 0) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "aliothcpufreq: domain 0x%llX: LUT empty, skipping\n",
            Domain->PhysBase));
        return;
    }

    Domain->MaxIndex = bestIdx;
    Domain->MaxFreqKHz = (ULONG)((bestLval * XO_HZ) / 1000);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "aliothcpufreq: domain 0x%llX: %u LUT rows, max row=%u L_VAL=%u -> %u kHz\n",
        Domain->PhysBase, Domain->Rows, bestIdx, bestLval, Domain->MaxFreqKHz));

    /* Request the highest row. Hardware (with AOP applying voltage) does the rest. */
    WriteReg32(Domain->MappedBase, REG_PERF_STATE, bestIdx);

    /* Read back what the domain reports. */
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "aliothcpufreq: domain 0x%llX: perf_state now %u (idx), domain_state 0x%08X\n",
        Domain->PhysBase,
        ReadReg32(Domain->MappedBase, REG_PERF_STATE),
        ReadReg32(Domain->MappedBase, REG_DOMAIN_STATE)));
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT     DriverObject,
    _In_ PUNICODE_STRING    RegistryPath
    )
{
    ULONG   i;
    PHYSICAL_ADDRESS pa;

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "aliothcpufreq: init - raising EPSS domains to max LUT row\n"));

    for (i = 0; i < EPSS_DOMAIN_COUNT; i++) {
        pa.QuadPart = (LONGLONG)g_Domains[i].PhysBase;

        g_Domains[i].MappedBase = MmMapIoSpaceEx(
            pa,
            EPSS_MAP_SIZE,
            PAGE_READWRITE | PAGE_NOCACHE);

        if (g_Domains[i].MappedBase == NULL) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "aliothcpufreq: MmMapIoSpaceEx failed for 0x%llX\n",
                g_Domains[i].PhysBase));
            continue;
        }

        SetDomainMaxFrequency(&g_Domains[i]);
    }

    /* Keep mappings for driver lifetime; no unload path (reboot to revert). */
    return STATUS_SUCCESS;
}
