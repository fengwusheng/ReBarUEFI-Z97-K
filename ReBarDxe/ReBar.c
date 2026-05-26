/*
Copyright (c) 2022-2023 xCuri0 <zkqri0@gmail.com>
SPDX-License-Identifier: MIT
*/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleTextInEx.h>
// #include <Library/DxeServicesTableLib.h>
#include <Protocol/PciRootBridgeIo.h>
#include <IndustryStandard/Pci22.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include "include/pciRegs.h"
#include "include/PciHostBridgeResourceAllocation.h"

#ifdef _MSC_VER
#pragma warning(disable:28251)
#include <intrin.h>
#pragma warning(default:28251)
#endif

#define PCI_POSSIBLE_ERROR(val) ((val) == 0xffffffff)

// for quirk
#define PCI_VENDOR_ID_ATI 0x1002

// if system time is before this year then CMOS reset will be detected and rebar will be disabled.
#define BUILD_YEAR 2023

// a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce
static GUID reBarStateGuid = { 0xa3c5b77a, 0xc88f, 0x4a93, {0xbf, 0x1c, 0x4a, 0x92, 0xa3, 0x2c, 0x65, 0xce}};

// 0: disabled
// >0: maximum BAR size (2^x) set to value. UINT8_MAX for unlimited
static UINT8 reBarState = 0;

static EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *pciResAlloc;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *pciRootBridgeIo;

static EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL_PREPROCESS_CONTROLLER o_PreprocessController;


// 检测 Ctrl 键是否被按下
BOOLEAN IsCtrlKeyPressed() {
	// 遵从 EDK2 规范：不赋初始值，默认就是 FALSE/零值
	static BOOLEAN isChecked;// = FALSE; 
	static BOOLEAN isPressed;// = FALSE;

	if (isPressed) return TRUE;
	
    EFI_STATUS Status;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *TxtInEx = NULL;
    EFI_KEY_DATA KeyData;
	UINTN RetryCount = isChecked ? 1 : 50;

	// 前置防御：在检测前，强行刷新重置标准输入流
	// 这一步是尝试踢醒那些还没完全准备好的键盘固件（比如 PS/2 或刚通电的 USB）
	if (gST->ConIn != NULL) {
		gST->ConIn->Reset (gST->ConIn, FALSE);
		gBS->Stall (100000); // 挂起 100,000 微秒（也就是 100 毫秒），给 USB 硬件一点反应时间
	}
	
	// 1. 通过 BootServices 获取系统的扩展输入协议
    Status = gBS->LocateProtocol(
        &gEfiSimpleTextInputExProtocolGuid, 
        NULL, 
        (VOID **)&TxtInEx
    );

    if (EFI_ERROR(Status) || TxtInEx == NULL) {
        return FALSE; // 获取协议失败，保守起见返回 FALSE
    }

    // 2. 读取当前的键盘状态（KeyState），500ms 黄金窗口蹲守
	for (; RetryCount > 0; RetryCount--) {
		Status = TxtInEx->ReadKeyStrokeEx (TxtInEx, &KeyData);
		if (Status == EFI_SUCCESS || Status == EFI_NOT_READY) {
			// 3. 判定判定：检测虚拟控制键状态，包括左 Ctrl 或右 Ctrl
			if ((KeyData.KeyState.KeyShiftState & 0x80000000) != 0) { // EFI_SHIFT_STATE_VALID 0x80000000
                if ((KeyData.KeyState.KeyShiftState & (0x00000001 | 0x00000002)) != 0) { // EFI_LEFT_CONTROL_PRESSED 0x00000001   EFI_RIGHT_CONTROL_PRESSED 0x00000002
                    isPressed = TRUE; 
                    break;
                }
            }
		}
		gBS->Stall (10000); // 每次死等 10 毫秒
	}

	isChecked = TRUE;
    return isPressed;
}

// 帮我们在宽字符里找子串的辅助函数
CHAR16* StrStr16(IN CHAR16* Str, IN CHAR16* Search) {
    UINTN Len = StrLen(Search);
    if (Len == 0) return Str;
    while (*Str) {
        if (StrnCmp(Str, Search, Len) == 0) return Str;
        Str++;
    }
    return NULL;
}


// find last set bit and return the index of it
INTN fls(UINT32 x)
{
    UINT32 r;

    #ifdef _MSC_VER
    _BitScanReverse64(&r, x);
    #else
    // taken from linux x86 bitops.h
    asm("bsrl %1,%0"
	    : "=r" (r)
	    : "rm" (x), "0" (-1));
    #endif

    return r;
}

UINT64 pciAddrOffset(UINTN pciAddress, INTN offset)
{
    UINTN reg = (pciAddress & 0xffffffff00000000) >> 32;
    UINTN bus = (pciAddress & 0xff000000) >> 24;
    UINTN dev = (pciAddress & 0xff0000) >> 16;
    UINTN func = (pciAddress & 0xff00) >> 8;

    return EFI_PCI_ADDRESS(bus, dev, func, ((INT64)reg + offset));
}

// created these functions to make it easy to read as we are adapting alot of code from Linux
EFI_STATUS pciReadConfigDword(UINTN pciAddress, INTN pos, UINT32 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint32, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigDword(UINTN pciAddress, INTN pos, UINT32 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint32, pciAddrOffset(pciAddress, pos), 1, buf);
}
EFI_STATUS pciReadConfigWord(UINTN pciAddress, INTN pos, UINT16 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint16, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigWord(UINTN pciAddress, INTN pos, UINT16 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint16, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciReadConfigByte(UINTN pciAddress, INTN pos, UINT8 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint8, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigByte(UINTN pciAddress, INTN pos, UINT8 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint8, pciAddrOffset(pciAddress, pos), 1, buf);
}

// adapted from linux pci_find_ext_capability
UINT16 pciFindExtCapability(UINTN pciAddress, INTN cap)
{
    INTN ttl;
    UINT32 header;
    UINT16 pos = PCI_CFG_SPACE_SIZE;

    /* minimum 8 bytes per capability */
    ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

    if (EFI_ERROR(pciReadConfigDword(pciAddress, pos, &header)))
        return 0;
    /*
     * If we have no capabilities, this is indicated by cap ID,
     * cap version and next pointer all being 0. Or it could also be all FF
     */
    if (header == 0 || PCI_POSSIBLE_ERROR(header))
        return 0;

    while (ttl-- > 0)
    {
        if (PCI_EXT_CAP_ID(header) == cap && pos != 0)
            return pos;

        pos = PCI_EXT_CAP_NEXT(header);
        if (pos < PCI_CFG_SPACE_SIZE)
            break;

        if (EFI_ERROR(pciReadConfigDword(pciAddress, pos, &header)))
            break;
    }
    return 0;
}

INTN pciRebarFindPos(UINTN pciAddress, INTN pos, UINT8 bar)
{
    UINTN nbars, i;
    UINT32 ctrl;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    nbars = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >>
            PCI_REBAR_CTRL_NBAR_SHIFT;

    for (i = 0; i < nbars; i++, pos += 8)
    {
        UINTN bar_idx;

        pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
        bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
        if (bar_idx == bar)
            return pos;
    }
    return -1;
}

UINT32 pciRebarGetPossibleSizes(UINTN pciAddress, UINTN epos, UINT16 vid, UINT16 did, UINT8 bar)
{
    INTN pos;
    UINT32 cap;

    pos = pciRebarFindPos(pciAddress, (INTN)epos, bar);
    if (pos < 0)
        return 0;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CAP, &cap);
    cap &= PCI_REBAR_CAP_SIZES;

    /* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
    if (vid == PCI_VENDOR_ID_ATI && did == 0x731f &&
        bar == 0 && cap == 0x7000)
        cap = 0x3f000;

    return cap >> 4;
}

INTN pciRebarSetSize(UINTN pciAddress, UINTN epos, UINT8 bar, UINT8 size)
{
    INTN pos;
    UINT32 ctrl;

    pos = pciRebarFindPos(pciAddress, (INTN)epos, bar);
    if (pos < 0)
        return pos;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    ctrl &= (UINT32)~PCI_REBAR_CTRL_BAR_SIZE;
    ctrl |= (UINT32)size << PCI_REBAR_CTRL_BAR_SHIFT;

    pciWriteConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    return 0;
}

VOID reBarSetupDevice(EFI_HANDLE handle, EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS addrInfo)
{
    UINTN epos;
    UINT16 vid, did;
    UINTN pciAddress;

    gBS->HandleProtocol(handle, &gEfiPciRootBridgeIoProtocolGuid, (void **)&pciRootBridgeIo);

    pciAddress = EFI_PCI_ADDRESS(addrInfo.Bus, addrInfo.Device, addrInfo.Function, 0);
    pciReadConfigWord(pciAddress, 0, &vid);
    pciReadConfigWord(pciAddress, 2, &did);

    if (vid == 0xFFFF)
        return;

	// added
	if (IsCtrlKeyPressed()) return;

    DEBUG((DEBUG_INFO, "ReBarDXE: Device vid:%x did:%x\n", vid, did));

	// added
	UINT8 actualReBarState = reBarState; // 引入一个局部变量承接全局状态
	// 【核心注入：在这里做厂商分流！】
    if (vid == 0x10DE) {
        // NVIDIA (V100): 可以给 32，满足它全映射死命令
        //actualReBarState = reBarState >= 32 ? 32 : reBarState;
		// N卡计算卡开始不开都没问题，后面 TCC 大量内存去申请也可以用
        actualReBarState = reBarState >= 32 ? 32 : reBarState >= 12 ? reBarState : 0; // 默认 4G 开关给的 9 会变成 0
    } else if (vid == 0x1002) {
        // AMD (6600XT): 只给 1GB (10) 或者是 512MB (9)，不撑爆主板
        //actualReBarState = reBarState >= 10 ? 10 : reBarState; 
		// 其实报错声音一长三短叫完后黑屏等待还是能进系统的（2GB即11很特殊，BIOS以为能低位能分配就不叫了，但结果花屏）
		actualReBarState = reBarState >= 32 ? 32 : reBarState;
    } else {
		// 其他不允许ReBar
        actualReBarState = 0;
    }
	
    epos = pciFindExtCapability(pciAddress, PCI_EXT_CAP_ID_REBAR);
    if (epos)
    {
        for (UINT8 bar = 0; bar < 6; bar++)
        {
            UINT32 rBarS = pciRebarGetPossibleSizes(pciAddress, epos, vid, did, bar);
            if (!rBarS)
                continue;
            // start with size from fls
            for (UINT8 n = MIN((UINT8)fls(rBarS), actualReBarState); n > 0; n--) { // added reBarState -> actualReBarState
                // check if size is supported
                if (rBarS & (1 << n)) {
                    pciRebarSetSize(pciAddress, epos, bar, n);
                    break;
                }
            }
        }
    }
}

EFI_STATUS EFIAPI PreprocessControllerOverride (
  IN  EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL  *This,
  IN  EFI_HANDLE                                        RootBridgeHandle,
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS       PciAddress,
  IN  EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE      Phase
  )
{
    // call the original method
    EFI_STATUS status = o_PreprocessController(This, RootBridgeHandle, PciAddress, Phase);

    DEBUG((DEBUG_INFO, "ReBarDXE: Hooked PreprocessController called %d\n", Phase));

    // EDK2 PciBusDxe setups Resizable BAR twice so we will do same
    if (Phase <= EfiPciBeforeResourceCollection) {
        reBarSetupDevice(RootBridgeHandle, PciAddress);
    }

    return status;
}

VOID pciHostBridgeResourceAllocationProtocolHook()
{
    EFI_STATUS status;
    UINTN handleCount;
    EFI_HANDLE *handleBuffer;

    status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiPciHostBridgeResourceAllocationProtocolGuid,
        NULL,
        &handleCount,
        &handleBuffer);

    if (EFI_ERROR(status))
        goto free;

    status = gBS->OpenProtocol(
        handleBuffer[0],
        &gEfiPciHostBridgeResourceAllocationProtocolGuid,
        (VOID **)&pciResAlloc,
        gImageHandle,
        NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (EFI_ERROR(status))
        goto free;

    DEBUG((DEBUG_INFO, "ReBarDXE: Hooking EfiPciHostBridgeResourceAllocationProtocol->PreprocessController\n"));

    // Hook PreprocessController
    o_PreprocessController = pciResAlloc->PreprocessController;
    pciResAlloc->PreprocessController = &PreprocessControllerOverride;

free:
    FreePool(handleBuffer);
}


EFI_STATUS EFIAPI rebarInit(
    IN EFI_HANDLE imageHandle,
    IN EFI_SYSTEM_TABLE *systemTable)
{
    UINTN bufferSize = 0;
    EFI_STATUS status;
	
	DEBUG((DEBUG_INFO, "ReBarDXE: Boot Option Scanner Loaded.\n"));

	// added
	reBarState = 0;
	if (IsCtrlKeyPressed()) return EFI_SUCCESS;

	CHAR8 StrConfig[] = "ReBar_CFG:Above4GDecodingOffset=0x0001|SetupReBarStateOffset=0xFFFF|EnableUEFIBootMenuDetectReBarState=1|EnableDefaultReBarState=0";
	UINT32 Above4GOffset   = (UINT32)AsciiStrHexToUintn(&StrConfig[32]);
	UINT32 SetupStateOffset = (UINT32)AsciiStrHexToUintn(&StrConfig[61]);
	UINT32 BootMenuDetect   = (UINT32)(StrConfig[103] - '0');
	UINT32 DefaultState     = (UINT32)(StrConfig[129] - '0');

	// added
	if (Above4GOffset < 0xFFFF || SetupStateOffset < 0xFFFF) {
	    UINT8 *setupBuffer = NULL;
	    EFI_GUID mSetupGuid = { 0xEC87D643, 0xEBA4, 0x4BB5, { 0xA1, 0xE5, 0x3F, 0x3E, 0x36, 0xB2, 0x0D, 0xA9 } };
	    // 第一步：先试探性读取，拿到 Setup 结构体的总物理大小（公摊面积）
	    bufferSize = 0;
        status = gRT->GetVariable(L"Setup", &mSetupGuid, NULL, &bufferSize, NULL);
        if (status == EFI_BUFFER_TOO_SMALL) {
            // 第二步：动态开辟内存，准备把整个 Setup 结构体打包拉出来
            gBS->AllocatePool(EfiBootServicesData, bufferSize, (VOID**)&setupBuffer);
            status = gRT->GetVariable(L"Setup", &mSetupGuid, NULL, &bufferSize, setupBuffer);
            if (status == EFI_SUCCESS && Above4GOffset < 0xFFFF && Above4GOffset < bufferSize) {
		    	reBarState = setupBuffer[Above4GOffset] > 0 ? 9 : reBarState; // ASUS Z97-K R2.0 的 Above 4G Decoding 是 0x1 即使不是，设置 9 即 512MB 问题也不大，双卡两个 512MB 也是很轻松
            }
            if (status == EFI_SUCCESS && SetupStateOffset < 0xFFFF && SetupStateOffset < bufferSize) {
		    	reBarState = setupBuffer[SetupStateOffset];
            }
            gBS->FreePool(setupBuffer);
        }
	}
	BootMenuDetect = reBarState && BootMenuDetect ? 1 : 0; // BootMenuDetect 只有在 4G打开 或 SetupStateOffset 已指定 reBarState 后才检测 UEFI 启动菜单快速修正 reBarState （清CMOS只清4G开关，清菜单应该要拔电池）
	DefaultState = reBarState && DefaultState ? 1 : 0; // DefaultState 同理受控于 4G打开 或 SetupStateOffset
	
	// added
	UINT16 bootIndex;
    UINT8 *bootBuffer = NULL;
    CHAR16 bootVarName[] = L"Boot0000";
    // 暴力遍历 Boot0000 到 Boot000F 这 16 个潜在的启动项
    for (bootIndex = 0; BootMenuDetect && bootIndex <= 0x000F; bootIndex++) {
		bootVarName[7] = (CHAR16)((bootIndex < 10) ? (L'0' + bootIndex) : (L'A' + (bootIndex - 10)));
        bufferSize = 0;
        status = gRT->GetVariable(bootVarName, &gEfiGlobalVariableGuid, NULL, &bufferSize, NULL);
        if (status == EFI_BUFFER_TOO_SMALL) {
            // 开辟内存读取整个启动项结构
            gBS->AllocatePool(EfiBootServicesData, bufferSize, (VOID**)&bootBuffer);
            status = gRT->GetVariable(bootVarName, &gEfiGlobalVariableGuid, NULL, &bufferSize, bootBuffer);
            if (status == EFI_SUCCESS) {
                // EFI_LOAD_OPTION 结构：
                // Attributes (4 字节) + FilePathListLength (2 字节)
                // 紧接着就是以 NULL 结尾的 Description 字符串（宽字符）
                CHAR16 *description = (CHAR16*)(bootBuffer + 6);
                
                // 寻找黄金关键字 L"ReBarState="
                CHAR16 *match = StrStr16(description, L"ReBarState=");
                if (match != NULL) {
                    CHAR16 *valueStr = match + 11; // 跳过 "ReBarState=" 这 11 个字符
                    UINTN parsedValue = StrDecimalToUintn(valueStr);
                    if (parsedValue <= 32) {
                        reBarState = (UINT8)parsedValue;
                        DEBUG((DEBUG_INFO, "ReBarDXE: Hijacked from %s! Found ReBarState=%u\n", bootVarName, reBarState));
                        gBS->FreePool(bootBuffer);
                        break; // 找到了就立刻收工，以此项为准！
                    }
                }
            }
            gBS->FreePool(bootBuffer);
        }
    }
	
    //DEBUG((DEBUG_INFO, "ReBarDXE: Loaded\n"));

    // Read ReBarState variable
    UINT32 attributes;
    EFI_TIME time;
	bufferSize = 1;
    status = !DefaultState ? EFI_BUFFER_TOO_SMALL : gRT->GetVariable(L"ReBarState", &reBarStateGuid,
        &attributes,
        &bufferSize, &reBarState);
    // any attempts to overflow reBarState should result in EFI_BUFFER_TOO_SMALL
    //if (status != EFI_SUCCESS)
	//{
	//	// added
	//	UINT8 Above4G_Enabled = 0;
	//	UINTN NumberOfDescriptors = 0;
	//	EFI_GCD_MEMORY_SPACE_DESCRIPTOR *MemorySpaceMap = NULL;
	//	// 1. 动态获取当前主板开机自检后，分配出来的全部硬件内存空间描述符
	//	status = gDS->GetMemorySpaceMap(&NumberOfDescriptors, &MemorySpaceMap);
	//	if (status == EFI_SUCCESS) {
    //		for (UINTN i = 0; i < NumberOfDescriptors; i++) {
    //    		// 2. 物理扫描：如果发现主板已经把 PCIe MMIO 资源（非系统主内存）开辟到了 4GB 以上的空间
    //    		if (MemorySpaceMap[i].GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo &&
    //        		MemorySpaceMap[i].BaseAddress >= 0x100000000ULL) { 
    //        		// 3. 证明主板此时硬件上已经绝对支持并开启了大窗口，安全阀直接解锁！
    //        		Above4G_Enabled = 1;
    //        		break;
    //    		}
    //		}
    //		gBS->FreePool(MemorySpaceMap);
	//	}
	//	reBarState = !Above4G_Enabled ? 0 : 9; // 后续 reBarSetupDevice 再分流
	//}
	if (status == EFI_SUCCESS)
	{
        // Detect CMOS reset by checking if year before BUILD_YEAR
        status = gRT->GetTime (&time, NULL);
        if (time.Year < BUILD_YEAR) {
            reBarState = 0;
            bufferSize = 1;
            attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
            status = gRT->SetVariable(L"ReBarState", &reBarStateGuid,
                attributes,
                bufferSize, &reBarState);
            return EFI_SUCCESS;
        }
	}

    if (reBarState)
    {
        DEBUG((DEBUG_INFO, "ReBarDXE: Enabled, maximum BAR size 2^%u MB\n", reBarState));

        // For overriding PciHostBridgeResourceAllocationProtocol
        pciHostBridgeResourceAllocationProtocolHook();
    }

    return EFI_SUCCESS;
}
