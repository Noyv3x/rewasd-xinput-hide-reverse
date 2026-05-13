# Client example

A small console tool that demonstrates the runtime side of the technique.

## Build

```cmd
cl /nologo /W3 hide-example.c /link Setupapi.lib Advapi32.lib Cfgmgr32.lib /out:hide.exe
```

## Use

```cmd
:: list every filter instance currently attached to a physical device
hide list

:: hide the Nth one
hide hide 0

:: show it again
hide show 0
```

The "parent" field shown by `hide list` is the device instance ID of the controller
that this filter handle controls (for example `USB\VID_045E&PID_0B12\01234`). In a
real remapper you would build a map from your own physical-device identification
to filter-instance indexes once at startup, instead of asking the user for indexes.

## Integrating into your remapper

In your remapper process:

```c
#define INITGUID
#include "HideFilter.h"   // copy from src/driver/

HANDLE OpenFilterFor(const WCHAR* targetParentInstanceId);
BOOL   HideController(HANDLE h)  { UCHAR x = 0; DWORD r; return DeviceIoControl(h, IOCTL_HIDEFILTER_TOGGLE_PRESENCE, &x, 1, NULL, 0, &r, NULL); }
BOOL   ShowController(HANDLE h)  { UCHAR x = 1; DWORD r; return DeviceIoControl(h, IOCTL_HIDEFILTER_TOGGLE_PRESENCE, &x, 1, NULL, 0, &r, NULL); }
```

Hold the handle open for as long as you want the device hidden. Closing it
implicitly releases the refcount (so if your process dies, the device becomes
visible again — which is the safe default).
