/*
 * HideFilter.h -- Public types shared between the HideFilter driver and any
 * user-mode component that talks to it (installer / runtime client).
 *
 * Include exactly once with INITGUID defined to materialise the GUID.
 */
#pragma once

#include <initguid.h>
#include <Windows.h>      // for METHOD_BUFFERED, FILE_READ_DATA, etc. in user-mode
#ifndef _Inout_
#include <sdkddkver.h>
#endif

/* ---------------------------------------------------------------------------
 *  Device interface GUID for HideFilter instances
 *  -----------------------------------------------
 *  ⚠ REPLACE THIS WITH YOUR OWN GUID (run guidgen.exe). Two unrelated kernel
 *    drivers must not share device-interface GUIDs.
 * --------------------------------------------------------------------------- */
// {A1B2C3D4-5E6F-7081-92A3-B4C5D6E7F809}   -- placeholder, replace before shipping
DEFINE_GUID(GUID_DEVINTERFACE_HIDEFILTER,
    0xA1B2C3D4, 0x5E6F, 0x7081, 0x92, 0xA3, 0xB4, 0xC5, 0xD6, 0xE7, 0xF8, 0x09);

/* ---------------------------------------------------------------------------
 *  IOCTL contract
 * --------------------------------------------------------------------------- */
#define HIDEFILTER_DEVICE_TYPE   0x002A   /* arbitrary FILE_DEVICE_* value */

/*
 * IOCTL_HIDEFILTER_TOGGLE_PRESENCE
 *
 *   Direction : input only
 *   Input     : 1 byte
 *                  0  -> hide      (refcount++ ; on 0->1 transition the driver
 *                                   calls HidNotifyPresence(PDO, FALSE))
 *                  !=0-> show      (refcount-- ; on 1->0 transition the driver
 *                                   calls HidNotifyPresence(PDO, TRUE))
 *   Output    : none
 *
 *   Refcount is maintained per file-handle, so application crashes never leave
 *   devices stuck hidden.
 */
#define IOCTL_HIDEFILTER_TOGGLE_PRESENCE \
    CTL_CODE(HIDEFILTER_DEVICE_TYPE, 0xA78, METHOD_BUFFERED, \
             FILE_READ_DATA | FILE_WRITE_DATA)
/* Numerically: 0x002AE9E0 */
