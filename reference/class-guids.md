# Target Device Classes

The filter driver is registered as a `LowerFilter` of all five of these classes simultaneously. Each class covers a different family of physical input devices. **Edit `HKLM\System\CurrentControlSet\Control\Class\{GUID}\LowerFilters`** (a `REG_MULTI_SZ`) and append your service name.

| GUID | Class | Physical devices |
|---|---|---|
| `{05f5cfe2-4733-4950-a6bb-07aad01a3a84}` | **XboxComposite** | Xbox One / Series S / Series X controllers (wired and Bluetooth via GIP). The Xbox Wireless Adapter root also enumerates here. |
| `{d61ca365-5af4-4486-998b-9db4734c6ca3}` | **XnaComposite** | Xbox 360 wired controllers; controllers attached via Xbox 360 Wireless Receiver. The `xusb22.sys` minidriver loads here. |
| `{745a17a0-74d3-11d0-b6fe-00a0c90f57da}` | HIDClass | All HID-aware devices — covers the DirectInput / RawInput / `HidD_*` enumeration paths. |
| `{4d36e96b-e325-11ce-bfc1-08002be10318}` | Keyboard | Standard keyboard class. Optional unless your remapper also handles keyboards. |
| `{4d36e96f-e325-11ce-bfc1-08002be10318}` | Mouse | Standard mouse class. Optional unless your remapper also handles mice. |

## What each is for

* **XboxComposite** is the Win10/11 "modern" class for any GIP device (Xbox One, Xbox Series, etc.). XInput's newer surface (`Windows.Gaming.Input`, `GameInput.dll`, and recent `xinput1_4.dll` builds) all read controllers through this stack. **Filter must be here to hide modern Xbox pads.**
* **XnaComposite** is the legacy class for `xusb22.sys`-driven controllers. **Filter must be here to hide Xbox 360 pads** and anything plugged into a Wireless Receiver.
* **HIDClass** is what HidHide already filters. Including it gives you DirectInput 8 / RawInput / HID-API coverage. Without it, games that read DirectInput devices (older titles) would still see the pad.
* **Keyboard / Mouse** are only relevant if your remapping software intercepts keyboard/mouse devices. Most XInput-focused remappers can omit these two.

## Minimum set for "hide any Xbox controller from games"

```
XboxComposite
XnaComposite
HIDClass
```

## Why this works even though `xusb22.sys` / `xboxgip.sys` aren't HID drivers

`HidNotifyPresence` is not really "a HID-only function". What it actually does is ask the PnP manager to invalidate device relations for a given PDO. PnP doesn't care which device class the PDO belongs to — once notified that the PDO is not present, it removes the device from *every* user-mode enumeration surface (XInput, GameInput, HID, RawInput, DirectInput, …).

We only need to be on the device's stack to obtain its PDO. LowerFilter injection on the device's class gets us exactly that.
