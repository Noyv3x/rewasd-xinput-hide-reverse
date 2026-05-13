/*
 * HidClassResolver.c -- Resolve HIDCLASS!HidNotifyPresence at driver load time
 * by walking PsLoadedModuleList and parsing HIDCLASS.SYS's PE export table.
 *
 * Why we can't just call MmGetSystemRoutineAddress(L"HidNotifyPresence"):
 *   Microsoft documents MmGetSystemRoutineAddress as supporting ntoskrnl and
 *   HAL exports only. Empirically it sometimes returns HIDCLASS exports too,
 *   but not on every Windows 10/11 build. Doing the lookup ourselves makes
 *   the driver work reliably across the matrix.
 */

#include <ntifs.h>

typedef VOID (NTAPI *PFN_HID_NOTIFY_PRESENCE)(PDEVICE_OBJECT, BOOLEAN);

PFN_HID_NOTIFY_PRESENCE g_HidNotifyPresence = NULL;

/* Subset of LDR_DATA_TABLE_ENTRY -- only what we need. Layout is stable
 * across all supported Windows 10/11 builds. */
typedef struct _HF_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY      InLoadOrderLinks;
    LIST_ENTRY      InMemoryOrderLinks;
    LIST_ENTRY      InInitializationOrderLinks;
    PVOID           DllBase;
    PVOID           EntryPoint;
    ULONG           SizeOfImage;
    UNICODE_STRING  FullDllName;
    UNICODE_STRING  BaseDllName;
} HF_LDR_DATA_TABLE_ENTRY, *PHF_LDR_DATA_TABLE_ENTRY;

extern PLIST_ENTRY PsLoadedModuleList;

static PVOID HfFindExport(PVOID DllBase, PCSTR Name)
{
    if (!DllBase) return NULL;

    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)DllBase;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((PUCHAR)DllBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

        IMAGE_DATA_DIRECTORY expDir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (expDir.Size == 0) return NULL;

        PIMAGE_EXPORT_DIRECTORY ed =
            (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)DllBase + expDir.VirtualAddress);

        PULONG  names    = (PULONG) ((PUCHAR)DllBase + ed->AddressOfNames);
        PULONG  funcs    = (PULONG) ((PUCHAR)DllBase + ed->AddressOfFunctions);
        PUSHORT ordinals = (PUSHORT)((PUCHAR)DllBase + ed->AddressOfNameOrdinals);

        for (ULONG i = 0; i < ed->NumberOfNames; ++i) {
            PCSTR n = (PCSTR)((PUCHAR)DllBase + names[i]);
            if (strcmp(n, Name) == 0) {
                return (PUCHAR)DllBase + funcs[ordinals[i]];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return NULL;
}

NTSTATUS HfResolveHidNotifyPresence(VOID)
{
    if (!PsLoadedModuleList) return STATUS_UNSUCCESSFUL;

    UNICODE_STRING target;
    RtlInitUnicodeString(&target, L"HIDCLASS.SYS");

    PLIST_ENTRY head = PsLoadedModuleList;
    for (PLIST_ENTRY cur = head->Flink; cur != head; cur = cur->Flink) {
        PHF_LDR_DATA_TABLE_ENTRY m =
            CONTAINING_RECORD(cur, HF_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (m->BaseDllName.Buffer == NULL || m->BaseDllName.Length == 0) continue;

        if (RtlEqualUnicodeString(&m->BaseDllName, &target, TRUE)) {
            PVOID p = HfFindExport(m->DllBase, "HidNotifyPresence");
            if (p) {
                g_HidNotifyPresence = (PFN_HID_NOTIFY_PRESENCE)p;
                return STATUS_SUCCESS;
            }
            break;
        }
    }

    /* Fallback to the documented API. Sometimes works. */
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"HidNotifyPresence");
    PVOID p = MmGetSystemRoutineAddress(&name);
    if (p) {
        g_HidNotifyPresence = (PFN_HID_NOTIFY_PRESENCE)p;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}
