/*
 * HideFilterSvc.c -- User-mode installer / LowerFilters writer for HideFilter.
 *
 *   build: cl /nologo /W3 /DHIDEFILTER_SVC_MAIN HideFilterSvc.c
 *              /link Setupapi.lib Newdev.lib Advapi32.lib Ole32.lib
 *              /out:HideFilterSvc.exe
 *
 *   run:   HideFilterSvc.exe install     (must be Administrator)
 *          HideFilterSvc.exe uninstall
 */

#include <Windows.h>
#include <SetupAPI.h>
#include <newdev.h>
#include <stdio.h>
#include <strsafe.h>
#include "HideFilterSvc.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

/* ---------------------------------------------------------------------------
 *  The five target device classes
 * --------------------------------------------------------------------------- */
static const WCHAR* g_TargetClassGuids[] = {
    L"{05f5cfe2-4733-4950-a6bb-07aad01a3a84}",  /* XboxComposite — Xbox One / Series / Wireless Adapter */
    L"{d61ca365-5af4-4486-998b-9db4734c6ca3}",  /* XnaComposite  — Xbox 360 / Wireless Receiver pads    */
    L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",  /* HIDClass      — DirectInput / RawInput visibility    */
    L"{4d36e96b-e325-11ce-bfc1-08002be10318}",  /* Keyboard                                              */
    L"{4d36e96f-e325-11ce-bfc1-08002be10318}"   /* Mouse                                                 */
};
#define NUM_TARGET_CLASSES (sizeof(g_TargetClassGuids)/sizeof(g_TargetClassGuids[0]))

/* ---------------------------------------------------------------------------
 *  REG_MULTI_SZ helpers
 * --------------------------------------------------------------------------- */
static BOOL MultiSzContains(const WCHAR* multi_sz, const WCHAR* value)
{
    if (!multi_sz) return FALSE;
    for (const WCHAR* p = multi_sz; *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, value) == 0) return TRUE;
    }
    return FALSE;
}

static DWORD MultiSzAppend(const WCHAR* base, DWORD base_size_bytes,
                           const WCHAR* value,
                           WCHAR** out_buf, DWORD* out_size_bytes)
{
    size_t value_chars = wcslen(value) + 1;

    DWORD effective_base = base_size_bytes;
    if (effective_base < 2 * sizeof(WCHAR)) effective_base = sizeof(WCHAR);

    DWORD new_size = effective_base + (DWORD)(value_chars * sizeof(WCHAR));
    WCHAR* buf = (WCHAR*)calloc(1, new_size + sizeof(WCHAR));
    if (!buf) return ERROR_OUTOFMEMORY;

    DWORD off = 0;
    if (base_size_bytes >= 2 * sizeof(WCHAR) && base) {
        /* copy everything up to but not including the final double-NUL */
        memcpy(buf, base, base_size_bytes - sizeof(WCHAR));
        off = base_size_bytes - sizeof(WCHAR);
    }
    memcpy((BYTE*)buf + off, value, value_chars * sizeof(WCHAR));
    /* trailing NUL guaranteed by calloc */
    *out_buf = buf;
    *out_size_bytes = (DWORD)(off + value_chars * sizeof(WCHAR) + sizeof(WCHAR));
    return ERROR_SUCCESS;
}

static DWORD MultiSzRemove(const WCHAR* base, DWORD base_size_bytes,
                           const WCHAR* value,
                           WCHAR** out_buf, DWORD* out_size_bytes)
{
    WCHAR* buf = (WCHAR*)calloc(1, base_size_bytes + 2 * sizeof(WCHAR));
    if (!buf) return ERROR_OUTOFMEMORY;
    WCHAR* w = buf;
    for (const WCHAR* p = base; *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, value) != 0) {
            size_t n = wcslen(p) + 1;
            memcpy(w, p, n * sizeof(WCHAR));
            w += n;
        }
    }
    *w = 0;
    *out_buf = buf;
    *out_size_bytes = (DWORD)((w - buf + 1) * sizeof(WCHAR));
    return ERROR_SUCCESS;
}

typedef enum { OP_ADD, OP_REMOVE } FilterOp;

static LONG ModifyClassLowerFilters(const WCHAR* class_guid,
                                    const WCHAR* svc, FilterOp op)
{
    WCHAR key_path[260];
    StringCchPrintfW(key_path, _countof(key_path),
                     L"System\\CurrentControlSet\\Control\\Class\\%s", class_guid);

    HKEY hKey = NULL;
    LSTATUS s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);
    if (s != ERROR_SUCCESS) return s;

    DWORD type = 0, size = 0;
    s = RegQueryValueExW(hKey, L"LowerFilters", NULL, &type, NULL, &size);

    WCHAR* current = NULL;
    DWORD current_size = 0;
    if (s == ERROR_SUCCESS && size >= sizeof(WCHAR) && type == REG_MULTI_SZ) {
        current = (WCHAR*)calloc(1, size + sizeof(WCHAR));
        if (!current) { RegCloseKey(hKey); return ERROR_OUTOFMEMORY; }
        s = RegQueryValueExW(hKey, L"LowerFilters", NULL, &type,
                             (BYTE*)current, &size);
        current_size = size;
    } else {
        current = (WCHAR*)calloc(2, sizeof(WCHAR));
        current_size = 2 * sizeof(WCHAR);
    }

    WCHAR* updated = NULL;
    DWORD  updated_size = 0;
    if (op == OP_ADD) {
        if (MultiSzContains(current, svc)) {
            free(current); RegCloseKey(hKey);
            return ERROR_SUCCESS;  /* nothing to do */
        }
        s = MultiSzAppend(current, current_size, svc, &updated, &updated_size);
    } else {
        s = MultiSzRemove(current, current_size, svc, &updated, &updated_size);
    }
    free(current);
    if (s != ERROR_SUCCESS) { RegCloseKey(hKey); return s; }

    s = RegSetValueExW(hKey, L"LowerFilters", 0, REG_MULTI_SZ,
                       (BYTE*)updated, updated_size);
    free(updated);
    RegCloseKey(hKey);
    return s;
}

/* ---------------------------------------------------------------------------
 *  Restart all devices in a given class
 * --------------------------------------------------------------------------- */
static DWORD RestartOneDevice(HDEVINFO hdi, PSP_DEVINFO_DATA dev)
{
    SP_PROPCHANGE_PARAMS p = {0};
    p.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.Scope     = DICS_FLAG_CONFIGSPECIFIC;
    p.HwProfile = 0;

    p.StateChange = DICS_STOP;
    if (!SetupDiSetClassInstallParamsW(hdi, dev, &p.ClassInstallHeader, sizeof(p)))
        return GetLastError();
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hdi, dev))
        return GetLastError();

    p.StateChange = DICS_START;
    if (!SetupDiSetClassInstallParamsW(hdi, dev, &p.ClassInstallHeader, sizeof(p)))
        return GetLastError();
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hdi, dev))
        return GetLastError();

    return ERROR_SUCCESS;
}

static void RestartAllDevicesInClass(const WCHAR* class_guid_str)
{
    GUID g;
    if (FAILED(CLSIDFromString(class_guid_str, &g))) return;

    HDEVINFO hdi = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT);
    if (hdi == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hdi, i, &dev); ++i) {
        RestartOneDevice(hdi, &dev);
    }
    SetupDiDestroyDeviceInfoList(hdi);
}

/* ---------------------------------------------------------------------------
 *  Virtual root\HideFilter device installation
 * --------------------------------------------------------------------------- */
static const GUID GUID_DEVCLASS_SYSTEM_LOCAL =
    { 0x4d36e97d, 0xe325, 0x11ce,
      {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18} };

static DWORD InstallVirtualRootDevice(const WCHAR* inf_full_path)
{
    HDEVINFO hdi = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SYSTEM_LOCAL, NULL);
    if (hdi == INVALID_HANDLE_VALUE) return GetLastError();

    SP_DEVINFO_DATA d = { sizeof(d) };
    if (!SetupDiCreateDeviceInfoW(hdi, L"HideFilter Virtual Device",
                                  &GUID_DEVCLASS_SYSTEM_LOCAL, NULL, NULL,
                                  DICD_GENERATE_ID, &d)) {
        DWORD e = GetLastError();
        SetupDiDestroyDeviceInfoList(hdi);
        return e;
    }

    static const WCHAR hwid[] = L"root\\HideFilter\0";   /* MULTI_SZ */
    if (!SetupDiSetDeviceRegistryPropertyW(hdi, &d, SPDRP_HARDWAREID,
                                           (const BYTE*)hwid, sizeof(hwid))) {
        DWORD e = GetLastError();
        SetupDiDestroyDeviceInfoList(hdi);
        return e;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hdi, &d)) {
        DWORD e = GetLastError();
        SetupDiDestroyDeviceInfoList(hdi);
        return e;
    }

    BOOL reboot = FALSE;
    UpdateDriverForPlugAndPlayDevicesW(NULL, L"root\\HideFilter",
                                       inf_full_path,
                                       INSTALLFLAG_FORCE, &reboot);

    SetupDiDestroyDeviceInfoList(hdi);
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------------------
 *  Public API
 * --------------------------------------------------------------------------- */
DWORD HideFilter_Install(const WCHAR* inf_full_path, const WCHAR* svc_name)
{
    /* 1) copy INF into the driver store */
    WCHAR dest_inf[MAX_PATH] = {0};
    if (!SetupCopyOEMInfW(inf_full_path, NULL, SPOST_PATH, 0, dest_inf,
                          _countof(dest_inf), NULL, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS) return e;
    }

    /* 2) install root\HideFilter (loads the driver) */
    (void)InstallVirtualRootDevice(inf_full_path);

    /* 3) inject filter into LowerFilters of every target class */
    for (size_t i = 0; i < NUM_TARGET_CLASSES; ++i) {
        ModifyClassLowerFilters(g_TargetClassGuids[i], svc_name, OP_ADD);
    }

    /* 4) restart every device in those classes so the filter joins their stacks */
    for (size_t i = 0; i < NUM_TARGET_CLASSES; ++i) {
        RestartAllDevicesInClass(g_TargetClassGuids[i]);
    }

    return ERROR_SUCCESS;
}

DWORD HideFilter_Uninstall(const WCHAR* svc_name, const WCHAR* inf_filename)
{
    /* 1) detach the filter from every class -- BEFORE restarting -- */
    for (size_t i = 0; i < NUM_TARGET_CLASSES; ++i) {
        ModifyClassLowerFilters(g_TargetClassGuids[i], svc_name, OP_REMOVE);
    }
    /* 2) restart everything so the filter exits the stacks */
    for (size_t i = 0; i < NUM_TARGET_CLASSES; ++i) {
        RestartAllDevicesInClass(g_TargetClassGuids[i]);
    }
    /* 3) only now is it safe to uninstall the INF package */
    SetupUninstallOEMInfW(inf_filename, SUOI_FORCEDELETE, NULL);
    return ERROR_SUCCESS;
}

#ifdef HIDEFILTER_SVC_MAIN
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage: HideFilterSvc.exe install|uninstall\n");
        return 1;
    }
    if (_wcsicmp(argv[1], L"install") == 0) {
        wchar_t inf[MAX_PATH];
        GetFullPathNameW(L"HideFilter.inf", _countof(inf), inf, NULL);
        DWORD e = HideFilter_Install(inf, L"HideFilter");
        wprintf(L"install: %lu\n", e);
        return (int)e;
    }
    if (_wcsicmp(argv[1], L"uninstall") == 0) {
        DWORD e = HideFilter_Uninstall(L"HideFilter", L"HideFilter.inf");
        wprintf(L"uninstall: %lu\n", e);
        return (int)e;
    }
    return 2;
}
#endif
