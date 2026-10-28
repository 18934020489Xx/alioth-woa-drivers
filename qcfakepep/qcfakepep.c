/*
 * qcfakepep.c - Stub PEP (Platform Power Engine) driver for Qualcomm alioth (SM8250)
 * Pure WDM - no framework dependencies.
 *
 * Why this exists:
 *   Downstream Qualcomm drivers (qcsmmu8280, qcsubsys8280, gpu, i2c/spi/uart,
 *   camera, scm ...) resolve the PEP0 ACPI device for two interfaces:
 *     1. Device interface  {55beca03-ba2c-4fbc-9efc-ee8eca56f0c7} (arrival/open)
 *     2. QueryInterface    {a0e1dd36-dfb9-4c0e-bbaa-78f0746be181}
 *                            (size 0x28, version 1, one function)
 *   Without them every downstream AddDevice fails (Code 31 / 0xC0000001).
 *
 *   Neither official qcpep build survives AddDevice on SM8250 (kd-verified:
 *   qcpep8250 writes SM8150 LMH registers, qcpep8280 writes SC8280XP-only
 *   TSENS controllers - both BugCheck 0x101).
 *
 *   qcpep8280's own vendor "stubbed PEP for presilicon" mode implements the
 *   {a0e1dd36} function as: unconditional STATUS_SUCCESS. This driver
 *   reproduces exactly that semantics with zero MMIO and zero power
 *   management. CPU frequency is deliberately not handled here (see the
 *   aliothcpufreq driver).
 *
 * Design note:
 *   The interface structure lives in static storage and outlives the device
 *   object, so InterfaceReference/Dereference can be no-ops safely.
 *
 * Target: ARM64, Windows 10/11 (testsigning), WDM
 */

#include <ntddk.h>
#include <initguid.h>
#include <wdm.h>

#pragma warning(disable:4201)   // nameless struct/union in wdm headers

/* {55beca03-ba2c-4fbc-9efc-ee8eca56f0c7} - SOC/PEP device interface class */
DEFINE_GUID(GUID_QCSOC_INTERFACE,
    0x55beca03, 0xba2c, 0x4fbc, 0x9e, 0xfc, 0xee, 0x8e, 0xca, 0x56, 0xf0, 0xc7);

/* {a0e1dd36-dfb9-4c0e-bbaa-78f0746be181} - PEP power-request query interface */
DEFINE_GUID(GUID_QCPEP_QUERY,
    0xa0e1dd36, 0xdfb9, 0x4c0e, 0xbb, 0xaa, 0x78, 0xf0, 0x74, 0x6b, 0xe1, 0x81);

typedef
NTSTATUS
(*PFN_QCPEP_POWER_REQUEST)(
    _In_ PVOID Context
    );

typedef struct _QC_PEP_INTERFACE {
    USHORT                  Size;               /* 0x28 */
    USHORT                  Version;            /* 1    */
    PVOID                   Context;
    PINTERFACE_REFERENCE    InterfaceReference;
    PINTERFACE_DEREFERENCE  InterfaceDereference;
    PFN_QCPEP_POWER_REQUEST PowerRequest;
} QC_PEP_INTERFACE, *PQC_PEP_INTERFACE;

C_ASSERT(sizeof(QC_PEP_INTERFACE) == 0x28);

#define QCPEP_TAG 'pefQ'

typedef struct _QCPEP_DEVICE_EXTENSION {
    PDEVICE_OBJECT  LowerDevice;
    PDEVICE_OBJECT  Pdo;
    UNICODE_STRING  SymLink;
} QCPEP_DEVICE_EXTENSION, *PQCPEP_DEVICE_EXTENSION;

/* ------------------------------------------------------------------ */
/* Stub callbacks (static lifetime - no-ops are safe)                  */
/* ------------------------------------------------------------------ */

static NTSTATUS
QcPepPowerRequestStub(
    _In_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
    /* Vendor presilicon-stub semantics: unconditional success, any IRQL. */
    return STATUS_SUCCESS;
}

static VOID
QcPepInterfaceReferenceStub(
    _In_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID
QcPepInterfaceDereferenceStub(
    _In_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

static const QC_PEP_INTERFACE g_PepInterfaceTemplate = {
    sizeof(QC_PEP_INTERFACE),   /* Size    = 0x28 */
    1,                          /* Version */
    NULL,                       /* Context */
    QcPepInterfaceReferenceStub,
    QcPepInterfaceDereferenceStub,
    QcPepPowerRequestStub
};

/* ------------------------------------------------------------------ */
/* Generic pass-through dispatch                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
QcPepDispatchPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PQCPEP_DEVICE_EXTENSION ext;

    ASSERT(DeviceObject != NULL);
    ext = (PQCPEP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

static NTSTATUS
QcPepDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* PnP dispatch                                                        */
/* ------------------------------------------------------------------ */

static NTSTATUS
QcPepDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PQCPEP_DEVICE_EXTENSION ext;
    PIO_STACK_LOCATION      sl;
    NTSTATUS                status;

    ext = (PQCPEP_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    sl  = IoGetCurrentIrpStackLocation(Irp);

    switch (sl->MinorFunction) {
    case IRP_MN_QUERY_INTERFACE:
        if (sl->Parameters.QueryInterface.Interface != NULL &&
            RtlEqualMemory(sl->Parameters.QueryInterface.InterfaceType,
                           &GUID_QCPEP_QUERY, sizeof(GUID)) &&
            sl->Parameters.QueryInterface.Version == 1 &&
            sl->Parameters.QueryInterface.Size >= sizeof(QC_PEP_INTERFACE)) {

            PQC_PEP_INTERFACE out =
                (PQC_PEP_INTERFACE)sl->Parameters.QueryInterface.Interface;

            out->Size                 = sizeof(QC_PEP_INTERFACE);
            out->Version              = 1;
            out->Context              = NULL;
            out->InterfaceReference   = QcPepInterfaceReferenceStub;
            out->InterfaceDereference = QcPepInterfaceDereferenceStub;
            out->PowerRequest         = QcPepPowerRequestStub;

            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "qcfakepep: served {a0e1dd36} v1\n"));

            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        /* fall through: unknown interface - pass down */
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);

    case IRP_MN_START_DEVICE:
        /* Let the PDO start first, then enable our interface. */
        status = QcPepDispatchPassThrough(DeviceObject, Irp);
        return status;

    case IRP_MN_REMOVE_DEVICE:
        if (ext->SymLink.Buffer != NULL) {
            IoSetDeviceInterfaceState(&ext->SymLink, FALSE);
            RtlFreeUnicodeString(&ext->SymLink);
            RtlZeroMemory(&ext->SymLink, sizeof(UNICODE_STRING));
        }
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoDetachDevice(ext->LowerDevice);
        IoDeleteDevice(DeviceObject);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "qcfakepep: device removed\n"));
        return status;

    default:
        return QcPepDispatchPassThrough(DeviceObject, Irp);
    }
}

/* ------------------------------------------------------------------ */
/* AddDevice                                                           */
/* ------------------------------------------------------------------ */

static NTSTATUS
QcPepAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT              fdo = NULL;
    PQCPEP_DEVICE_EXTENSION     ext = NULL;
    UNICODE_STRING              devName;
    NTSTATUS                    status;

    RtlInitUnicodeString(&devName, L"\\Device\\QcFakePep0");

    status = IoCreateDevice(
        DriverObject,
        sizeof(QCPEP_DEVICE_EXTENSION),
        &devName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &fdo);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    ext = (PQCPEP_DEVICE_EXTENSION)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(QCPEP_DEVICE_EXTENSION));

    ext->Pdo = PhysicalDeviceObject;
    ext->LowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (ext->LowerDevice == NULL) {
        status = STATUS_NO_SUCH_DEVICE;
        goto Fail;
    }

    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Publish {55beca03}: creates the symlink that IoGetDeviceInterfaces
     * enumerates and fires arrival notifications for listeners. */
    status = IoRegisterDeviceInterface(
        PhysicalDeviceObject,
        &GUID_QCSOC_INTERFACE,
        NULL,
        &ext->SymLink);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    status = IoSetDeviceInterfaceState(&ext->SymLink, TRUE);
    if (!NT_SUCCESS(status)) {
        RtlFreeUnicodeString(&ext->SymLink);
        RtlZeroMemory(&ext->SymLink, sizeof(UNICODE_STRING));
        goto Fail;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "qcfakepep: AddDevice OK. {55beca03} published.\n"));

    return STATUS_SUCCESS;

Fail:
    if (fdo != NULL) {
        if (ext != NULL && ext->LowerDevice != NULL) {
            IoDetachDevice(ext->LowerDevice);
        }
        IoDeleteDevice(fdo);
    }
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "qcfakepep: AddDevice failed 0x%08X\n", status));
    return status;
}

/* ------------------------------------------------------------------ */
/* EPSS max-frequency (merged from aliothcpufreq)                      */
/*                                                                     */
/* SM8250 frequency control is plain MMIO (Linux qcom-cpufreq-hw.c,    */
/* EPSS variant): write the max LUT row index to base+0x320.           */
/*   Domain 0 (CPU0-3, A77): 0x18591000                                */
/*   Domain 1 (CPU4-7, A55): 0x18592000                                */
/* No SCM call; enable bit set by firmware; voltage applied by AOP;    */
/* LMH thermal limiting stays active in hardware.                      */
/* Nobody else writes PERF_STATE on this system (no PEP, no CPPC), so  */
/* the setting persists until reboot.                                  */
/* ------------------------------------------------------------------ */

#define EPSS_DOMAIN_COUNT   2
#define EPSS_D0_PHYS        0x0000000185910000ULL
#define EPSS_D1_PHYS        0x0000000185920000ULL
#define EPSS_MAP_SIZE       0x1000
#define EPSS_REG_FREQ_LUT   0x100
#define EPSS_REG_PERF_STATE 0x320
#define EPSS_REG_DOMAIN_ST  0x020
#define EPSS_LUT_MAX_ROWS   40
#define EPSS_LUT_LVAL_MASK  0x000000FFu
#define EPSS_XO_HZ          19200000ULL

static VOID
QcPepRaiseEpssFrequencies(VOID)
{
    static const ULONGLONG bases[EPSS_DOMAIN_COUNT] = { EPSS_D0_PHYS, EPSS_D1_PHYS };
    ULONG d;

    for (d = 0; d < EPSS_DOMAIN_COUNT; d++) {
        PHYSICAL_ADDRESS pa;
        PVOID mapped;
        ULONG i, row, lval, bestIdx = 0, bestLval = 0;

        pa.QuadPart = (LONGLONG)bases[d];
        mapped = MmMapIoSpaceEx(pa, EPSS_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (mapped == NULL) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "qcfakepep: EPSS 0x%llX map failed\n", bases[d]));
            continue;
        }

        for (i = 0; i < EPSS_LUT_MAX_ROWS; i++) {
            row  = READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapped + EPSS_REG_FREQ_LUT + i * 4));
            lval = row & EPSS_LUT_LVAL_MASK;
            if (lval != 0 && lval >= bestLval) {
                bestLval = lval;
                bestIdx  = i;
            }
        }

        if (bestLval != 0) {
            WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapped + EPSS_REG_PERF_STATE), bestIdx);
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "qcfakepep: EPSS 0x%llX: max row=%u L_VAL=%u -> %u kHz (perf_state now %u, domain_state 0x%08X)\n",
                bases[d], bestIdx, bestLval,
                (ULONG)((bestLval * EPSS_XO_HZ) / 1000),
                READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapped + EPSS_REG_PERF_STATE)),
                READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapped + EPSS_REG_DOMAIN_ST))));
        } else {
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "qcfakepep: EPSS 0x%llX: LUT empty, skipped\n", bases[d]));
        }
        /* Keep the mapping for the driver lifetime (never unloaded). */
    }
}

/* ------------------------------------------------------------------ */
/* Driver entry                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT     DriverObject,
    _In_ PUNICODE_STRING    RegistryPath
    )
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Raise both EPSS domains to their max LUT row before anything else.
     * Runs once per boot; no other component manages frequency here. */
    QcPepRaiseEpssFrequencies();

    DriverObject->DriverExtension->AddDevice = QcPepAddDevice;
    DriverObject->DriverUnload = NULL;   /* reboot to unload */

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = QcPepDispatchPassThrough;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcPepDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]  = QcPepDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_PNP]    = QcPepDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]  = QcPepDispatchPassThrough;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "qcfakepep: DriverEntry OK (WDM stub PEP)\n"));

    return STATUS_SUCCESS;
}
