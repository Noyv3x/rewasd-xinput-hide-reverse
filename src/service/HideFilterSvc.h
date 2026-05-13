/*
 * HideFilterSvc.h -- User-mode installer API for HideFilter.
 *
 * Link with Setupapi.lib, Newdev.lib, Advapi32.lib.
 */
#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-shot install:
 *   1) SetupCopyOEMInfW(inf_full_path) -- copy INF into the driver store
 *   2) Register root\HideFilter virtual device + UpdateDriverForPlugAndPlay…
 *      (loads HideFilter.sys boot-time start service)
 *   3) Append svc_name (e.g. L"HideFilter") to the LowerFilters value of
 *      each of the five target device classes
 *   4) DIF_PROPERTYCHANGE Stop+Start every existing device in those classes
 *      so the filter enters their stacks immediately
 *
 * Must be run elevated.
 */
DWORD HideFilter_Install(_In_ const WCHAR* inf_full_path,
                         _In_ const WCHAR* svc_name);

/*
 * Exact reverse: remove the filter entry from every class, restart affected
 * devices so the filter exits their stacks, then uninstall the OEM INF.
 *
 * inf_filename is the *short* INF name as known by SetupAPI (e.g.
 * L"oem17.inf" or L"HideFilter.inf"). Caller is responsible for knowing it
 * (or for enumerating with SetupGetInfDriverStoreLocation if needed).
 */
DWORD HideFilter_Uninstall(_In_ const WCHAR* svc_name,
                           _In_ const WCHAR* inf_filename);

#ifdef __cplusplus
}
#endif
