# Driver IOCTL Contract

The HideFilter driver exposes one IOCTL.

## `IOCTL_HIDEFILTER_TOGGLE_PRESENCE`

```c
#define IOCTL_HIDEFILTER_TOGGLE_PRESENCE \
    CTL_CODE(0x002A, 0xA78, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
// numerically: 0x002AE9E0
```

| Field | Value |
|---|---|
| Direction | input only |
| Input size | 1 byte |
| Input value | `0` ⇒ hide ; non-zero ⇒ show |
| Output | none |

## Semantics

Each filter instance corresponds to one physical device's stack (one PDO). Sending the IOCTL against a filter handle affects only that PDO.

The driver maintains a per-device hide refcount with these transitions:

| Transition | Driver action |
|---|---|
| `0 → 1` (first hide) | `HidNotifyPresence(PDO, FALSE)` — device disappears from XInput, GameInput, HID, RawInput, DirectInput |
| `1 → 0` (last show)  | `HidNotifyPresence(PDO, TRUE)` — device reappears |
| `n → n+1`, `n+1 → n` (n ≥ 1) | refcount only; no API call |

Refcount is tracked per file handle. If your process dies, Windows closes the handle and the kernel automatically decrements. This is the safe default — a crashing remapper can never leave a controller stuck-hidden.

## Opening the right handle

```c
HDEVINFO hdi = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HIDEFILTER, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hdi, NULL,
                  &GUID_DEVINTERFACE_HIDEFILTER, i, &ifd); ++i) {
    /* getDetail(), look at the parent device-instance-id with
       CM_Get_Parent + CM_Get_Device_IDW, match against the controller
       you want to hide (by VID/PID/InstanceId), CreateFile() the
       interface path. */
}
```

Each iteration of the loop is one "filtered" PDO. You'll see one filter interface per physical device that currently has the filter attached to its stack — i.e. one per Xbox controller, plus one per keyboard/mouse/HID device since those classes also have the filter registered.

In practice your remapper does this mapping once at startup and keeps the handles open.

## Calling the IOCTL

```c
UCHAR hide  = 0;     /* 0 hide,  non-zero show */
DWORD ret   = 0;
BOOL  ok    = DeviceIoControl(h, IOCTL_HIDEFILTER_TOGGLE_PRESENCE,
                              &hide, sizeof(hide), NULL, 0, &ret, NULL);
```

## Return values

| GetLastError()                       | Meaning |
|---|---|
| `ERROR_SUCCESS` (0)                  | Done. Device should now be in the requested state. |
| `ERROR_INVALID_FUNCTION` (1)         | You sent something other than `IOCTL_HIDEFILTER_TOGGLE_PRESENCE`. |
| `ERROR_INSUFFICIENT_BUFFER` (122)    | Input buffer was < 1 byte. |
| `ERROR_NOT_SUPPORTED` (50)           | The driver could not resolve `HIDCLASS!HidNotifyPresence` at load time. Indicates a build issue — check `HidClassResolver.c`. |
