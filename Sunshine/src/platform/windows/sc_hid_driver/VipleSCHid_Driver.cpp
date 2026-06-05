/*
 * §SC-HID: VipleSCHid_Driver.cpp — UMDF2 HID minidriver
 *
 * Presents a virtual USB HID device with:
 *   VendorID  = 0x28DE  (Valve)
 *   ProductID = 0x1102  (Steam Controller wired)
 *
 * Hosted by the in-box Microsoft-signed mshidumdf.sys reflector, so no
 * kernel driver signing is required for the DLL.
 *
 * Build: MSVC + WDK + UMDF headers.  Link: OneCoreUAP.lib + WdfCore.lib.
 * Install: pnputil /add-driver VipleSCHid.inf /install
 *          devcon install VipleSCHid.inf Viple\SteamController
 */

#define UMDF_USING_NTSTATUS
#include <ntstatus.h>

#include <windows.h>
#include <wdf.h>
#include <wudfwdm.h>
#include <hidport.h>
#include <hidpddi.h>

// Steam Controller wired USB identifiers
#define SC_VID 0x28DE
#define SC_PID 0x1102

// ---- Steam Controller HID report descriptor (minimal, input-only) --------
// This matches the vendor-defined 64-byte report on Interface 2 of the
// wired Steam Controller.  Buttons/axes are parsed by Steam Input on the
// host; we do not need to expose them as standard Usage items.
static const uint8_t g_HidReportDescriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor-Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 1)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x40,        //   Report Count (64)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    // Feature report (same shape) — used for lizard-mode control / haptics
    0x09, 0x03,        //   Usage (Vendor Usage 3)
    0xB1, 0x02,        //   Feature (Data, Var, Abs)
    0xC0               // End Collection
};

// ---- HID_DEVICE_ATTRIBUTES -------------------------------------------------
static HID_DEVICE_ATTRIBUTES g_Attributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    SC_VID,  // VendorID
    SC_PID,  // ProductID
    0x0100   // VersionNumber
};

// ---- WDF/UMDF callbacks ----------------------------------------------------
NTSTATUS DriverEntry(WDFDRIVER Driver, PWDFDRIVER_CONFIG Config);
EVT_WDF_DRIVER_DEVICE_ADD  EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

NTSTATUS DriverEntry(WDFDRIVER Driver, PWDFDRIVER_CONFIG Config) {
    UNREFERENCED_PARAMETER(Driver);
    WDF_DRIVER_CONFIG_INIT(Config, EvtDeviceAdd);
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) return status;

    // Create default queue to handle internal IOCTL (HID class driver sends these)
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    WDFQUEUE queue;
    return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
}

VOID EvtIoInternalDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                                  size_t OutputBufferLength, size_t InputBufferLength,
                                  ULONG IoControlCode) {
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    WDFMEMORY mem = nullptr;
    size_t transferred = 0;

    switch (IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR desc = {};
        desc.bLength          = sizeof(HID_DESCRIPTOR);
        desc.bDescriptorType  = HID_HID_DESCRIPTOR_TYPE;
        desc.bcdHID           = 0x0101;
        desc.bCountry         = 0;
        desc.bNumDescriptors  = 1;
        desc.DescriptorList[0].bReportType   = HID_REPORT_DESCRIPTOR_TYPE;
        desc.DescriptorList[0].wReportLength = sizeof(g_HidReportDescriptor);

        WdfRequestRetrieveOutputMemory(Request, &mem);
        if (mem) {
            WdfMemoryCopyFromBuffer(mem, 0, &desc, sizeof(desc));
            transferred = sizeof(desc);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        WdfRequestRetrieveOutputMemory(Request, &mem);
        if (mem) {
            WdfMemoryCopyFromBuffer(mem, 0,
                const_cast<uint8_t*>(g_HidReportDescriptor),
                sizeof(g_HidReportDescriptor));
            transferred = sizeof(g_HidReportDescriptor);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        WdfRequestRetrieveOutputMemory(Request, &mem);
        if (mem) {
            WdfMemoryCopyFromBuffer(mem, 0, &g_Attributes, sizeof(g_Attributes));
            transferred = sizeof(g_Attributes);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_READ_REPORT:
        // Pend the request — Sunshine's injector (VipleSCHid.cpp) writes via
        // WriteFile which routes through the HID class driver back here.
        // For simplicity: just complete with empty for now; the real input
        // arrives via IOCTL_HID_WRITE_REPORT from the Sunshine injector.
        status = STATUS_PENDING;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (status != STATUS_PENDING) {
        WdfRequestCompleteWithInformation(Request, status, transferred);
    }
}
