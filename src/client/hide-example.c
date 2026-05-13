/*
 * hide-example.c -- Minimal client demonstrating hide / show.
 *
 * Build:
 *   cl /nologo /W3 hide-example.c /link Setupapi.lib Advapi32.lib /out:hide.exe
 *
 * Usage:
 *   hide.exe list                     -- enumerate all filter instances
 *   hide.exe hide  <instance-index>   -- hide that controller
 *   hide.exe show  <instance-index>   -- make it visible again
 *
 * NOTE: An instance index here is just "Nth result of SetupDiEnumDeviceInterfaces"
 * for our interface GUID. In a real remapper you map physical-device identity
 * (VID/PID/instance ID) to a filter instance once at startup by inspecting the
 * parent of each filter interface.
 */

#include <Windows.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>

#define INITGUID
#include "../driver/HideFilter.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* Open the Nth instance of GUID_DEVINTERFACE_HIDEFILTER. */
static HANDLE OpenFilterInstance(DWORD index, WCHAR* parent_id, DWORD parent_cch)
{
    HDEVINFO hdi = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HIDEFILTER, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE h = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };

    if (SetupDiEnumDeviceInterfaces(hdi, NULL, &GUID_DEVINTERFACE_HIDEFILTER, index, &ifd)) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, NULL, 0, &req, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* d =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(req);
        if (d) {
            d->cbSize = sizeof(*d);
            SP_DEVINFO_DATA dev = { sizeof(dev) };
            if (SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, d, req, &req, &dev)) {

                if (parent_id && parent_cch) {
                    DEVINST parent = 0;
                    if (CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS) {
                        ULONG len = parent_cch;
                        CM_Get_Device_IDW(parent, parent_id, len, 0);
                    } else {
                        parent_id[0] = 0;
                    }
                }

                h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
            }
            free(d);
        }
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return h;
}

static int CmdList(void)
{
    HDEVINFO hdi = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HIDEFILTER, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) {
        wprintf(L"No filter instances found.\n");
        return 0;
    }
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hdi, NULL,
            &GUID_DEVINTERFACE_HIDEFILTER, i, &ifd); ++i) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, NULL, 0, &req, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* d =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(req);
        if (!d) break;
        d->cbSize = sizeof(*d);
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (SetupDiGetDeviceInterfaceDetailW(hdi, &ifd, d, req, &req, &dev)) {
            WCHAR parent_id[512] = {0};
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS) {
                ULONG cch = _countof(parent_id);
                CM_Get_Device_IDW(parent, parent_id, cch, 0);
            }
            wprintf(L"[%lu]\n  path  : %ls\n  parent: %ls\n\n",
                    i, d->DevicePath, parent_id);
        }
        free(d);
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return 0;
}

static BOOL SetHidden(HANDLE h, BOOL hide)
{
    UCHAR in = hide ? 0 : 1;      /* 0 hide, !=0 show */
    DWORD ret = 0;
    return DeviceIoControl(h, IOCTL_HIDEFILTER_TOGGLE_PRESENCE,
                           &in, sizeof(in), NULL, 0, &ret, NULL);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage:\n"
                L"  %ls list\n"
                L"  %ls hide  <instance-index>\n"
                L"  %ls show  <instance-index>\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    if (_wcsicmp(argv[1], L"list") == 0) return CmdList();

    if (argc < 3) {
        wprintf(L"error: need instance index\n");
        return 1;
    }
    DWORD idx = (DWORD)_wtoi(argv[2]);
    BOOL  hide = (_wcsicmp(argv[1], L"hide") == 0);

    WCHAR parent[512];
    HANDLE h = OpenFilterInstance(idx, parent, _countof(parent));
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"OpenFilterInstance(%lu) failed: %lu\n", idx, GetLastError());
        return 2;
    }
    wprintf(L"%ls parent: %ls ... ", hide ? L"hiding" : L"showing", parent);
    BOOL ok = SetHidden(h, hide);
    CloseHandle(h);
    wprintf(L"%ls\n", ok ? L"OK" : L"FAILED");
    return ok ? 0 : 3;
}
