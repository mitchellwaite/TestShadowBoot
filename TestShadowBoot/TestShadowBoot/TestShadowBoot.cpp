#include <stdint.h>
#include <xtl.h>
#include "Kernel.h"
#include "HvExpansion.h"

#define KRNL_VERSION	12387
#define KRNL_VERSION_TEXT L"12387"

#if KRNL_VERSION == 17489

#define DEVKIT_USBD_POWER_DOWN_NOTIFICATION		0x8010e140
#define TESTKIT_USBD_POWER_DOWN_NOTIFICATION	0x8010cbf0

#define HVX_QUIESCE_PROCESSOR	2
#define HVX_SHADOW_BOOT			33

#elif KRNL_VERSION == 12387

#define DEVKIT_USBD_POWER_DOWN_NOTIFICATION		0x800F69E8
#define TESTKIT_USBD_POWER_DOWN_NOTIFICATION	0x800F6168

#define HVX_QUIESCE_PROCESSOR	2
#define HVX_SHADOW_BOOT			33

#else

#error Unsupported kernel version

#endif

#define NUM_PROCESSORS	6

#define DEVKIT_SHADOW_BOOT_ROM	"GAME:\\xboxromw2d.bin"
#define TESTKIT_SHADOW_BOOT_ROM	"GAME:\\xboxromtw2d.bin"

static DWORD szXboxRom, XboxRomPhys;
static PVOID pXboxRom;


static void MessageBox(wchar_t *text)
{
	static LPCWSTR buttons[1] = {L"OK"};
	static MESSAGEBOX_RESULT result;
	static XOVERLAPPED overlapped;

	memset(&overlapped, 0, sizeof(overlapped));
	memset(&result, 0, sizeof(result));

    if (XShowMessageBoxUI(XUSER_INDEX_ANY, L"XellLaunch2", text, 1, buttons, 0, XMB_ERRORICON, &result, &overlapped) == ERROR_IO_PENDING)
    {
        while (!XHasOverlappedIoCompleted(&overlapped))
            Sleep(50);
    }
}

static __declspec(naked) ULONG MyThreadId()
{
	__asm
	{
		lbz		r3,0x10C(r13)
		blr
	}
}

// NOTE: these are valid for 17489 only BLDR_SHADOW_BOOT

#define QUIESCE_REASON_SHADOW_BOOT	2

static __declspec(naked) VOID HvxQuiesceProcessor(ULONG QuiesceReason)
{
	__asm
	{
		li	r0, HVX_QUIESCE_PROCESSOR
		sc
		blr
	}
}

#define BLDR_SHADOW_BOOT	0x200

static __declspec(naked) VOID HvxShadowBoot(ULONG XboxRomPhys, ULONG XboxRomSize, ULONG BldrFlags)
{
	__asm
	{
		li	r0, HVX_SHADOW_BOOT
		sc
		blr
	}
}

struct MY_SHADOW_BOOT_PARAM
{
	KDPC DpcArray[NUM_PROCESSORS];
	LONG StartedCount;
};

static VOID MyShadowBootDpc(KDPC *Dpc, MY_SHADOW_BOOT_PARAM *Param)
{
	InterlockedIncrement(&Param->StartedCount);

	if (MyThreadId() == 0)
	{
		while (Param->StartedCount < NUM_PROCESSORS)
			KeRetireDpcList();

		// Wait a bit because secondary thread must be waiting in
		// HvxQuiesceProcessor() before calling HvxShadowBoot()
		for (int i = 0; i < 10000000; i++)
			__asm nop

		HvxShadowBoot(XboxRomPhys, szXboxRom, BLDR_SHADOW_BOOT);
	}
	else
	{
		while (Param->StartedCount < NUM_PROCESSORS)
			KeRetireDpcList();

		HvxQuiesceProcessor(QUIESCE_REASON_SHADOW_BOOT);
	}
}

static VOID MyShadowBoot()
{
	MY_SHADOW_BOOT_PARAM Param = {0};
	BYTE OldIrql;

	for (int i = 0; i < NUM_PROCESSORS; i++)
	{
		KeInitializeDpc(&Param.DpcArray[i], MyShadowBootDpc, &Param);
		Param.DpcArray[i].TargetNumber = i + 1; // NOTE: 1 based
	}

	OldIrql = KeRaiseIrqlToDpcLevel();
	for (int i = 0; i < NUM_PROCESSORS; i++)
		KeInsertQueueDpc(&Param.DpcArray[i], NULL, NULL);
	KfLowerIrql(OldIrql);

	for (;;)
		;
}

void __cdecl main()
{
	const char *XboxRomFilename = DEVKIT_SHADOW_BOOT_ROM;
	void (*UsbdPowerDownNotification)() = (decltype(UsbdPowerDownNotification)) DEVKIT_USBD_POWER_DOWN_NOTIFICATION;
	HANDLE hXboxRom;
	DWORD szRead;
	BYTE OldIrql;

	// Make sure we are on a dev/test kernel
	if (XboxKrnlVersion->Build != KRNL_VERSION || (XboxHardwareInfo->Flags & XBOX_HW_FLAG_NOT_RETAIL) == 0)
	{
		MessageBox(L"Error: must be running on" KRNL_VERSION_TEXT L"devkit or testkit kernel\n");
		return;
	}

	// Use testkit shadowboot filename and correct function addresses on test kits
	if ((XboxHardwareInfo->Flags & XBOX_HW_FLAG_TESTKIT) != 0)
	{
		XboxRomFilename = TESTKIT_SHADOW_BOOT_ROM;
		UsbdPowerDownNotification = (decltype(UsbdPowerDownNotification)) TESTKIT_USBD_POWER_DOWN_NOTIFICATION;

#if KRNL_VERSION == 12387
		DbgPrint("Checking if the HVPP expansion is installed...\n");
		if (HvPeekWORD(0) != 0x5E4E)
		{
			// install signed and encrypted HVPP expansion
			DbgPrint("Installing HVPP expansion...\n");
			QWORD qwRet = InstallExpansion();
			if ((qwRet & 0xFFFFFFFF) != ERROR_SUCCESS)
			{
				if(HvPeekWORD(0) != 0x5E4E)
				{
					MessageBox(L"Error: failed to install HVPP expansion!\n");
					return;
				}
			}

			DbgPrint("Done!\n");
		}
		else
		{
			DbgPrint("Expansion is already installed, skipping...\n");
		}

		// Is this enough to patch the shadowboot routine?
		// The rest of the bootanim exploit is the memory
		// 
		HvPokeDWORD(0xD994, 0x38600001);
#endif
	}

	// Load shadowboot boot ROM
	hXboxRom = CreateFile(XboxRomFilename, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hXboxRom == INVALID_HANDLE_VALUE)
	{
		MessageBox(L"Failed to open shadowboot rom file!");
		return;
	}
	szXboxRom = GetFileSize(hXboxRom, NULL);
	// NOTE: it is important the shadowboot ROM is allocated in a specific area of memory because the HV payload will
	// zero a lot of phyiscal memory.
	pXboxRom = MmAllocatePhysicalMemoryEx(2, szXboxRom, 4, 0x1000000, 0x1800000, 0x1000);
	XboxRomPhys = MmGetPhysicalAddress(pXboxRom);
	if (pXboxRom == NULL)
	{
		MessageBox(L"Failed to allocate memory for shadowboot!");
		return;
	}
	if (ReadFile(hXboxRom, pXboxRom, szXboxRom, &szRead, NULL) == FALSE || szRead < szXboxRom)
	{
		MessageBox(L"Failed to read shadowboot rom file!");
		return;
	}

	// Disable USB devices to avoid messing up next kernel
	OldIrql = KeRaiseIrqlToDpcLevel();
	UsbdPowerDownNotification();
	KfLowerIrql(OldIrql);

	// Run shadowboot using DPC mechanism
	MyShadowBoot();
}