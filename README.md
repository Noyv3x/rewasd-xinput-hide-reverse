# reWASD 隐藏物理手柄技术方案完整逆向报告

> 目标:从 `reWASD933-11029.exe`(reWASD 9.3.3,2026-03)的二进制中还原其"隐藏物理控制器"的技术方案,**重点覆盖 HidHide 无法处理的 XInput / XUSB / GIP 设备**,产出可直接照搬到自制手柄映射软件的指导。

---

## 0. TL;DR(执行摘要)

**HidHide 失效的根因**:HidHide 是 HIDClass 上层过滤驱动,只能 hide 走 `HIDClass.sys` 的设备。XInput 系列设备(Xbox 360 用 `xusb22.sys`、Xbox One/Series 走 GIP 用 `xboxgip.sys` + `GameInput.sys`、Xbox Wireless Adapter 用 NDIS 媒体类型 `xboxwirelessinput`)**根本不暴露给 HIDClass**,所以 HidHide 看不见。

**reWASD 的方案核心是把同一个内核驱动 `xjn17hlp.sys` 作为 5 个不同设备类的 LowerFilter 同时注入**,其中包括 Xbox 复合类(`{05f5cfe2-...}`)和 XnaComposite 类(`{d61ca365-...}`)— 这两个类覆盖了所有 XInput / GIP 物理控制器。注入之后,xjn17hlp.sys 作为 lower filter 进入这些设备的驱动栈,在内核层拦截输入 IRP,并通过运行时 IOCTL `0x2AE9E0` 控制单个设备的"隐藏"开关。配合自带的虚拟手柄子设备(root\xjn17hlp 创建的 child PDO),实现游戏 XInput API 看到虚拟手柄、看不见物理手柄的效果。

**移植所需 5 件东西**:
1. 一个 KMDF 过滤驱动(对照本报告 §3 撰写)
2. 一份装驱动+装 INF 的 INF 文件(§4)
3. 一个 user-mode service,执行"将驱动 ID 加入指定 5 个设备类的 LowerFilters,然后逐个 restart 设备"(§5)
4. 一个简单的运行时 IPC(§6)
5. (可选)虚拟手柄 child PDO 用于"占位输出"。可以直接复用现有 ViGEmBus,无需自己写(§7 给替代方案)

---

## 1. 分析过程与证据来源

### 1.1 解包路径

```
reWASD933-11029.exe          ← .NET WPF installer (Costura.Fody 打包)
├─ Properties.Resources.resx (~80 MB)  ← 把所有产物压成 zip 嵌进 .NET resource
│   ├─ commondlls.bin (zip)   → Engine.dll/Common.dll/reWASDUI/...
│   ├─ binx64.bin    (zip)   → xjn17svc.exe / InGameOverlay64.dll / StartDXOverlay64.exe
│   ├─ firmware.bin  (zip)   → reWASD 外设固件 (ESP32 / GIMX 等,无关)
│   └─ ...
└─ xjn17svc.exe 内还嵌了 2 份 PE,即驱动二进制 xjn17hlp.sys (NT native PE32+)
```

可以用 `ilspycmd -p -o out reWASD933-11029.exe`(8.x 版本)反编译 .NET,再脚本解 base64 → zip → 解压。本报告 §A 附完整解包脚本。

### 1.2 关键二进制清单

| 名称 | 类型 | 作用 |
|------|------|------|
| `xjn17hlp.sys` | 内核驱动(KMDF + NDIS LWF) | **核心**:lower filter 驱动 + 虚拟设备总线驱动 |
| `xjn17flt.sys` | INF 配套 | xboxwirelessinput NDIS LWF 注册条目(实体也是 xjn17hlp.sys) |
| `xjn17svc.exe` | x64 native Windows Service | 安装/卸载驱动、改注册表 LowerFilters、转发 IOCTL |
| `reWASDUI.exe / Engine.exe` | .NET 6 WPF / Engine | UI / profile 持久化 / NamedPipe 与服务通信 |

### 1.3 Ghidra 反编译数据

驱动 + 服务都用 pyghidra headless 完整反编译,产物在 `/workspaces/code/rc1/ghidra_proj/out/`:
- `xjn17hlp.sys/decompiled_all/` — 407 个函数全反编译
- `xjn17svc.exe/all_dec_full/` — 4071 个函数全反编译

本报告引用的所有偏移、IOCTL、GUID 都在这些反编译产物里可复查。

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────┐
│  reWASDUI.exe  /  reWASDCommandLine.exe   (.NET 6)   │
│  - 用户在 UI 勾选 "Hide Physical Gamepad"            │
│  - profile.HardwareDevice.HidePhysicalGamepad = true │
└──────────────┬──────────────────────────────────────┘
               │ Named Pipe \\.\pipe\{9AB72C00-54D9-4A0F-A236-C31895AB0B20}
               │ 二进制协议:REWASD_REQUEST_HEADER {uint32 Size; uint32 Command;}
               ▼
┌─────────────────────────────────────────────────────┐
│            xjn17svc.exe  (Windows Service)           │
│  - 装/卸载驱动 INF (SetupCopyOEMInfW + UpdateDriver) │
│  - 注册表注入:把 "xjn17hlp" 加进 5 个 device class │
│    的 LowerFilters(REG_MULTI_SZ)                    │
│  - 调用 SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)│
│    DICS_STOP→DICS_START 重启每个被影响设备让 filter生效│
│  - 为每个物理设备 CreateFile(driver_interface),     │
│    再 DeviceIoControl(handle, 0x2AE9E0, &flag, 1,…) │
└──────────────┬──────────────────────────────────────┘
               │ DeviceIoControl
               ▼
┌─────────────────────────────────────────────────────┐
│              xjn17hlp.sys  (KMDF)                    │
│  作为 LowerFilter 同时挂在:                          │
│   • HIDClass {745a17a0-74d3-11d0-b6fe-00a0c90f57da}  │
│   • Keyboard {4d36e96b-…}                            │
│   • Mouse    {4d36e96f-…}                            │
│   • XboxComposite {05f5cfe2-4733-4950-a6bb-07aad01a3a84} ← XInput! │
│   • XnaComposite  {d61ca365-5af4-4486-998b-9db4734c6ca3} ← XInput! │
│                                                      │
│  Hide 实现:                                          │
│   • 收到 IOCTL 0x2AE9E0 (input=1 byte: 0=hide,1=show)│
│   • 内部维护 per-PDO hide refcount(0x1e0 偏移)      │
│   • 调用 HidNotifyPresence(PDO, IsPresent) ←           │
│     这是 HIDCLASS.SYS 文档化的 export,通过           │
│     MmGetSystemRoutineAddress 动态解析(不是 .lib 链接)│
│   • 对于 Xbox 复合设备(XboxComposite/XnaComposite),  │
│     filter 在 PDO stack 上,通过 IRP 拦截让            │
│     XInput / RawInput / GameInput.dll 都看不到设备    │
└──────────────────────────────────────────────────────┘
```

---

## 3. 内核驱动 `xjn17hlp.sys` 关键实现

### 3.1 导入与符号解析

驱动模块导入(`/workspaces/code/rc1/ghidra_proj/out/xjn17hlp.sys/imports.tsv`)只含 NTOSKRNL / NDIS / WDFLDR / KSECDD,**而 HidNotifyPresence 是通过运行时解析得到的**。

```c
// 伪代码(对应 FUN_140052660,被 DriverEntry 后期调用)
PVOID HidNotifyPresence_ptr = NULL;
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"HidNotifyPresence");
    HidNotifyPresence_ptr = MmGetSystemRoutineAddress(&name);
    // 注意:MmGetSystemRoutineAddress 在 Win10+ 才能解析非 ntoskrnl 的导出。
    // 实际看到的是 reWASD 自己做了一个 "在所有已加载内核模块中搜导出" 的封装
    // (FUN_140052660),先取出 PsLoadedModuleList,逐个 module 解析 export
    // table,这样也能拿到 HIDCLASS.SYS 的导出。
}
```

> 重要:`MmGetSystemRoutineAddress` 文档上只承诺解析 ntoskrnl 和 hal,所以 reWASD 自己实现了"遍历 `PsLoadedModuleList`,加载每个模块 PE 头,查 EAT 找名字"的 hook。这是必要的,因为 `HidNotifyPresence` 是 `HIDCLASS.SYS` 的导出,任何 minidriver/filter 想调用它都需要这样做。

### 3.2 隐藏函数 `FUN_140043a20`(refcount 版的 HidNotifyPresence)

直译自反编译:

```c
// param_1 = client/file 对象上下文
// param_2 = 0 ⇒ hide;非零 ⇒ unhide
NTSTATUS XjnHidePresence(PVOID FileCtx, BOOLEAN Show)
{
    PVOID DevCtx = FileCtx->ParentDevice;          // +0x368

    // 确保设备还在
    if (!ExAcquireRundownProtection(&DevCtx->Rundown)) // +200(0xc8)
        return STATUS_DEVICE_REMOVED;

    KeWaitForSingleObject(&DevCtx->Mutex, ...);    // +0xd0

    if (Show == 0) {           // 请求 hide
        if (FileCtx->HasRequestedHide) goto release;        // +0x1fd
        FileCtx->HasRequestedHide = 1;
        if (DevCtx->HideRefcount++ == 0) {                  // +0x1e0
            // 第一次 hide → 真正调驱动隐藏
            NTSTATUS s = HidNotifyPresence_ptr(DevCtx->Pdo, FALSE); // +0xa8
            // …错误处理…
        }
    } else {                    // 请求 unhide
        if (!FileCtx->HasRequestedHide) goto release;
        FileCtx->HasRequestedHide = 0;
        if (--DevCtx->HideRefcount == 0) {
            HidNotifyPresence_ptr(DevCtx->Pdo, TRUE);
        }
    }
release:
    KeReleaseMutex(&DevCtx->Mutex, FALSE);
    ExReleaseRundownProtection(&DevCtx->Rundown);
    return STATUS_SUCCESS;
}
```

要点:
- **PDO 是被 filter 的物理设备对象指针**。lower filter 在 EvtDeviceAdd 时拿到 `WdfDeviceWdmGetPhysicalDevice(Device)`,存到 DevCtx->Pdo。
- 多客户端引用计数:多个 reWASD profile 同时要求 hide 同一设备,合并到一次 `HidNotifyPresence(pdo, FALSE)`,最后一个 release 时才调 `(pdo, TRUE)`。
- File ctx 上有自己的 "HasRequestedHide" 标志,防止重复操作泄漏 refcount。

### 3.3 IOCTL Dispatcher

驱动主 dispatch 函数:`FUN_14003b200`(在反编译输出 `xjn17hlp.sys/decompiled_all/14003b200_*.c`)。结构:

| 编号 | IOCTL Code | 输入(WdfRequestRetrieveInputBuffer) | 用途 |
|------|------------|--------------------------------------|------|
| case 0 | 0x2AE9DA | – | enumerate / list controllers |
| case 4 | 0x2AE9DE | 4 字节 (report_id ¦ collection_idx) | 从 driver 取 HID input report |
| **case 6** | **0x2AE9E0** | **1 字节 (0=hide, !=0=unhide)** | **本次方案的核心 IOCTL** |
| case 0xc | 0x2AE9E6 | WdfMemory 对象 | bulk transfer-style |
| case 0xe..0x46 | 0x2AE9E8 … 0x2AEA20 | 杂项 | 注入按键、设置 LED、震动、配对、电池查询、自适应扳机等 |

IOCTL 编码方式都是 `CTL_CODE(0x002A, function, METHOD_*, FILE_READ_DATA|FILE_WRITE_DATA)`,DeviceType 是 reWASD 自定义的 `0x002A`(无标准含义)。

调用约束:
- IOCTL 0x2AE9E0 的输入必须是 **1 字节**,内容是 `0` 或 `1`。
- handle 必须是通过 `\\?\<driver-interface-path>` 打开的,见 §3.4。

### 3.4 设备接口与 user-mode 打开方式

xjn17hlp 在 `WdfDeviceCreateDeviceInterface` 注册了若干 GUID。用户态服务通过 `SetupDiGetClassDevsW(&GUID, 0, 0, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)` 枚举,然后 `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetailW` 拿到形如:

```
\\?\ROOT#xjn17hlp#0000#{8999D406-CC96-496B-B2EE-1147253985E3}
```

其中 `{8999D406-CC96-496B-B2EE-1147253985E3}` 是 reWASD 自定义的 driver interface GUID(在 xjn17svc.exe 字符串里看到的)。再 `CreateFileW(path, GENERIC_READ|GENERIC_WRITE, ...)` 拿 handle,后续就用这个 handle DeviceIoControl。

> 移植时:**你自己生成一个新的 GUID 即可**,用 `guidgen.exe`。

---

## 4. INF 模板

驱动安装走两个 INF。两个 INF 的完整内容直接从 `xjn17svc.exe` 的内嵌数据里抽出来(`strings -a` + 区间分析)。下面给的是 reWASD 实物 + 注释,**改 vendor / driver name / GUID 后即可直接用**。

### 4.1 `xjn17hlp.inf`(过滤驱动 + 虚拟根设备)

```ini
[Version]
Signature   = "$WINDOWS NT$"
Class       = System
ClassGUID   = {4d36e97d-e325-11ce-bfc1-08002be10318}
Provider    = %Vendor%
DriverVer   = 01/17/2026,3.47.0.0
CatalogFile = xjn17hlp.cat
PnpLockdown = 1                              ; ⚠ 必须设,防止 INF 在系统目录被改

[DestinationDirs]
DefaultDestDir = 12                          ; %WINDIR%\system32\drivers

[SourceDisksNames.amd64]
1 = %DisplayName%
[SourceDisksFiles.amd64]
xjn17hlp.sys = 1

[Manufacturer]
%Vendor% = Vendor, NTamd64

[Vendor.NTamd64]
%DisplayName%  = Install, root\xjn17hlp
%DisplayName2% = Install, BTHENUM\{4B6770D0-6471-43EA-B5A9-B39268F43B96}
; 第二行是 reWASD 注册的"虚拟蓝牙 HID 服务",用于 BT Xbox 控制器代理
; 移植时如不做 BT 代理可以删除

[Install.NTamd64]
CopyFiles = Drivers_Dir

[Install.NTamd64.HW]
AddReg = Security_Reg

[Security_Reg]
HKR,,DeviceCharacteristics,0x10001,0x0100    ; FILE_DEVICE_SECURE_OPEN
HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GR;;;BA)(A;;GR;;;WD)(A;;GR;;;RC)"
; SYSTEM 全权,Admin 只读,其余只读 — 这是 reWASD 给用户态服务/管理员能调 IOCTL 的最低 ACL

[Install.NTamd64.Services]
AddService = xjn17hlp, 2, Service_Inst       ; SPSVCINST_ASSOCSERVICE

[Service_Inst]
DisplayName    = %DisplayName%
Description    = %Description%
ServiceType    = 1                           ; SERVICE_KERNEL_DRIVER
StartType      = 0                           ; SERVICE_BOOT_START — 必要!filter 必须早于 device
ErrorControl   = 1                           ; SERVICE_ERROR_NORMAL
LoadOrderGroup = Filter                      ; ⚠ 关键,必须放在 Filter 组
ServiceBinary  = %12%\xjn17hlp.sys

[Drivers_Dir]
xjn17hlp.sys

[Strings]
DisplayName  = "System Driver"
DisplayName2 = "Virtual Bluetooth HID Device"
Description  = "System Driver"
Vendor       = "reWASD Team"
```

### 4.2 `xjn17flt.inf`(可选,只在你想搞 Xbox Wireless Adapter 时需要)

```ini
[Version]
Signature   = "$WINDOWS NT$"
Class       = NetService
ClassGUID   = {4D36E974-E325-11CE-BFC1-08002BE10318}
Provider    = %Vendor%
DriverVer   = 01/17/2026,3.47.0.0
CatalogFile = xjn17flt.cat
PnpLockdown = 1

[Manufacturer]
%Vendor% = Vendor, NTamd64

[Vendor.NTamd64]
%DisplayName% = Install, xjn17flt

[Install.NTamd64]
AddReg           = Ndi_Reg
Characteristics  = 0x40000                   ; NCF_LW_FILTER (NDIS Lightweight Filter)
NetCfgInstanceId = "{A35593B3-5EF1-4FFC-9D9F-5B2D89DAA0BE}"   ; 自己生成新 GUID

[Ndi_Reg]
HKR, Ndi,Service,,"xjn17hlp"
HKR, Ndi,CoServices,0x00010000,"xjn17hlp"
HKR, Ndi,HelpText,,%DisplayName%
HKR, Ndi,FilterClass,,custom
HKR, Ndi,FilterType,0x00010001,2             ; FILTER_TYPE_MODIFYING
HKR, Ndi\Interfaces,UpperRange,,"noupper"
HKR, Ndi\Interfaces,LowerRange,,"nolower"
HKR, Ndi\Interfaces, FilterMediaTypes,,"xboxwirelessinput"
HKR, Ndi,FilterRunType,0x00010001, 2         ; FILTER_RUN_TYPE_OPTIONAL
```

> 这个 INF 只在你需要拦截 Xbox Wireless Adapter 的无线流量时才装。普通有线/BT Xbox 控制器不需要。如果你的软件只支持有线、BT,可以省略整个 xjn17flt.inf。

---

## 5. ★ 用户态注入流程 — XInput 隐藏的关键 ★

这一节就是 HidHide **做不到、reWASD 多做了**的事情。代码全部来自 `xjn17svc.exe` 的反编译。

### 5.1 5 个目标设备类 GUID(逐字摘抄,**不要改**)

```c
// xjn17svc.exe 中 FUN_140007340 顺序调用 FUN_140005240 (class-level filter writer):
static const WCHAR* TargetClassGuids[] = {
    L"{05f5cfe2-4733-4950-a6bb-07aad01a3a84}",  // ★ Xbox 复合设备类(Wireless Adapter receiver、Xbox Series via XBOXGIP 的根接口) — 主战场
    L"{d61ca365-5af4-4486-998b-9db4734c6ca3}",  // ★ XnaComposite / xinputhid 类(Xbox 360 controller、Xbox Wireless Receiver 装的 controller 都走这个)
    L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",  //   HIDClass(给 DirectInput / RawInput 看的 HID 兼容接口)
    L"{4d36e96b-e325-11ce-bfc1-08002be10318}",  //   Keyboard
    L"{4d36e96f-e325-11ce-bfc1-08002be10318}",  //   Mouse
};
```

要让游戏的 XInput API 看不见 Xbox 物理控制器,**`{05f5cfe2-…}` 与 `{d61ca365-…}` 是必装的两个**。HIDClass 只覆盖 DirectInput / RawInput 路径,**不解决 XInput 问题**。这就是 HidHide 在 Xbox 控制器上失效的原因。

### 5.2 把驱动名写入设备类 LowerFilters

伪代码(对应 `FUN_140005240`,逻辑 1:1):

```c
// driver_service_name = L"xjn17hlp" (你驱动 INF 里 [Service_Inst] 的 service name)
LONG AddLowerFilter(LPCWSTR driver_service_name, LPCWSTR class_guid_str)
{
    // 构造 HKLM\System\CurrentControlSet\Control\Class\{xxx}
    WCHAR key_path[260];
    swprintf_s(key_path, L"System\\CurrentControlSet\\Control\\Class\\%ws", class_guid_str);

    HKEY hKey;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, NULL, 0,
                                KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
    if (s != ERROR_SUCCESS) return s;

    // 读 MULTI_SZ 现有 LowerFilters(可能不存在 ⇒ ERROR_FILE_NOT_FOUND)
    DWORD cb = 0;
    s = RegGetValueW(hKey, NULL, L"LowerFilters",
                     RRF_RT_REG_MULTI_SZ, NULL, NULL, &cb);

    WCHAR* buf = malloc(cb + (wcslen(driver_service_name) + 2) * sizeof(WCHAR));
    if (s == ERROR_SUCCESS) {
        RegGetValueW(hKey, NULL, L"LowerFilters",
                     RRF_RT_REG_MULTI_SZ, NULL, buf, &cb);
        // 检查是否已经存在
        for (WCHAR* p = buf; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, driver_service_name) == 0) { RegCloseKey(hKey); free(buf); return 0; }
    } else {
        // 没有现有 LowerFilters → 从空 multi_sz 开始
        cb = sizeof(WCHAR);
        buf[0] = 0;
    }
    // 在尾部 append 一项(注意 MULTI_SZ 双零结尾)
    size_t name_chars = wcslen(driver_service_name) + 1;
    memcpy((BYTE*)buf + cb - sizeof(WCHAR), driver_service_name, name_chars * sizeof(WCHAR));
    ((WCHAR*)((BYTE*)buf + cb - sizeof(WCHAR) + name_chars * sizeof(WCHAR)))[0] = 0;
    DWORD new_cb = cb + (DWORD)(name_chars * sizeof(WCHAR));

    s = RegSetValueExW(hKey, L"LowerFilters", 0, REG_MULTI_SZ, (BYTE*)buf, new_cb);
    RegCloseKey(hKey);
    free(buf);
    return s;
}

void InstallFilterIntoAllControllerClasses(void)
{
    for (int i = 0; i < _countof(TargetClassGuids); ++i)
        AddLowerFilter(L"xjn17hlp", TargetClassGuids[i]);
}
```

**安装时只做一次**,不需要每次开机。但是要做完之后让现有设备重新加载驱动栈 — 见 §5.4。

### 5.3 卸载时移除(对照 §5.2 反向操作)

```c
LONG RemoveLowerFilter(LPCWSTR svc, LPCWSTR class_guid_str);  // 把 svc 从 MULTI_SZ 里抠出来
```

`xjn17svc.exe` 中是用同一个函数 `FUN_140005240` 处理 add/remove 两个分支,以 `param_5` 输出 是否含/不含来决定写还是直接退出。卸载时遍历相同 5 个 class,移除条目即可。

### 5.4 让现有设备重启(把刚加进去的 LowerFilter 真的接上去)

LowerFilters 改完了只对**之后枚举**的设备生效。要让**当前已存在**的 Xbox 控制器重新走 PnP,需要对每个属于上述 class 的设备做 STOP + START。这正是 `FUN_1400719a0` 干的事:

```c
DWORD RestartDevice(HDEVINFO h, PSP_DEVINFO_DATA d)
{
    // 1) DICS_STOP
    SP_PROPCHANGE_PARAMS p = {0};
    p.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;  // 0x12
    p.StateChange = DICS_STOP;                                  // 5
    p.Scope       = DICS_FLAG_CONFIGSPECIFIC;                   // 2
    p.HwProfile   = 0;
    if (!SetupDiSetClassInstallParamsW(h, d, &p.ClassInstallHeader, sizeof(p))) return GetLastError();
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, d))                   return GetLastError();
    // 2) DICS_START
    p.StateChange = DICS_START;                                 // 4
    if (!SetupDiSetClassInstallParamsW(h, d, &p.ClassInstallHeader, sizeof(p))) return GetLastError();
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, d))                   return GetLastError();
    // 3) 看是否需要重启(SP_DEVINSTALL_PARAMS.Flags & (DI_NEEDREBOOT|DI_NEEDRESTART) = 0x180)
    SP_DEVINSTALL_PARAMS_W ip = { sizeof(ip) };
    SetupDiGetDeviceInstallParamsW(h, d, &ip);
    return (ip.Flags & 0x180) ? ERROR_RESTART_APPLICATION : 0;
}

void RestartAllAffectedDevices(void)
{
    for (int i = 0; i < _countof(TargetClassGuids); ++i) {
        GUID g; CLSIDFromString(TargetClassGuids[i], &g);
        HDEVINFO h = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT);
        SP_DEVINFO_DATA d = { sizeof(d) };
        for (DWORD idx = 0; SetupDiEnumDeviceInfo(h, idx, &d); ++idx) {
            RestartDevice(h, &d);
        }
        SetupDiDestroyDeviceInfoList(h);
    }
}
```

> **执行顺序**:`Add LowerFilter → Restart Devices`。等所有设备重新启动后,你的 filter driver 才真的进栈。
>
> 此时 **所有** 现在以及之后插入的 Xbox 控制器都会走过你的 filter 一遍。但默认是 **透明** 的(filter 啥都不干,直接 IoSkipCurrentIrpStackLocation 转发)。隐藏开关由运行时 IOCTL 控制(§3.2)。

### 5.5 安装/卸载驱动包(用 SetupAPI 而不是 sc.exe)

```c
// 装 INF + 装 .sys + 触发受影响 device 安装
BOOL Inst1 = SetupCopyOEMInfW(L"C:\\Path\\xjn17hlp.inf",
                              NULL, SPOST_PATH, 0, NULL, 0, NULL, NULL);
// 装一个虚拟根设备(承载 vJoy/虚拟手柄 child PDO),HardwareID = L"root\\xjn17hlp"
// — 完全照搬 FUN_140006bc0
HDEVINFO h = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SYSTEM, NULL);
SP_DEVINFO_DATA d = { sizeof(d) };
SetupDiCreateDeviceInfoW(h, L"<friendly>", &GUID_DEVCLASS_SYSTEM, NULL, NULL,
                        DICD_GENERATE_ID, &d);
SetupDiSetDeviceRegistryPropertyW(h, &d, SPDRP_HARDWAREID,
                                  (BYTE*)L"root\\xjn17hlp\0\0",
                                  sizeof(L"root\\xjn17hlp\0\0"));
SetupDiCallClassInstaller(DIF_REGISTERDEVICE /*0x19*/, h, &d);
BOOL reboot = FALSE;
UpdateDriverForPlugAndPlayDevicesW(NULL, L"root\\xjn17hlp",
                                   L"C:\\Path\\xjn17hlp.inf",
                                   INSTALLFLAG_FORCE, &reboot);
```

要点:
- **`StartType=0` (BOOT_START) 决定了驱动是开机第一批加载** — 必须这样,LowerFilter 必须在 device 枚举前就在内核里。
- 装 root\xjn17hlp 这一步是 reWASD 用来挂虚拟手柄的"母板"。**如果你不打算自带虚拟手柄(直接用 ViGEmBus),可以跳过这步**,但保留 `Service_Inst`(只装 filter)。

---

## 6. 运行时:UI → 服务 → 驱动 的协议

reWASD 这一层是 native-pipe 协议:

### 6.1 Named Pipe

```
pipe name:    \\.\pipe\{9AB72C00-54D9-4A0F-A236-C31895AB0B20}
flow:         message-mode, duplex, server in xjn17svc.exe,
              client in reWASDUI.exe / reWASDCommandLine.exe (.NET)
framing:      二进制 struct REWASD_REQUEST_HEADER {
                 uint32_t Size;       // 整个 request 字节数(含 header)
                 uint32_t Command;    // 0..28 见下表
              };
```

UI 中 `XBServiceCommunicator.cs` 的几乎所有 method 都是构造对应的 `REWASD_*_REQUEST struct`(C# 结构 byte-equivalent),`Header.Size = sizeof(struct)`,`Header.Command = <号>`,然后 TransactNamedPipe。

### 6.2 命令号(直接搬,你也可以重用同名号或者自定义)

```
REWASD_COMMAND_GET_VERSION                = 0
REWASD_COMMAND_GET_PROFILE_INFO           = 1
REWASD_COMMAND_GET_PROFILE                = 2
REWASD_COMMAND_ADD_PROFILE                = 3
REWASD_COMMAND_DEL_PROFILES               = 4
REWASD_COMMAND_GET_PROFILE_STATE          = 5
REWASD_COMMAND_SET_PROFILE_STATE          = 6
REWASD_COMMAND_GET_CONTROLLER_LIST        = 7
REWASD_COMMAND_GET_CONTROLLER_STATE       = 8
REWASD_COMMAND_SET_CONTROLLER_STATE       = 9
REWASD_COMMAND_SET_CONTROLLER_OPTIONS     = 10   ← profile apply
REWASD_COMMAND_SET_VIBRATION              = 11
REWASD_COMMAND_SET_LED                    = 12
REWASD_COMMAND_SET_ADAPTIVE_TRIGGERS      = 13
REWASD_COMMAND_LICENSE_CONTROL            = 14
REWASD_COMMAND_SET_BLUETOOTH_COD          = 15
REWASD_COMMAND_GET_RADIO_INFO             = 16
REWASD_COMMAND_ADD_RADIO                  = 17
REWASD_COMMAND_DEL_RADIO                  = 18
REWASD_COMMAND_BLUETOOTH_SCAN             = 19
REWASD_COMMAND_PAIR_CONTROLLER            = 20
REWASD_COMMAND_UNPAIR_CONTROLLER          = 21
REWASD_COMMAND_ADD_ENGINE_CONTROLLER      = 22
REWASD_COMMAND_DEL_ENGINE_CONTROLLER      = 23
REWASD_COMMAND_START_BROADCAST            = 24
REWASD_COMMAND_STOP_BROADCAST             = 25
REWASD_COMMAND_STOP_SERVICE               = 26
REWASD_COMMAND_SET_PREFERENCES            = 27
REWASD_COMMAND_DEL_OLD_DRIVERS            = 28
```

### 6.3 ★ 关键 profile 字段(决定 hide 触发) ★

`REWASD_CONTROLLER_PROFILE_COMMON`(`#pragma pack(1)`)中有两个 mask:

```c
struct REWASD_CONTROLLER_PROFILE_COMMON {
    GUID     Guid;
    WCHAR    Description[144];
    uint32_t Type[15];                  // 每个槽的物理 controller type
    uint32_t VirtualType;
    uint64_t Id[15];                    // 每个槽对应物理设备 instance id
    uint64_t GuiContext;
    uint64_t GuiContextExtended;
    uint64_t AuthControllerId;
    uint64_t AuthControllerAddress;
    uint64_t LocalBthAddr;
    uint64_t RemoteBthAddr;
    uint8_t  VirtualFlags;
    uint8_t  Reserved[7];
    uint16_t WriteControllersMask;
    uint16_t PaddleLockMask;
    uint16_t HiddenControllersMask;     // ★ bit i = 1 ⇒ 槽 i 的 controller 走 hide IOCTL
    uint16_t ExclusiveControllersMask;  // ★ bit i = 1 ⇒ 槽 i 的 controller 走 exclusive-capture 路径
    uint32_t GimxPortFlags;
    uint32_t GimxBaudRate;
    WCHAR    GimxPortName[16];
    uint16_t SwitchProLeftStickDeadzone;
    uint16_t SwitchProRightStickDeadzone;
    uint8_t  SwitchProColors[12];
};
```

UI 中 `bindings.HidePhysicalGamepad = true` 时,`MacroCompiler.FillHiddenControllersMask` 把对应 bit 设进去。然后整个 profile 通过 `REWASD_COMMAND_ADD_PROFILE = 3` 发到服务。服务收到后,**为每个 mask=1 的槽,把对应 device 的 driver handle 上调一次 `DeviceIoControl(handle, 0x2AE9E0, &(uint8_t)0, 1, NULL, 0, &ret, NULL)`**。

`ExclusiveControllersMask` 是给 Windows 10+ 支持 ExclusiveAccess 的设备(SimpleDeviceInfo.Properties bit `0x20000000`)走"独占采集"路径(IGameController::AcquireExclusive),目前只针对 Xbox Elite / Xbox One / Xbox Series。该路径与 hide 互斥(同设备只走一种)。对你的实现来说先不用关心,把所有 controller 走 HiddenControllersMask 路径就行,**对 XInput 也照样能 hide**,因为有 §5 的 class filter 兜底。

---

## 7. 自制软件的最小可移植方案(精简版)

如果你不想抄全部 reWASD,下面是**只为 hide XInput** 的最小集合:

### 7.1 一个 KMDF lower filter driver(几百行 C)

```c
// HideFilter.c —— 你的最简 lower filter
#include <ntddk.h>
#include <wdf.h>
#include <hidclass.h>  // for HidNotifyPresence prototype reference

typedef VOID (*PFN_HID_NOTIFY_PRESENCE)(PDEVICE_OBJECT, BOOLEAN);
static PFN_HID_NOTIFY_PRESENCE g_HidNotifyPresence = NULL;

typedef struct _DEV_CTX {
    WDFDEVICE Self;
    PDEVICE_OBJECT Pdo;          // 物理设备 PDO
    LONG  HideRefCount;
    BOOLEAN Hidden;
} DEV_CTX, *PDEV_CTX;
WDF_DECLARE_CONTEXT_TYPE(DEV_CTX);

// 1) 解析 HIDCLASS!HidNotifyPresence 一次。
//    最稳妥:遍历 PsLoadedModuleList 取 HIDCLASS.SYS 基址,自己走 PE EAT 解析。
//    见 reWASD 的 FUN_140052660。下面给最简单的 stub。
NTSTATUS ResolveHidNotifyPresence(VOID)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"HidNotifyPresence");
    g_HidNotifyPresence = (PFN_HID_NOTIFY_PRESENCE)MmGetSystemRoutineAddress(&name);
    // ⚠ MmGetSystemRoutineAddress 在某些 Win10 build 上只搜 ntoskrnl/hal。
    //    如果返回 NULL,fallback:走 PsLoadedModuleList,在 HIDCLASS.SYS 的 EAT 里找。
    return g_HidNotifyPresence ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

// 2) EvtDriverDeviceAdd:把自己 attach 进 stack
NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WdfFdoInitSetFilter(init);                           // ★ 标记为 filter
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEV_CTX);
    WDFDEVICE dev;
    NTSTATUS s = WdfDeviceCreate(&init, &attr, &dev);
    if (!NT_SUCCESS(s)) return s;

    PDEV_CTX c = WdfObjectGet_DEV_CTX(dev);
    c->Self = dev;
    c->Pdo  = WdfDeviceWdmGetPhysicalDevice(dev);

    // I/O queue,只为接收 IOCTL
    WDF_IO_QUEUE_CONFIG qc;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoDeviceControl = EvtIoDeviceControl;
    WdfIoQueueCreate(dev, &qc, WDF_NO_OBJECT_ATTRIBUTES, NULL);

    // 暴露一个 device interface,让 user-mode service 找到自己
    static const GUID MY_IFACE = { /* 你的 guidgen 出来的 GUID */ };
    WdfDeviceCreateDeviceInterface(dev, &MY_IFACE, NULL);
    return STATUS_SUCCESS;
}

#define IOCTL_HIDE_TOGGLE  CTL_CODE(0x002A, 0xA78, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA)
// = 0x2AE9E0 (沿用 reWASD 编号即可,也可以换)

VOID EvtIoDeviceControl(WDFQUEUE q, WDFREQUEST req, size_t obuf, size_t ibuf, ULONG code)
{
    UNREFERENCED_PARAMETER(obuf);
    NTSTATUS s = STATUS_INVALID_DEVICE_REQUEST;
    if (code == IOCTL_HIDE_TOGGLE && ibuf >= 1) {
        BOOLEAN* in = NULL;
        WdfRequestRetrieveInputBuffer(req, 1, (PVOID*)&in, NULL);
        BOOLEAN hide = (*in == 0);                  // 0 ⇒ hide, !=0 ⇒ unhide
        WDFDEVICE dev = WdfIoQueueGetDevice(q);
        PDEV_CTX c = WdfObjectGet_DEV_CTX(dev);
        if (hide) {
            if (InterlockedIncrement(&c->HideRefCount) == 1 && g_HidNotifyPresence) {
                g_HidNotifyPresence(c->Pdo, FALSE);
                c->Hidden = TRUE;
            }
        } else {
            if (InterlockedDecrement(&c->HideRefCount) == 0 && g_HidNotifyPresence && c->Hidden) {
                g_HidNotifyPresence(c->Pdo, TRUE);
                c->Hidden = FALSE;
            }
        }
        s = STATUS_SUCCESS;
    }
    WdfRequestComplete(req, s);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regp)
{
    ResolveHidNotifyPresence();
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(drv, regp, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
```

### 7.2 INF + 安装(直接抄 §4.1)

### 7.3 用户态服务(§5 的 5 个 GUID + LowerFilters 写入 + 设备 restart)

### 7.4 运行时(§6)

应用要 hide 某个 Xbox controller 时:
1. `SetupDiGetClassDevsW(&MY_IFACE, ...)`,枚举出 filter driver 在该设备实例上暴露的 interface
2. `CreateFileW(...)` 拿 handle(此 handle 对应的 device context 里的 `Pdo` 正好是要 hide 的物理 controller PDO)
3. `DeviceIoControl(handle, IOCTL_HIDE_TOGGLE, &(BYTE)0, 1, NULL, 0, &ret, NULL)`

至于"给应用用什么作为虚拟手柄输出" — 直接复用 [ViGEmBus](https://github.com/nefarius/ViGEmBus) + [ViGEm.NET](https://github.com/nefarius/ViGEm.NET) 即可,**不要自己再写一遍**。reWASD 自己写了 root\xjn17hlp 虚拟总线只是因为 ViGEm 早期没有,在自己的方案里可以省。

---

## 8. 为什么 HidHide 不工作而本方案能用 — 原理对比

| 项 | HidHide | reWASD 方案 |
|---|---|---|
| 过滤位置 | 只装在 HIDClass `{745a17a0-…}` 的 UpperFilters | 装在 5 个 class 的 LowerFilters,**其中 `{05f5cfe2-…}` 和 `{d61ca365-…}` 是 XInput 路径** |
| 触达 XInput 设备? | ❌ XInput 设备走 xusb22 / xboxgip,完全不进 HIDClass | ✅ XnaComposite 类下挂的就是 Xbox 360 controller / Xbox Wireless Receiver;XboxComposite 类下挂 Xbox Wireless Adapter |
| 隐藏机制 | `HidNotifyPresence(PDO, FALSE)`,只对 HIDClass 设备有效 | 同样调 `HidNotifyPresence`,但 PDO 是来自 XInput 设备类的 PDO(filter 直接挂在那里) |
| 对 XInput API 的可见性 | 仍然能看到设备 | **看不到**(PnP manager 把 PDO 标记为 absent,xusb22/xboxgip 不会被 XInput API 枚举到) |

`HidNotifyPresence` 本质是请求 PnP 管理器对该 PDO 发出 invalidate device-relations,触发 device remove。这跟"是不是 HID 设备"无关 — **只要你的 filter 拿到了 PDO,就能让 PnP 把它从所在的 bus 上"摘掉"**。reWASD 的洞察是:把 filter 直接装到 XInput 设备类的 stack 里,让 filter 拿到 Xbox controller 的 PDO,然后照样能 hide。

---

## 9. 风险与移植注意事项

1. **驱动签名**:Win10/11 启用了 driver signature enforcement,你的 .sys 必须有 EV 证书签名 + WHQL(或者要求用户开 testsigning)。reWASD 的 cat 文件由 "Microsoft Windows Hardware Compatibility Publisher" 签发,即走了 WHQL。
2. **HVCI / Memory Integrity**:挂 lower filter 没问题,但你的代码必须满足 HVCI 兼容(不能用 RWX 内存、不能 patch 内核)。reWASD 的驱动完全符合(全静态 IRP 拦截 + 文档化 API)。
3. **MmGetSystemRoutineAddress 解析 HidNotifyPresence**:有些早期 Win10 build 这个 API 不能拿非 ntoskrnl 的导出。reWASD 实现了自己的 "遍历 `PsLoadedModuleList` → 解析 HIDCLASS.SYS PE → EAT 找名字" 的回退(见 `xjn17hlp.sys` 的 FUN_140052660)。**移植时务必照样做回退**,否则在某些 Windows 版本上 hide 不工作。
4. **改 LowerFilters 的副作用**:HID/Keyboard/Mouse/XboxComposite/XnaComposite 是系统级 class,操作不当会让用户系统进不去桌面。一定要在写之前备份 MULTI_SZ,卸载时严格按 list 移除,而不是清空。
5. **修改完 LowerFilters 后必须 STOP+START 影响到的设备**,否则你的 filter driver 不会出现在 stack 里(对应 §5.4)。
6. **不要试图用 `SetupDiCallClassInstaller(DICS_DISABLE)`** —— 那会在设备管理器留下感叹号,用户体验差,reWASD 故意没用这条路。
7. **对 Xbox One Wireless Adapter** 用 `xjn17flt.sys` (NDIS LWF) 才能 hide,因为 Wireless Adapter 没有 PnP 子设备走 HIDClass,所有数据走 NDIS `xboxwirelessinput` 媒体类型。
8. **PnpLockdown=1** 必须设;否则 INF 在系统里会被任何人改。
9. **测试** 建议在 Hyper-V 或物理测试机上,挂 `windbg -k` kernel debugger。

---

## 10. 附录

### A. 解包脚本(本仓库 /workspaces/code/rc1/extract/dump_resx.py)

```python
import base64, sys, xml.etree.ElementTree as ET
from pathlib import Path
src, out = Path(sys.argv[1]), Path(sys.argv[2]); out.mkdir(exist_ok=True)
cur_name, cur_type, chunks = None, None, []
for ev, e in ET.iterparse(str(src), events=("start","end")):
    if ev == "start" and e.tag == "data":
        cur_name, cur_type, chunks = e.attrib.get("name"), e.attrib.get("type",""), []
    elif ev == "end":
        if e.tag == "value" and cur_name and e.text:
            chunks.append(e.text)
        elif e.tag == "data":
            if "System.Byte[]" in (cur_type or ""):
                (out/(cur_name+".bin")).write_bytes(base64.b64decode("".join(chunks)))
            cur_name = None; e.clear()
```

调用:
```bash
ilspycmd -p -o decompiled reWASD933-11029.exe
python3 dump_resx.py decompiled/RewasdWpfInstaller.Properties.Resources.resx blobs/
7z x blobs/binx64.bin -obinx64/        # → xjn17svc.exe / *.dll
7z x blobs/commondlls.bin -ocommon/    # → Engine/Common/Protocol 等
```

### B. 反编译/分析工具

- `pyghidra` (Ghidra 12) — headless,~10 min 全反编译 4000 函数
- `ilspycmd` 8.x — .NET 反编译
- `monodis --presources` — .NET 资源 enumerate

### C. 关键文件偏移摘要

| 项 | 位置 | 偏移 |
|---|---|---|
| Hide IOCTL dispatcher | xjn17hlp.sys | `FUN_14003b200` case 6 |
| Hide refcount + HidNotifyPresence | xjn17hlp.sys | `FUN_140043a20` |
| Class GUID list (5 个) | xjn17svc.exe | `FUN_140007340` |
| Class-level LowerFilters writer | xjn17svc.exe | `FUN_140005240` |
| Device-level LowerFilter writer | xjn17svc.exe | `FUN_140005b60` |
| Restart device (STOP+START) | xjn17svc.exe | `FUN_1400719a0` |
| INF install (DIF_REGISTERDEVICE + UpdateDriver) | xjn17svc.exe | `FUN_140006bc0` |
| Hide IOCTL caller (run-time) | xjn17svc.exe | `FUN_140003010` / `FUN_14004cbb0` / `FUN_1400172d0` |

### D. 用到的 GUID 备查

```
{4d36e97d-e325-11ce-bfc1-08002be10318}  GUID_DEVCLASS_SYSTEM (你的驱动装到这个类)
{05f5cfe2-4733-4950-a6bb-07aad01a3a84}  XboxComposite          ← ★ XInput 必装
{d61ca365-5af4-4486-998b-9db4734c6ca3}  XnaComposite           ← ★ XInput 必装
{745a17a0-74d3-11d0-b6fe-00a0c90f57da}  HIDClass
{4d36e96b-e325-11ce-bfc1-08002be10318}  Keyboard
{4d36e96f-e325-11ce-bfc1-08002be10318}  Mouse
{4D36E974-E325-11CE-BFC1-08002BE10318}  NetService (NDIS LWF 的 class)
{8999D406-CC96-496B-B2EE-1147253985E3}  reWASD 自定义 driver interface(自己换)
{9AB72C00-54D9-4A0F-A236-C31895AB0B20}  reWASD 自定义 Named Pipe ID(自己换)
{A35593B3-5EF1-4FFC-9D9F-5B2D89DAA0BE}  reWASD NDIS LWF NetCfgInstanceId(自己换)
{4B6770D0-6471-43EA-B5A9-B39268F43B96}  reWASD 自定义 BTHENUM service(自己换)
```

---

## 11. 移植 Checklist

- [ ] 用 guidgen 生成 4 个新 GUID(driver interface、pipe、NetCfgInstanceId、BTHENUM 服务)
- [ ] 写 KMDF lower filter 驱动(§7.1) — 记得 `WdfFdoInitSetFilter`、解析 HidNotifyPresence
- [ ] 写 INF(§4.1):`StartType=0`,`LoadOrderGroup=Filter`,`PnpLockdown=1`
- [ ] EV/WHQL 签名(否则 Win10+ 拒载)
- [ ] 写 user-mode service:
  - [ ] `SetupCopyOEMInfW` 装 INF
  - [ ] `SetupDiCreateDeviceInfoW + DIF_REGISTERDEVICE + UpdateDriverForPlugAndPlayDevicesW` 安装 root\<yourname> 虚拟设备
  - [ ] 遍历 §5.1 的 5 个 GUID,调 §5.2 `AddLowerFilter`
  - [ ] 调 §5.4 `RestartAllAffectedDevices`(这是让现有 Xbox 控制器走过 filter 的关键)
- [ ] 写应用端:`SetupDiGetClassDevs(&MY_IFACE)` → `CreateFile` → `DeviceIoControl(0x2AE9E0, &(BYTE)0/1, 1)`
- [ ] 测试:把 Xbox One controller 接上,在你的程序里点 Hide,xinput-test 应该看不到任何按键。Unhide 应该立刻恢复。
- [ ] 卸载时:`RemoveLowerFilter`(从 MULTI_SZ 抠掉)+ `RestartAllAffectedDevices` + `SetupUninstallOEMInfW`。
- [ ] 错误处理:用户拔掉控制器时,filter driver 的 EvtDeviceRemove 要把所有 hidden refcount 清零;否则下一次插上的控制器仍然是 hidden(用户感受到的是"我的 Xbox 手柄突然不工作了")。

---

报告完。报告中的所有声明都可以在 `/workspaces/code/rc1/ghidra_proj/out/` 与 `/workspaces/code/rc1/extract/` 中复查到具体反编译函数与字符串证据。
