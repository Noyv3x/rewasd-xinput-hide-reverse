# XInput Controller Hiding — Implementation Reference

A complete, portable reference for hiding physical Xbox controllers (XInput / GIP) from Windows applications and games. This is the technique a custom gamepad remapper uses to present a clean *virtual* controller while keeping the physical one invisible to **everything** — including `xinput1_4.dll`, `xinput1_3.dll`, `xinput9_1_0.dll`, `GameInput.dll`, `Windows.Gaming.Input`, DirectInput 8, and RawInput.

> This package contains only the implementation artefacts (driver source, INF, installer, client demo). The technique itself was reverse-engineered from a commercial product; that part of the work is out of scope here.

---

## The problem with HidHide

[HidHide](https://github.com/ViGEm/HidHide) installs as an *UpperFilter* of the **HIDClass** device class. XInput devices do **not** live under HIDClass:

| Device | Function driver | Device class |
|---|---|---|
| Xbox 360 (wired) | `xusb22.sys` | **XnaComposite** |
| Xbox 360 Wireless Receiver controllers | `xusb22.sys` | **XnaComposite** |
| Xbox One / Series (wired & Bluetooth via GIP) | `xboxgip.sys` + `GameInput.sys` | **XboxComposite** |
| Xbox Wireless Adapter | NDIS LWF surface | media type `xboxwirelessinput` |

HidHide's filter never gets attached to the stacks of these devices, so games keep seeing them through the XInput API.

## The solution

Install **one** kernel filter driver, but register it as a **LowerFilter of five device classes simultaneously** — including XboxComposite and XnaComposite. Once on the stack, the filter can issue `HIDCLASS!HidNotifyPresence(PDO, FALSE)` against the controller's PDO. The PnP manager removes the device from all user-mode enumerations.

This works on XInput devices despite them not being in HIDClass: the filter is on the device's *own* stack and holds the PDO directly, so it asks the PnP manager to invalidate device-relations for that PDO. PnP doesn't care which class the device belongs to.

### The five target classes

| GUID | Class | Why |
|---|---|---|
| `{05f5cfe2-4733-4950-a6bb-07aad01a3a84}` | **XboxComposite** | Xbox One / Series controllers + Wireless Adapter root |
| `{d61ca365-5af4-4486-998b-9db4734c6ca3}` | **XnaComposite** | Xbox 360 controllers + Wireless Receiver-attached pads |
| `{745a17a0-74d3-11d0-b6fe-00a0c90f57da}` | HIDClass | DirectInput / RawInput visibility |
| `{4d36e96b-e325-11ce-bfc1-08002be10318}` | Keyboard | optional, for keyboard remap use cases |
| `{4d36e96f-e325-11ce-bfc1-08002be10318}` | Mouse | optional, for mouse remap use cases |

Only the first three are strictly required to hide a gamepad from every input API on Windows.

---

## End-to-end picture

```
                 your remapping application
                            │
              CreateFile + DeviceIoControl
                            │
                            ▼
                  HideFilter device interface
                  (one instance per affected PDO)
                            │
   ┌────────────────────────┴────────────────────────┐
   │ kernel: HideFilter.sys (this repo)              │
   │  ─ attached as LowerFilter to 5 device classes  │
   │  ─ holds physical PDO via WdfDeviceWdmGet…      │
   │  ─ refcounted toggle:                           │
   │      0 → 1 : HidNotifyPresence(PDO, FALSE)      │
   │      1 → 0 : HidNotifyPresence(PDO, TRUE )      │
   └────────────────────────┬────────────────────────┘
                            ▼
              xboxgip.sys / xusb22.sys / hidclass.sys
                            ▼
                       physical PDO
```

One-time setup (user-mode installer, `src/service/`):

```
1) SetupCopyOEMInfW           — copy INF into the driver store
2) DIF_REGISTERDEVICE         — create the root\HideFilter virtual device
   UpdateDriverForPlugAndPlayDevicesW   so the driver is loaded
3) For each of the 5 class GUIDs:
      Append "HideFilter" to HKLM\…\Class\{GUID}\LowerFilters  (REG_MULTI_SZ)
4) For each affected device currently plugged in:
      DIF_PROPERTYCHANGE  DICS_STOP, then DICS_START
   (so the filter enters the existing stacks)
```

Runtime (your remapper, `src/client/`):

```
HANDLE h = CreateFile("\\?\…HideFilter…")        // one instance per filtered PDO
DeviceIoControl(h, IOCTL_HIDEFILTER_TOGGLE_PRESENCE,
                &(BYTE){0|1}, 1, NULL, 0, &ret, NULL);
//                            ↑  0 = hide   1 = show
```

---

## Repository layout

```
.
├── README.md                       this file
├── inf/
│   ├── HideFilter.inf              main filter + virtual root device
│   ├── HideFilter-NDIS.inf         optional: NDIS LWF for Xbox Wireless Adapter
│   └── README.md
├── src/
│   ├── driver/                     KMDF lower-filter driver
│   │   ├── HideFilter.c
│   │   ├── HidClassResolver.c
│   │   ├── HideFilter.h
│   │   └── README.md
│   ├── service/                    user-mode installer / LowerFilters writer
│   │   ├── HideFilterSvc.c
│   │   ├── HideFilterSvc.h
│   │   └── README.md
│   └── client/                     application-side example
│       ├── hide-example.c
│       └── README.md
└── reference/
    ├── class-guids.md              5 target classes, in detail
    └── protocol.md                 IOCTL contract
```

---

## Quick start

1. **Generate your own GUIDs.** Replace `{REPLACE-WITH-YOUR-GUID-…}` placeholders in `src/driver/HideFilter.h` and the INF files. Use `guidgen.exe` from the SDK.
2. **Build the driver.** Open the WDK project in Visual Studio 2022 and produce `HideFilter.sys` + `HideFilter.cat`. See `src/driver/README.md`.
3. **Sign it.** For dev: testsigning with a self-signed cert. For release: EV cert + Microsoft Partner Center attestation.
4. **Build the installer/uninstaller.** See `src/service/README.md`. Run as Administrator.
5. **Hide a controller** from your app via `src/client/hide-example.c`.

---

## Driver protocol summary

One IOCTL, defined in `src/driver/HideFilter.h`:

```c
#define IOCTL_HIDEFILTER_TOGGLE_PRESENCE \
    CTL_CODE(0x002A, 0xA78, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
// = 0x002AE9E0
// Input  : 1 byte. 0 = hide, non-zero = show.
// Output : none.
```

Per-handle refcount, so app crashes do not leave devices stuck-hidden.

---

## Operational requirements (don't skip these)

* **INF `StartType = SERVICE_BOOT_START` and `LoadOrderGroup = Filter` are mandatory.** The filter has to be in memory before any device in the affected classes starts.
* **Adding to LowerFilters only takes effect on next enumeration.** Use `DIF_PROPERTYCHANGE` `DICS_STOP` then `DICS_START` on every existing device in those classes immediately after editing the registry.
* **`HidNotifyPresence` is exported from `HIDCLASS.SYS`, not from `ntoskrnl.exe`.** `MmGetSystemRoutineAddress` does not reliably resolve it on every Windows build. The included `HidClassResolver.c` walks `PsLoadedModuleList`, finds `HIDCLASS.SYS`, and parses its export table manually. **Don't skip this** — your driver will silently fail to hide on some Windows versions if you rely only on `MmGetSystemRoutineAddress`.
* **Uninstall order matters.** Remove your service from LowerFilters → restart affected devices → `SetupUninstallOEMInfW`. Reversing this order can leave devices unable to start.
* **Force-release on remove.** In `EvtDeviceContextCleanup`, if your refcount is non-zero and the device is being removed (cable unplug etc.), call `HidNotifyPresence(PDO, TRUE)` so the next plug-in is visible.
* **Do not use `SetupDiCallClassInstaller(DICS_DISABLE)`.** That leaves a yellow bang in Device Manager and confuses users. The whole point of `HidNotifyPresence` is to hide cleanly.

---

## Driver-signing reality check

Windows 10/11 require all kernel drivers to be cryptographically signed. For public distribution you need:

1. **EV code-signing certificate** (~ $300 / yr).
2. **Microsoft Partner Center** attestation signature, *or* full WHQL submission.
3. **HVCI compatibility.** The included source is HVCI-clean (no RWX kernel memory, no patching, no undocumented IRP fiddling).

For development:

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

```cmd
makecert -r -pe -ss PrivateCertStore -n "CN=TestDevCert" TestDevCert.cer
signtool sign /v /s PrivateCertStore /n TestDevCert /t http://timestamp.digicert.com HideFilter.sys
inf2cat /driver:. /os:10_X64
signtool sign /v /s PrivateCertStore /n TestDevCert /t http://timestamp.digicert.com HideFilter.cat
```

---

## What this hides from / does not hide from

**Hidden from:**

* XInput API — `XInputGetState`, `XInputGetCapabilities`, etc. across all three XInput DLL versions
* `GameInput.dll` (the newest controller API)
* `Windows.Gaming.Input` — `Gamepad`, `RawGameController` enumeration
* `hid.dll` / `HidD_*` raw HID
* `RawInput` — `GetRawInputDeviceList`
* DirectInput 8 — `IDirectInput8::EnumDevices`

**Not hidden from:**

* WinUSB / libusb-style direct opening by USB instance ID. Programs that bypass the function driver are out of scope. Hiding from those requires an additional USB lower filter, which is not provided here.

---

## License

MIT — see `LICENSE`. Use at your own risk. Driver development can render systems unbootable; test in a VM with kernel debugging attached.

---

## Acknowledgement

The technique is not novel — it has been employed in commercial software for years. This package documents and reimplements the approach in a portable, MIT-licensed form so that hobby remapper authors don't have to rediscover it.
