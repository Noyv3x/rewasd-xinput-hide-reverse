# INF files

| File | Required? | Purpose |
|---|---|---|
| `HideFilter.inf` | **yes** | Installs the kernel filter driver as a boot-start service, plus a virtual `root\HideFilter` device that the driver attaches to (this is how the user-mode service finds the driver). |
| `HideFilter-NDIS.inf` | optional | Registers the driver as an NDIS Lightweight Filter for `xboxwirelessinput` media type. **Only needed if you want to also hide the Xbox One Wireless Adapter root.** The vast majority of users do not need this. |

## Before building

Open each INF and replace the placeholder values:

* `%Vendor%` → your company / project name
* `DriverVer` → today's date and your driver version
* In `HideFilter-NDIS.inf`, `NetCfgInstanceId` must be a **freshly-generated GUID** (use `guidgen.exe`).

## Critical settings — do not change

* `Class = System` + `ClassGUID = {4d36e97d-…}` (System class is the correct class for a filter that's installed via `root\` and uses LowerFilters injection rather than HardwareID matching).
* `[Service_Inst].StartType = 0` (SERVICE_BOOT_START) — must be boot-start.
* `[Service_Inst].LoadOrderGroup = Filter` — must be in the Filter group.
* `PnpLockdown = 1` — required by modern Windows for filter-style INFs.
* `[Install.NTamd64.HW].AddReg = Security_Reg` with the supplied SDDL — gives SYSTEM full access and read-only to admins/everyone, which is the minimum your user-mode service needs to send IOCTLs.

## Generating .cat and signing

```cmd
inf2cat /driver:. /os:10_X64
signtool sign /fd SHA256 /a /t http://timestamp.digicert.com HideFilter.cat
```
