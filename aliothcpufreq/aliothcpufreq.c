/*
 * aliothcpufreq.c - EPSS probe / max-frequency driver for SM8250 (alioth)
 *
 * SAFE-BY-DEFAULT DESIGN:
 *   Mode = 0  (default) : READ ONLY. Maps both EPSS domains, dumps the LUT and
 *                         the current PERF_STATE, writes the result into the
 *                         service Parameters key as REG_SZ "Report".
 *                         Nothing is written to hardware.
 *   Mode = 1            : read, then write the highest LUT index into
 *                         PERF_STATE for both domains, then read back and
 *                         report again.
 *
 * Mode is read from:
 *   \Registry\Machine\System\CurrentControlSet\Services\aliothcpufreq\Parameters
 *     Mode   REG_DWORD   0 = read only (default), 1 = write max index
 *
 * Register model (matches Linux drivers/cpufreq/qcom-cpufreq-hw.c, EPSS):
 *   Domain 0 (CPU0-3, Cortex-A77) base 0x185910000
 *   Domain 1 (CPU4-7, Cortex-A55) base 0x185920000
 *   reg_freq_lut   = base + 0x100   4-byte rows, L_VAL = bits 7:0
 *   reg_perf_state = base + 0x320   write the LUT row INDEX
 *   reg_domain_state = base + 0x020
 *   frequency = L_VAL * 19.2 MHz (XO)
 *
 * Load/unload: legacy WDM driver, DriverEntry + DriverUnload so it can be
 * stopped with "sc stop" and re-started with a different Mode without a reboot.
 *
 * Target: ARM64, Windows 10/11 (testsigning)
 */

#include <ntddk.h>

#define EPSS_DOMAIN_COUNT   2
#define EPSS_D0_PHYS        0x0000000185910000ULL
#define EPSS_D1_PHYS        0x0000000185920000ULL
#define EPSS_MAP_SIZE       0x1000

#define REG_DOMAIN_STATE    0x020
#define REG_FREQ_LUT        0x100
#define REG_PERF_STATE      0x320

#define LUT_MAX_ROWS        40
#define LUT_L_VAL_MASK      0x000000FFu
#define XO_HZ               19200000ULL

#define PARAM_KEY_PATH  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\aliothcpufreq\\Parameters"

typedef struct _EPSS_DOMAIN {
    ULONGLONG   PhysBase;
    PVOID       MappedBase;
    ULONG       ValidRows;
    ULONG       MaxIndex;
    ULONG       MaxLval;
    ULONG       MaxMHz;
    ULONG       PerfState;
    ULONG       DomainState;
    ULONG       Lut[8];
} EPSS_DOMAIN, *PEPSS_DOMAIN;

static EPSS_DOMAIN g_Domains[EPSS_DOMAIN_COUNT] = {
    { EPSS_D0_PHYS, NULL, 0, 0, 0, 0, 0, 0, {0} },
    { EPSS_D1_PHYS, NULL, 0, 0, 0, 0, 0, 0, {0} }
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

static ULONG
QueryMode(VOID)
{
    ULONG mode = 0;
    ULONG defMode = 0;
    RTL_QUERY_REGISTRY_TABLE tab[2];

    RtlZeroMemory(tab, sizeof(tab));
    tab[0].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    tab[0].Name         = L"Mode";
    tab[0].EntryContext = &mode;
    tab[0].DefaultType  = REG_DWORD;
    tab[0].DefaultData  = &defMode;
    tab[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                           PARAM_KEY_PATH,
                                           tab, NULL, NULL))) {
        return 0;
    }
    return mode;
}

static VOID
ScanDomain(
    _Inout_ PEPSS_DOMAIN Domain
    )
{
    ULONG i;
    ULONG row;
    ULONG lval;

    Domain->ValidRows = 0;
    Domain->MaxIndex  = 0;
    Domain->MaxLval   = 0;

    for (i = 0; i < LUT_MAX_ROWS; i++) {
        row  = ReadReg32(Domain->MappedBase, REG_FREQ_LUT + i * 4);
        lval = row & LUT_L_VAL_MASK;
        if (i < 8) {
            Domain->Lut[i] = row;
        }
        if (lval == 0) {
            continue;
        }
        Domain->ValidRows++;
        if (lval >= Domain->MaxLval) {
            Domain->MaxLval  = lval;
            Domain->MaxIndex = i;
        }
    }

    Domain->MaxMHz      = (ULONG)(((ULONGLONG)Domain->MaxLval * XO_HZ) / 1000000ULL);
    Domain->PerfState   = ReadReg32(Domain->MappedBase, REG_PERF_STATE);
    Domain->DomainState = ReadReg32(Domain->MappedBase, REG_DOMAIN_STATE);
}

static VOID
WriteReport(
    _In_ ULONG Mode
    )
{
    CHAR        buf[1024];
    NTSTATUS    st;
    ULONG       i;
    size_t      len = 0;

    RtlStringCbPrintfA(buf, sizeof(buf),
                       "mode=%u\r\n", Mode);

    for (i = 0; i < EPSS_DOMAIN_COUNT; i++) {
        PEPSS_DOMAIN d = &g_Domains[i];
        CHAR tmp[320];

        if (d->MappedBase == NULL) {
            RtlStringCbPrintfA(tmp, sizeof(tmp),
                               "d%u: base=0x%llX MAP_FAILED\r\n", i, d->PhysBase);
        } else {
            RtlStringCbPrintfA(tmp, sizeof(tmp),
                               "d%u: base=0x%llX valid=%u maxIdx=%u maxLval=%u maxMHz=%u perf=%u domstate=0x%08X\r\n",
                               i, d->PhysBase, d->ValidRows, d->MaxIndex,
                               d->MaxLval, d->MaxMHz, d->PerfState, d->DomainState);
        }
        RtlStringCbCatA(buf, sizeof(buf), tmp);

        if (d->MappedBase != NULL) {
            RtlStringCbPrintfA(tmp, sizeof(tmp),
                               "d%u: lut=%08X %08X %08X %08X %08X %08X %08X %08X\r\n",
                               i, d->Lut[0], d->Lut[1], d->Lut[2], d->Lut[3],
                               d->Lut[4], d->Lut[5], d->Lut[6], d->Lut[7]);
            RtlStringCbCatA(buf, sizeof(buf), tmp);
        }
    }

    if (NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &len))) {
        st = RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE,
                                   PARAM_KEY_PATH,
                                   L"Report",
                                   REG_SZ,
                                   buf,
                                   (ULONG)len + 1);
        if (!NT_SUCCESS(st)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "aliothcpufreq: report write failed 0x%08X\n", st));
        }
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "aliothcpufreq: report -> %s\n", buf));
}

static VOID
DriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    ULONG i;

    UNREFERENCED_PARAMETER(DriverObject);

    for (i = 0; i < EPSS_DOMAIN_COUNT; i++) {
        if (g_Domains[i].MappedBase != NULL) {
            MmUnmapIoSpace(g_Domains[i].MappedBase, EPSS_MAP_SIZE);
            g_Domains[i].MappedBase = NULL;
        }
    }
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "aliothcpufreq: unloaded\n"));
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT     DriverObject,
    _In_ PUNICODE_STRING    RegistryPath
    )
{
    ULONG   i;
    ULONG   mode;
    PHYSICAL_ADDRESS pa;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;

    mode = QueryMode();
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "aliothcpufreq: init mode=%u (0=read only, 1=write max)\n", mode));

    for (i = 0; i < EPSS_DOMAIN_COUNT; i++) {
        pa.QuadPart = (LONGLONG)g_Domains[i].PhysBase;

        g_Domains[i].MappedBase = MmMapIoSpaceEx(pa, EPSS_MAP_SIZE,
                                                 PAGE_READWRITE | PAGE_NOCACHE);
        if (g_Domains[i].MappedBase == NULL) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "aliothcpufreq: MmMapIoSpaceEx failed 0x%llX\n",
                       g_Domains[i].PhysBase));
            continue;
        }

        ScanDomain(&g_Domains[i]);
    }

    WriteReport(mode);

    if (mode == 1) {
        for (i = 0; i < EPSS_DOMAIN_COUNT; i++) {
            PEPSS_DOMAIN d = &g_Domains[i];
            if (d->MappedBase == NULL || d->MaxLval == 0) {
                continue;
            }
            WriteReg32(d->MappedBase, REG_PERF_STATE, d->MaxIndex);
            d->PerfState   = ReadReg32(d->MappedBase, REG_PERF_STATE);
            d->DomainState = ReadReg32(d->MappedBase, REG_DOMAIN_STATE);
        }
        WriteReport(1);
    }

    return STATUS_SUCCESS;
}
