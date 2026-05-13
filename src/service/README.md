# HideFilterSvc — Installer / LowerFilters writer

User-mode component that performs one-shot install and uninstall of the HideFilter driver. Can be built standalone (`.exe`) or as a static library linked into your main remapper process.

| File | Purpose |
|---|---|
| `HideFilterSvc.c` | Implementation |
| `HideFilterSvc.h` | Public API |

## What it does (in order)

`HideFilter_Install`:
1. `SetupCopyOEMInfW` — copy your `.inf` into `%SystemRoot%\System32\DriverStore\FileRepository`.
2. Create a `root\HideFilter` virtual device and load the driver via `UpdateDriverForPlugAndPlayDevicesW`.
3. Append `"HideFilter"` to the `LowerFilters` REG_MULTI_SZ in each of the five class registry keys.
4. `DIF_PROPERTYCHANGE` Stop+Start every existing device in those classes so the filter enters their stacks now (instead of after next boot / device re-enumeration).

`HideFilter_Uninstall` reverses everything in safe order.

## Build (standalone exe)

```cmd
cl /nologo /W3 /DHIDEFILTER_SVC_MAIN HideFilterSvc.c ^
   /link Setupapi.lib Newdev.lib Advapi32.lib Ole32.lib ^
   /out:HideFilterSvc.exe
```

## Build (library to link into your app)

```cmd
cl /nologo /W3 /c HideFilterSvc.c
lib /out:HideFilterSvc.lib HideFilterSvc.obj
```

In your app:

```c
#include "HideFilterSvc.h"
DWORD err = HideFilter_Install(L"C:\\Path\\HideFilter.inf", L"HideFilter");
```

## Run (must be elevated)

Place `HideFilter.inf`, `HideFilter.sys`, `HideFilter.cat`, and `HideFilterSvc.exe` in the same directory, then:

```cmd
HideFilterSvc.exe install
…
HideFilterSvc.exe uninstall
```

## Notes

* **Always elevated.** All `SetupDi*` and `RegSetValueEx` on `HKLM\System\…\Class\…` require Administrator.
* **One-shot, not a service.** Despite the file name, this isn't a Windows Service binary. It's invoked once at install time and once at uninstall. The runtime hide/unhide commands go from your main app *directly* to the kernel driver via DeviceIoControl (see `../client/`).
* **Idempotent.** Running `install` twice does no harm — `ModifyClassLowerFilters` checks for the existing entry and skips it.
* **Uninstall order matters.** The code removes the filter from LowerFilters *before* tearing down the driver. If you reverse this, you risk class-keys referencing a service that no longer exists, which can prevent affected devices from starting.
