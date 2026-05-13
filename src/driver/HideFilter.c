/*
 * HideFilter.c -- KMDF lower-filter driver entry point and IOCTL dispatcher.
 *
 * Builds against the standard KMDF template. This file deliberately does not
 * include any Hardware-IDs or class match logic -- the driver is attached by
 * way of LowerFilters injection performed at install time by the user-mode
 * service (see ../service/).
 */

#include <ntddk.h>
#include <wdf.h>
#include "HideFilter.h"

/* ----- HidNotifyPresence resolution (see HidClassResolver.c) -------------- */

typedef VOID (NTAPI *PFN_HID_NOTIFY_PRESENCE)(PDEVICE_OBJECT Pdo, BOOLEAN IsPresent);
extern PFN_HID_NOTIFY_PRESENCE g_HidNotifyPresence;
NTSTATUS HfResolveHidNotifyPresence(VOID);

/* ----- Per-device context ------------------------------------------------- */

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE       Self;
    PDEVICE_OBJECT  PhysicalPdo;     /* the controller's PDO */
    volatile LONG   HideRefCount;
    BOOLEAN         Hidden;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, HfGetDeviceContext)

/* ----- Forward decls ------------------------------------------------------ */

DRIVER_INITIALIZE              DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD      EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDeviceContextCleanup;

/* ----- Implementation ----------------------------------------------------- */

NTSTATUS
EvtPrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  RawResources,
    _In_ WDFCMRESLIST  TranslatedResources)
{
    UNREFERENCED_PARAMETER(RawResources);
    UNREFERENCED_PARAMETER(TranslatedResources);

    PDEVICE_CONTEXT ctx = HfGetDeviceContext(Device);
    /*
     * WdfDeviceWdmGetPhysicalDevice() returns the PDO at the bottom of the
     * stack. This is exactly what HidNotifyPresence() expects.
     */
    ctx->PhysicalPdo = WdfDeviceWdmGetPhysicalDevice(Device);
    return STATUS_SUCCESS;
}

VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    if (IoControlCode != IOCTL_HIDEFILTER_TOGGLE_PRESENCE) goto done;
    if (InputBufferLength < 1) { status = STATUS_BUFFER_TOO_SMALL; goto done; }
    if (!g_HidNotifyPresence)  { status = STATUS_NOT_SUPPORTED;    goto done; }

    UCHAR *in = NULL;
    status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) goto done;

    BOOLEAN hide = (*in == 0);

    WDFDEVICE       dev = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = HfGetDeviceContext(dev);

    if (hide) {
        if (InterlockedIncrement(&ctx->HideRefCount) == 1) {
            g_HidNotifyPresence(ctx->PhysicalPdo, FALSE);
            ctx->Hidden = TRUE;
        }
    } else {
        if (ctx->HideRefCount > 0) {
            if (InterlockedDecrement(&ctx->HideRefCount) == 0 && ctx->Hidden) {
                g_HidNotifyPresence(ctx->PhysicalPdo, TRUE);
                ctx->Hidden = FALSE;
            }
        }
    }
    status = STATUS_SUCCESS;

done:
    WdfRequestComplete(Request, status);
}

VOID
EvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    /*
     * Device being removed (cable unplug, driver unload, etc.). If we are
     * still holding hide refcount, force a show first so the user does not
     * find his pad mysteriously invisible the next time he plugs it in.
     */
    PDEVICE_CONTEXT ctx = HfGetDeviceContext((WDFDEVICE)Object);
    if (ctx->Hidden && g_HidNotifyPresence && ctx->PhysicalPdo) {
        g_HidNotifyPresence(ctx->PhysicalPdo, TRUE);
        ctx->Hidden = FALSE;
        ctx->HideRefCount = 0;
    }
}

NTSTATUS
EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    /*
     * Mark this device object as a filter. Without this, KMDF tries to act
     * as the function driver, fails to handle PnP for the actual function,
     * and the device stack breaks.
     */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    attr.EvtCleanupCallback = EvtDeviceContextCleanup;

    WDFDEVICE  device = NULL;
    NTSTATUS   status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    PDEVICE_CONTEXT ctx = HfGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Self = device;

    /* Default-queue receives our IOCTL. */
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = EvtIoDeviceControl;

    WDFQUEUE queue;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    /*
     * Expose a device interface so user mode can SetupDiGetClassDevs() us and
     * obtain a handle to this specific filter instance (= one per filtered
     * physical device).
     */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_HIDEFILTER, NULL);
    return status;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    /*
     * Best-effort resolve HidNotifyPresence. If it fails, the driver still
     * loads -- we just return STATUS_NOT_SUPPORTED to any hide attempt.
     */
    HfResolveHidNotifyPresence();

    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);

    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
