# HideFilter — KMDF lower-filter driver

| File | Purpose |
|---|---|
| `HideFilter.c` | KMDF entry point, `EvtDeviceAdd`, IOCTL dispatcher |
| `HidClassResolver.c` | Resolves `HIDCLASS!HidNotifyPresence` by walking `PsLoadedModuleList` and parsing PE exports manually |
| `HideFilter.h` | Public types shared with user mode (device-interface GUID, IOCTL code) |

## Prerequisites

* Visual Studio 2022 with the **Windows Driver Kit (WDK)** workload installed.
* Spectre-mitigated MSVC libraries (the WDK installer adds these).
* A code-signing certificate. For dev work: any self-signed cert + `bcdedit /set testsigning on`. For release: an EV cert.

## Building

1. In Visual Studio: **File → New → Project → Kernel Mode Driver, Empty (KMDF)**. Name it `HideFilter`.
2. Right-click the project → **Add → Existing item** — pick `HideFilter.c`, `HidClassResolver.c`, `HideFilter.h`.
3. Right-click the project → **Add → Existing item** — pick `../../inf/HideFilter.inf`. Visual Studio will treat it as the install INF for the package.
4. Project properties:
   * **Configuration**: `Release`, `x64`.
   * **Driver Settings → General → Target Platform**: `Windows 10 or later`.
   * **Driver Signing → Sign Mode**: `Test Sign` (during dev) or `Off` (production — you sign manually).
5. **Build**. Output:
   * `HideFilter.sys`
   * `HideFilter.cat`
   * (after `inf2cat`/signing) a deployable package.

## Manual signing for testing

```cmd
:: 1) create + import a dev cert into PrivateCertStore
makecert -r -pe -ss PrivateCertStore -n "CN=HideFilterDev" HideFilterDev.cer
certmgr.exe -add HideFilterDev.cer -s -r localMachine root
certmgr.exe -add HideFilterDev.cer -s -r localMachine trustedpublisher

:: 2) sign the .sys
signtool sign /v /s PrivateCertStore /n HideFilterDev ^
  /t http://timestamp.digicert.com /fd SHA256 HideFilter.sys

:: 3) build a catalogue and sign it
inf2cat /driver:. /os:10_X64
signtool sign /v /s PrivateCertStore /n HideFilterDev ^
  /t http://timestamp.digicert.com /fd SHA256 HideFilter.cat

:: 4) enable testsigning on the target machine and reboot
bcdedit /set testsigning on
shutdown /r /t 0
```

## Verifying

After the user-mode installer runs and you `Stop+Start` an Xbox controller:

```cmd
:: Should show your driver loaded
sc query HideFilter

:: Should show your driver attached as filter to e.g. an Xbox controller node
:: (run as admin):
pnputil /enum-drivers | findstr HideFilter

:: Driver Verifier with default flags is recommended during development.
verifier /flags 0x209BB /driver HideFilter.sys
```

## Common pitfalls

* **Forgetting `WdfFdoInitSetFilter`** — without it, the driver is treated as a function driver and the device stack breaks. Symptoms: device gets a yellow bang the moment your filter loads.
* **Skipping the manual PE export walk** for `HidNotifyPresence` — works on your dev machine, silently breaks for users.
* **Not registering the device interface** — the user-mode client cannot find the per-PDO handle to send the IOCTL to.
* **Leaving `HideRefCount > 0` on device cleanup** — next time the user plugs the pad in it stays hidden until reboot.
