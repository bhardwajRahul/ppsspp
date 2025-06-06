// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <set>

#include "Common/Data/Format/IniFile.h"

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeMap.h"
#include "Common/Serialize/SerializeSet.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/HLEHelperThread.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/Reporting.h"
#include "Core/System.h"

#include "Core/HLE/sceJpeg.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/scePower.h"
#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HLE/sceNet.h"

#include "Core/Dialog/PSPSaveDialog.h"
#include "Core/Dialog/PSPMsgDialog.h"
#include "Core/Dialog/PSPPlaceholderDialog.h"
#include "Core/Dialog/PSPOskDialog.h"
#include "Core/Dialog/PSPGamedataInstallDialog.h"
#include "Core/Dialog/PSPNetconfDialog.h"
#include "Core/Dialog/PSPNpSigninDialog.h"
#include "Core/Dialog/PSPScreenshotDialog.h"

#define PSP_AV_MODULE_AVCODEC     0
#define PSP_AV_MODULE_SASCORE     1
#define PSP_AV_MODULE_ATRAC3PLUS  2 // Requires PSP_AV_MODULE_AVCODEC loading first
#define PSP_AV_MODULE_MPEGBASE    3 // Requires PSP_AV_MODULE_AVCODEC loading first
#define PSP_AV_MODULE_MP3         4
#define PSP_AV_MODULE_VAUDIO      5
#define PSP_AV_MODULE_AAC         6
#define PSP_AV_MODULE_G729        7

#define PSP_USB_MODULE_PSPCM 1
#define PSP_USB_MODULE_ACC 2
#define PSP_USB_MODULE_MIC 3 // Requires PSP_USB_MODULE_ACC loading first
#define PSP_USB_MODULE_CAM 4 // Requires PSP_USB_MODULE_ACC loading first
#define PSP_USB_MODULE_GPS 5 // Requires PSP_USB_MODULE_ACC loading first

static const int noDeps[] = {0};
static const int httpModuleDeps[] = {0x0102, 0x0103, 0x0104, 0};
static const int sslModuleDeps[] = {0x0102, 0};
static const int httpStorageModuleDeps[] = {0x00100, 0x0102, 0x0103, 0x0104, 0x0105, 0};
static const int atrac3PlusModuleDeps[] = {0x0300, 0};
static const int mpegBaseModuleDeps[] = {0x0300, 0};
static const int mp4ModuleDeps[] = {0x0300, 0};

static void NotifyLoadStatusAvcodec(int state, u32 loadAddr, u32 totalSize) {
	JpegNotifyLoadStatus(state);
}

static void NotifyLoadStatusAtrac(int state, u32 loadAddr, u32 totalSize) {
	if (state == 1) {
		// If HLE of sceAtrac is disabled, things will break!
		// For now we do angry logging and a debug assert.
		if ((DisableHLEFlags)g_Config.iDisableHLE & DisableHLEFlags::sceAtrac) {
			ERROR_LOG(Log::ME, "sceAtrac HLE is disabled, and the game tries to load sceAtrac from firmware - this won't work!");
			_dbg_assert_(false);

			// Actually, if the user has an F0 (psardumper) dump, we could go look for the file there.
		}

		// We try to imitate a recent version of the prx.
		// Let's just give it a piece of the space.
		constexpr int version = 0x105;  // latest.
		constexpr int bssSize = 0x67C;
		_dbg_assert_(bssSize <= totalSize);
		__AtracNotifyLoadModule(version, 0, loadAddr, bssSize);
	} else if (state == -1) {
		// Unload.
		__AtracNotifyUnloadModule();
	}
}

ModuleLoadInfo::ModuleLoadInfo(int m, u32 s, const char *_name, ModuleLoadCallback n)
	: name(_name), mod(m), size(s), dependencies(noDeps), notify(n) {}
ModuleLoadInfo::ModuleLoadInfo(int m, u32 s, const char *_name, const int *d, ModuleLoadCallback n)
	: name(_name), mod(m), size(s), dependencies(d), notify(n) {}

// Not sure if these have official names, or if there's a mapping exactly to HLE modules.
static const ModuleLoadInfo moduleLoadInfo[] = {
	ModuleLoadInfo(0x100, 0x00014000, "net_common"),
	ModuleLoadInfo(0x101, 0x00020000, "net_adhoc"),
	ModuleLoadInfo(0x102, 0x00058000, "net_inet"),
	ModuleLoadInfo(0x103, 0x00006000, "net_parse_uri"),
	ModuleLoadInfo(0x104, 0x00002000, "net_parse_http"),
	ModuleLoadInfo(0x105, 0x00028000, "net_http", httpModuleDeps),
	ModuleLoadInfo(0x106, 0x00044000, "net_ssl", sslModuleDeps),
	ModuleLoadInfo(0x107, 0x00010000, "unk_0x107"),
	ModuleLoadInfo(0x108, 0x00008000, "usb_pspcm", httpStorageModuleDeps),
	ModuleLoadInfo(0x200, 0x00000000, "usb_mic"),
	ModuleLoadInfo(0x201, 0x00000000, "usb_cam"),
	ModuleLoadInfo(0x202, 0x00000000, "usb_gps"),
	ModuleLoadInfo(0x203, 0x00000000, "usb_unk_0x203"),
	ModuleLoadInfo(0x2ff, 0x00000000, "unk_0x2ff"),
	ModuleLoadInfo(0x300, 0x00000000, "av_avcodec", &NotifyLoadStatusAvcodec),  // AudioCodec
	ModuleLoadInfo(0x301, 0x00000000, "av_sascore"),
	// The size varies a bit per version, from about 0x3C00 to 0x4500 bytes. We could make a lookup table...
	// Changing this breaks some bad cheats though..
	ModuleLoadInfo(0x302, 0x00008000, "av_atrac3plus", atrac3PlusModuleDeps, &NotifyLoadStatusAtrac),
	ModuleLoadInfo(0x303, 0x0000c000, "av_mpegbase", mpegBaseModuleDeps),
	ModuleLoadInfo(0x304, 0x00004000, "av_mp3"),
	ModuleLoadInfo(0x305, 0x0000a300, "av_vaudio"),
	ModuleLoadInfo(0x306, 0x00004000, "av_aac"),
	ModuleLoadInfo(0x307, 0x00000000, "av_g729"),
	ModuleLoadInfo(0x308, 0x0003c000, "av_mp4", mp4ModuleDeps),
	ModuleLoadInfo(0x3fe, 0x00000000, "me_stuff"),
	ModuleLoadInfo(0x3ff, 0x00000000, "me_core"),  // ME Core?
	ModuleLoadInfo(0x400, 0x0000c000, "np_common"),
	ModuleLoadInfo(0x401, 0x00018000, "np_service"),
	ModuleLoadInfo(0x402, 0x00048000, "np_matching2"),
	ModuleLoadInfo(0x403, 0x0000e000, "np_unk_0x403"),
	ModuleLoadInfo(0x500, 0x00000000, "np_drm"),
	ModuleLoadInfo(0x600, 0x00000000, "irda"),
	ModuleLoadInfo(0x601, 0x00000000, "unk_0x601"),
};

// Only a single dialog is allowed at a time.
static UtilityDialogType currentDialogType;
bool currentDialogActive;
static PSPSaveDialog *saveDialog;
static PSPMsgDialog *msgDialog;
static PSPOskDialog *oskDialog;
static PSPNetconfDialog *netDialog;
static PSPScreenshotDialog *screenshotDialog;
static PSPGamedataInstallDialog *gamedataInstallDialog;
static PSPNpSigninDialog *npSigninDialog;

static int oldStatus = -1;
static std::map<int, u32> currentlyLoadedModules;
static int volatileUnlockEvent = -1;
static HLEHelperThread *accessThread = nullptr;
static bool accessThreadFinished = true;
static const char *accessThreadState = "initial";
static int lastSaveStateVersion = -1;
static int netParamLatestId = 1;

static void CleanupDialogThreads(bool force = false) {
	if (accessThread) {
		if (accessThread->Stopped() || accessThreadFinished) {
			delete accessThread;
			accessThread = nullptr;
			accessThreadState = "cleaned up";
		} else if (force) {
			ERROR_LOG_REPORT(Log::sceUtility, "Utility access thread still running, state: %s, dialog=%d/%d", accessThreadState, (int)currentDialogType, currentDialogActive);

			// Try to force shutdown anyway.
			accessThread->Terminate();
			delete accessThread;
			accessThread = nullptr;
			accessThreadState = "force terminated";
			// Try to unlock in case other dialog was shutting down.
			KernelVolatileMemUnlock(0);
		}
	}
}

static void ActivateDialog(UtilityDialogType type) {
	CleanupDialogThreads();
	if (!currentDialogActive) {
		currentDialogType = type;
		currentDialogActive = true;
		// So that we log the next one.
		oldStatus = -1;
	}
}

static void DeactivateDialog() {
	CleanupDialogThreads();
	if (currentDialogActive) {
		currentDialogActive = false;
	}
}

static PSPDialog *CurrentDialog(UtilityDialogType type) {
	switch (type) {
	case UtilityDialogType::NONE:
		break;
	case UtilityDialogType::SAVEDATA:
		return saveDialog;
	case UtilityDialogType::MSG:
		return msgDialog;
	case UtilityDialogType::OSK:
		return oskDialog;
	case UtilityDialogType::NET:
		return netDialog;
	case UtilityDialogType::SCREENSHOT:
		return screenshotDialog;
	case UtilityDialogType::GAMESHARING:
		break;
	case UtilityDialogType::GAMEDATAINSTALL:
		return gamedataInstallDialog;
	case UtilityDialogType::NPSIGNIN:
		return npSigninDialog;
	}
	return nullptr;
}

static void UtilityVolatileUnlock(u64 userdata, int cyclesLate) {
	PSPDialog *dialog = CurrentDialog(currentDialogType);
	if (dialog)
		dialog->FinishVolatile();
}

void __UtilityInit() {
	saveDialog = new PSPSaveDialog(UtilityDialogType::SAVEDATA);
	msgDialog = new PSPMsgDialog(UtilityDialogType::MSG);
	oskDialog = new PSPOskDialog(UtilityDialogType::OSK);
	netDialog = new PSPNetconfDialog(UtilityDialogType::NET);
	screenshotDialog = new PSPScreenshotDialog(UtilityDialogType::SCREENSHOT);
	gamedataInstallDialog = new PSPGamedataInstallDialog(UtilityDialogType::GAMEDATAINSTALL);
	npSigninDialog = new PSPNpSigninDialog(UtilityDialogType::NPSIGNIN);

	currentDialogType = UtilityDialogType::NONE;
	DeactivateDialog();
	SavedataParam::Init();
	currentlyLoadedModules.clear();
	volatileUnlockEvent = CoreTiming::RegisterEvent("UtilityVolatileUnlock", UtilityVolatileUnlock);

	ResetSecondsSinceLastGameSave();
}

void __UtilityDoState(PointerWrap &p) {
	auto s = p.Section("sceUtility", 1, 6);
	if (!s) {
		return;
	}

	Do(p, currentDialogType);
	Do(p, currentDialogActive);
	saveDialog->DoState(p);
	msgDialog->DoState(p);
	oskDialog->DoState(p);
	netDialog->DoState(p);
	screenshotDialog->DoState(p);
	gamedataInstallDialog->DoState(p);

	if (s >= 2) {
		Do(p, currentlyLoadedModules);
	} else {
		std::set<int> oldModules;
		Do(p, oldModules);
		for (auto it = oldModules.begin(), end = oldModules.end(); it != end; ++it) {
			currentlyLoadedModules[*it] = 0;
		}
	}

	if (s >= 3) {
		Do(p, volatileUnlockEvent);
	} else {
		volatileUnlockEvent = -1;
	}
	CoreTiming::RestoreRegisterEvent(volatileUnlockEvent, "UtilityVolatileUnlock", UtilityVolatileUnlock);

	bool hasAccessThread = accessThread != nullptr;
	if (s >= 4) {
		Do(p, hasAccessThread);
		if (hasAccessThread) {
			Do(p, accessThread);
			if (p.mode == p.MODE_READ)
				accessThreadState = "from save state";
		}
	} else {
		hasAccessThread = false;
	}

	if (s >= 5)
		Do(p, accessThreadFinished);

	if (s >= 6) {
		npSigninDialog->DoState(p);
		lastSaveStateVersion = -1;
	} else {
		lastSaveStateVersion = s.Version();
	}

	if (!hasAccessThread && accessThread) {
		accessThread->Forget();
		delete accessThread;
		accessThread = nullptr;
		accessThreadState = "cleared from save state";
	}
}

void __UtilityShutdown() {
	saveDialog->Shutdown(true);
	msgDialog->Shutdown(true);
	oskDialog->Shutdown(true);
	netDialog->Shutdown(true);
	screenshotDialog->Shutdown(true);
	gamedataInstallDialog->Shutdown(true);
	npSigninDialog->Shutdown(true);

	if (accessThread) {
		// Don't need to free it during shutdown, may have already been freed.
		accessThread->Forget();
		delete accessThread;
		accessThread = nullptr;
		accessThreadState = "shutdown";
	}
	accessThreadFinished = true;
	lastSaveStateVersion = -1;

	delete saveDialog;
	delete msgDialog;
	delete oskDialog;
	delete netDialog;
	delete screenshotDialog;
	delete gamedataInstallDialog;
	delete npSigninDialog;
}

void UtilityDialogInitialize(UtilityDialogType type, int delayUs, int priority) {
	int partDelay = delayUs / 4;
	const u32_le insts[] = {
		// Make sure we don't discard/deadbeef a0.
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_S0, MIPS_REG_A0, 0),

		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_ZERO, 0),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A1, MIPS_REG_ZERO, 0),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A2, MIPS_REG_ZERO, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceSuspendForUser", "sceKernelVolatileMemLock"),

		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),

		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_ZERO, (int)type),
		(u32_le)MIPS_MAKE_JR_RA(),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityInitDialog"),
	};

	CleanupDialogThreads(true);
	accessThread = new HLEHelperThread("ScePafJob", insts, (uint32_t)ARRAY_SIZE(insts), priority, 0x200);
	accessThread->Start(partDelay, 0);
	accessThreadFinished = false;
	accessThreadState = "initializing";
}

void UtilityDialogShutdown(UtilityDialogType type, int delayUs, int priority) {
	// Break it up so better-priority rescheduling happens.
	// The windows aren't this regular, but close.
	int partDelay = delayUs / 4;
	const u32_le insts[] = {
		// Make sure we don't discard/deadbeef 'em.
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_S0, MIPS_REG_A0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),
		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_S0, 0),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityWorkUs"),

		(u32_le)MIPS_MAKE_ORI(MIPS_REG_A0, MIPS_REG_ZERO, (int)type),
		(u32_le)MIPS_MAKE_JR_RA(),
		(u32_le)MIPS_MAKE_SYSCALL("sceUtility", "__UtilityFinishDialog"),
	};

	CleanupDialogThreads(true);
	bool prevInterrupts = __InterruptsEnabled();
	__DisableInterrupts();
	accessThread = new HLEHelperThread("ScePafJob", insts, (uint32_t)ARRAY_SIZE(insts), priority, 0x200);
	accessThread->Start(partDelay, 0);
	accessThreadFinished = false;
	accessThreadState = "shutting down";
	if (prevInterrupts)
		__EnableInterrupts();
}

static int UtilityWorkUs(int us) {
	// This blocks, but other better priority threads can get time.
	// Simulate this by allowing a reschedule.
	if (us > 1000) {
		hleEatMicro(1000);
		return hleDelayResult(hleNoLog(0), "utility work", us - 1000);
	}
	hleEatMicro(us);
	hleReSchedule("utility work");
	return hleNoLog(0);
}

static int UtilityInitDialog(int type) {
	PSPDialog *dialog = CurrentDialog((UtilityDialogType)type);
	accessThreadFinished = true;
	accessThreadState = "init finished";
	if (dialog)
		return hleLogDebug(Log::sceUtility, dialog->FinishInit());
	return hleLogError(Log::sceUtility, 0, "invalid dialog type?");
}

static int UtilityFinishDialog(int type) {
	PSPDialog *dialog = CurrentDialog((UtilityDialogType)type);
	accessThreadFinished = true;
	accessThreadState = "shutdown finished";
	if (dialog)
		return hleLogDebug(Log::sceUtility, dialog->FinishShutdown());
	return hleLogError(Log::sceUtility, 0, "invalid dialog type?");
}

static int sceUtilitySavedataInitStart(u32 paramAddr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::SAVEDATA) {
		if (PSP_CoreParameter().compat.flags().YugiohSaveFix) {
			WARN_LOG_REPORT(Log::sceUtility, "Yugioh Savedata Correction (state=%d)", lastSaveStateVersion);
			if (accessThread) {
				accessThread->Terminate();
				delete accessThread;
				accessThread = nullptr;
				accessThreadFinished = true;
				accessThreadState = "terminated";
				// Try to unlock in case other dialog was shutting down.
				KernelVolatileMemUnlock(0);
			}
		} else {
			return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
		}
	}

	ActivateDialog(UtilityDialogType::SAVEDATA);
	return hleLogDebug(Log::sceUtility, saveDialog->Init(paramAddr));
}

static int sceUtilitySavedataShutdownStart() {
	if (currentDialogType != UtilityDialogType::SAVEDATA)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");

	DeactivateDialog();
	int ret = saveDialog->Shutdown();
	hleEatCycles(30000);
	return hleLogDebug(Log::sceUtility, ret);
}

static int sceUtilitySavedataGetStatus() {
	if (currentDialogType != UtilityDialogType::SAVEDATA) {
		hleEatCycles(200);
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = saveDialog->GetStatus();
	hleEatCycles(200);
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}

static int sceUtilitySavedataUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::SAVEDATA || !saveDialog) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int result = hleLogDebug(Log::sceUtility, saveDialog->Update(animSpeed));
	if (result >= 0)
		return hleDelayResult(result, "savedata update", 300);
	return result;
}

const ModuleLoadInfo *__UtilityModuleInfo(int module) {
	const ModuleLoadInfo *info = 0;
	for (size_t i = 0; i < ARRAY_SIZE(moduleLoadInfo); ++i) {
		if (moduleLoadInfo[i].mod == module) {
			info = &moduleLoadInfo[i];
			break;
		}
	}
	return info;
}

const std::map<int, u32> &__UtilityGetLoadedModules() {
	return currentlyLoadedModules;
}

bool __UtilityModuleGetMemoryRange(int moduleID, u32 *startPtr, u32 *sizePtr) {
	const ModuleLoadInfo *info = __UtilityModuleInfo(moduleID);
	if (!info) {
		return false;
	}
	*sizePtr = info->size;
	auto iter = currentlyLoadedModules.find(moduleID);
	if (iter == currentlyLoadedModules.end()) {
		return false;
	}
	*startPtr = iter->second;
	return true;
}

static int LoadModuleInternal(u32 module);
static int UnloadModuleInternal(u32 module);

// Same as sceUtilityLoadModule, just limited in categories.
// It seems this just loads module 0x300 + module & 0xFF..
static u32 sceUtilityLoadAvModule(u32 module) {
	if (module > 7) {
		ERROR_LOG_REPORT(Log::sceUtility, "sceUtilityLoadAvModule(%i): invalid module id", module);
		return hleLogError(Log::sceUtility, SCE_ERROR_AV_MODULE_BAD_ID);
	}

	int result = LoadModuleInternal(0x300 | module);
	return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility av module loaded", 25000);
}

static u32 sceUtilityUnloadAvModule(u32 module) {
	if (module > 7) {
		ERROR_LOG_REPORT(Log::sceUtility, "sceUtilityLoadAvModule(%i): invalid module id", module);
		return hleLogError(Log::sceUtility, SCE_ERROR_AV_MODULE_BAD_ID);
	}

	int result = UnloadModuleInternal(0x300 | module);
	return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility av module unloaded", 800);
}

static u32 sceUtilityLoadModule(u32 module) {
	int result = LoadModuleInternal(module);
	// TODO: Each module has its own timing, technically, but this is a low-end.
	if (module == 0x3FF) {
		return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility module loaded", 130);
	} else {
		return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility module loaded", 25000);
	}
}

static u32 sceUtilityUnloadModule(u32 module) {
	int result = UnloadModuleInternal(module);
	// TODO: Each module has its own timing, technically, but this is a low-end.
	if (module == 0x3FF) {
		return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility module unloaded", 110);
	} else {
		return hleDelayResult(hleLogDebugOrError(Log::sceUtility, result), "utility module unloaded", 400);
	}
}

static int LoadModuleInternal(u32 module) {
	const ModuleLoadInfo *info = __UtilityModuleInfo(module);
	if (!info) {
		return SCE_ERROR_MODULE_BAD_ID;
	}

	if (currentlyLoadedModules.find(module) != currentlyLoadedModules.end()) {
		return SCE_ERROR_MODULE_ALREADY_LOADED;
	}

	// Some games, like Kamen Rider Climax Heroes OOO, require an error if dependencies aren't loaded yet.
	for (const int *dep = info->dependencies; *dep != 0; ++dep) {
		if (currentlyLoadedModules.find(*dep) == currentlyLoadedModules.end()) {
			return SCE_KERNEL_ERROR_LIBRARY_NOTFOUND;
		}
	}

	u32 allocSize = info->size;
	u32 address = 0;
	char name[128];
	snprintf(name, sizeof(name), "UtilityModule/%3x_%s", module, info->name);
	if (allocSize != 0) {
		address = userMemory.Alloc(allocSize, false, name);
	}
	currentlyLoadedModules[module] = address;
	if (info->notify) {
		info->notify(1, address, allocSize);
	}
	return 0;
}

static int UnloadModuleInternal(u32 module) {
	const ModuleLoadInfo *info = __UtilityModuleInfo(module);
	if (!info) {
		return SCE_ERROR_MODULE_BAD_ID;
	}

	auto iter = currentlyLoadedModules.find(module);
	if (iter == currentlyLoadedModules.end()) {
		return SCE_ERROR_MODULE_NOT_LOADED;
	}
	if (iter->second != 0) {
		userMemory.Free(iter->second);
	}
	currentlyLoadedModules.erase(module);

	if (info->notify) {
		info->notify(-1, 0, 0);
	}
	return 0;
}

static int sceUtilityMsgDialogInitStart(u32 paramAddr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::MSG) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::MSG);
	return hleLogInfo(Log::sceUtility, msgDialog->Init(paramAddr));
}

static int sceUtilityMsgDialogShutdownStart() {
	if (currentDialogType != UtilityDialogType::MSG) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, msgDialog->Shutdown());
}

static int sceUtilityMsgDialogUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::MSG) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int ret = msgDialog->Update(animSpeed);
	if (ret >= 0)
		return hleDelayResult(hleLogDebug(Log::sceUtility, ret), "msgdialog update", 800);
	else
		return hleLogDebug(Log::sceUtility, ret);
}

static int sceUtilityMsgDialogGetStatus() {
	if (currentDialogType != UtilityDialogType::MSG) {
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = msgDialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}

static int sceUtilityMsgDialogAbort() {
	if (currentDialogType != UtilityDialogType::MSG) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogDebug(Log::sceUtility, msgDialog->Abort());
}


// On screen keyboard
static int sceUtilityOskInitStart(u32 oskPtr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::OSK) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::OSK);
	return hleLogInfo(Log::sceUtility, oskDialog->Init(oskPtr));
}

static int sceUtilityOskShutdownStart() {
	if (currentDialogType != UtilityDialogType::OSK) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, oskDialog->Shutdown());
}

static int sceUtilityOskUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::OSK) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	// This is the vblank period, plus a little slack. Needed to fix timing bug in Ghost Recon: Predator.
	// See issue #12044.
	hleEatCycles(msToCycles(0.7315 + 0.1));
	return hleLogDebug(Log::sceUtility, oskDialog->Update(animSpeed));
}

static int sceUtilityOskGetStatus() {
	if (currentDialogType != UtilityDialogType::OSK) {
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = oskDialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}


static int sceUtilityNetconfInitStart(u32 paramsAddr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::NET) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::NET);
	return hleLogInfo(Log::sceUtility, netDialog->Init(paramsAddr));
}

static int sceUtilityNetconfShutdownStart() {
	if (currentDialogType != UtilityDialogType::NET) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, netDialog->Shutdown());
}

static int sceUtilityNetconfUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::NET) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogDebug(Log::sceUtility, netDialog->Update(animSpeed));
}

static int sceUtilityNetconfGetStatus() {
	if (currentDialogType != UtilityDialogType::NET) {
		// Spam in Danball Senki BOOST.
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = netDialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}

/**
* Check existence of a Net Configuration
*
* @param id - id of net Configuration (1 to n)
* @return 0 on success
*
* Note: some homebrew may only support a limited number of entries (ie. 10 entries)
*/
static int sceUtilityCheckNetParam(int id)
{
	bool available = (id >= 0 && id <= 24);
	// We only have 1 faked net config entry
	if (id > PSP_NETPARAM_MAX_NUMBER_DUMMY_ENTRIES) {
		available = false;
	}
	int ret = available ? 0 : SCE_ERROR_NETPARAM_BAD_NETCONF;
	return hleLogDebugOrError(Log::sceUtility, ret);
}

/**
* Get Net Configuration Parameter
*
* @param conf - Net Configuration number (1 to n) (0 returns valid but seems to be a copy of the last config requested)
* @param param - which parameter to get
* @param data - parameter data
* @return 0 on success
*/
// Let's figure out what games use this.
static int sceUtilityGetNetParam(int id, int param, u32 dataAddr) {
	if (id < 0 || id > 24) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_NETPARAM_BAD_NETCONF, "invalid id=%d", id);
	}

	if (!g_netApctlInited) {
		// Is this allowed?
		WARN_LOG_REPORT_ONCE(getnetparam_early, Log::sceNet, "sceUtilityGetNetParam called before initializing netApctl!");
	}

	// TODO: Replace the temporary netApctlInfo with netConfInfo, since some of netApctlInfo contents supposed to be taken from netConfInfo during ApctlInit, while sceUtilityGetNetParam can be used before Apctl Initialized
	char name[APCTL_PROFILENAME_MAXLEN];
	truncate_cpy(name, sizeof(name), defaultNetConfigName + std::to_string(id == 0 ? netParamLatestId:id));
	char dummyWEPKey[6] = "XXXXX"; // WEP 64-bit = 10 hex digits key or 5-digit ASCII equivalent
	char dummyUserPass[256] = "PPSSPP"; // FIXME: Username / Password max length = 255 chars?
	char dummyWPAKey[64] = "XXXXXXXX"; // FIXME: WPA 256-bit = 64 hex digits key or 8 to 63-chars ASCII passphrases?
	switch (param) {
	case PSP_NETPARAM_NAME:
		if (!Memory::IsValidRange(dataAddr, APCTL_PROFILENAME_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, name, APCTL_PROFILENAME_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_PROFILENAME_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_SSID:
		if (!Memory::IsValidRange(dataAddr, APCTL_SSID_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.ssid, APCTL_SSID_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_SSID_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_SECURE:
		// 0 is no security.
		// 1 is WEP (64-bit).
		// 2 is WEP (128-bit).
		// 3 is WPA (256-bit ?).
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(1, dataAddr); // WEP 64-bit
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_WEPKEY:
		// WEP 64-bit = 10 hex digits key or 5-digit ASCII equivalent
		// WEP 128-bit = 26 hex digits key or 13-digit ASCII equivalent
		// WEP 256-bit = 58 hex digits key or 29-digit ASCII equivalent
		// WPA 256-bit = 64 hex digits key or 8 to 63-chars ASCII passphrases?
		if (!Memory::IsValidRange(dataAddr, 5))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyWEPKey, 5);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 5, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_IS_STATIC_IP:
		// 0 is DHCP.
		// 1 is static.
		// 2 is PPPOE.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(1, dataAddr);  // static IP
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_IP:
		if (!Memory::IsValidRange(dataAddr, APCTL_IPADDR_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.ip, APCTL_IPADDR_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_IPADDR_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_NETMASK:
		if (!Memory::IsValidRange(dataAddr, APCTL_IPADDR_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.subNetMask, APCTL_IPADDR_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_IPADDR_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_ROUTE:
		if (!Memory::IsValidRange(dataAddr, APCTL_IPADDR_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.gateway, APCTL_IPADDR_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_IPADDR_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_MANUAL_DNS:
		// 0 is auto.
		// 1 is manual. We always use manual.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(1, dataAddr);  // manual
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_PRIMARYDNS:
		if (!Memory::IsValidRange(dataAddr, APCTL_IPADDR_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.primaryDns, APCTL_IPADDR_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_IPADDR_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_SECONDARYDNS:
		if (!Memory::IsValidRange(dataAddr, APCTL_IPADDR_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.secondaryDns, APCTL_IPADDR_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_IPADDR_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_PROXY_USER:
		// FIXME: Proxy's Username max length = 255 chars?
		if (!Memory::IsValidRange(dataAddr, 255))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyUserPass, 255);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 255, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_PROXY_PASS:
		// FIXME: Proxy's Password max length = 255 chars?
		if (!Memory::IsValidRange(dataAddr, 255))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyUserPass, 255);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 255, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_USE_PROXY:
		// 0 is to not use proxy.
		// 1 is to use proxy.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(netApctlInfo.useProxy, dataAddr);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_PROXY_SERVER:
		if (!Memory::IsValidRange(dataAddr, APCTL_URL_MAXLEN))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, netApctlInfo.proxyUrl, APCTL_URL_MAXLEN);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, APCTL_URL_MAXLEN, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_PROXY_PORT:
		if (!Memory::IsValidRange(dataAddr, 2))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U16(netApctlInfo.proxyPort, dataAddr);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 2, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_VERSION:
		// 0 is not used.
		// 1 is old version.
		// 2 is new version.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(2, dataAddr);  // new version
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_UNKNOWN:
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(0, dataAddr);  // reserved?
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		DEBUG_LOG(Log::sceUtility, "sceUtilityGetNetParam - Unknown Param(%d)", param);
		break;
	case PSP_NETPARAM_8021X_AUTH_TYPE:
		// 0 is none.
		// 1 is EAP (MD5).
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(netApctlInfo.eapType, dataAddr);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_8021X_USER:
		// FIXME: 8021X's Username max length = 255 chars?
		if (!Memory::IsValidRange(dataAddr, 255))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyUserPass, 255);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 255, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_8021X_PASS:
		// FIXME: 8021X's Password max length = 255 chars?
		if (!Memory::IsValidRange(dataAddr, 255))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyUserPass, 255);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 255, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_WPA_TYPE:
		// 0 is key in hexadecimal format.
		// 1 is key in ASCII format.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(1, dataAddr);  // ASCII format
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_WPA_KEY:
		// FIXME: WPA 256-bit = 64 hex digits key or 8 to 63-chars ASCII passphrases?
		if (!Memory::IsValidRange(dataAddr, 63))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::MemcpyUnchecked(dataAddr, dummyWPAKey, 63);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 63, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_BROWSER:
		// 0 is to not start the native browser.
		// 1 is to start the native browser.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(netApctlInfo.startBrowser, dataAddr);
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	case PSP_NETPARAM_WIFI_CONFIG:
		// 0 is no config.
		// 1 is unknown. (WISP ?)
		// 2 is Playstation Spot.
		// 3 is unknown.
		if (!Memory::IsValidRange(dataAddr, 4))
			return hleLogError(Log::sceNet, -1, "invalid arg");
		Memory::WriteUnchecked_U32(0, dataAddr);  // no config / netApctlInfo.wifisp ?
		NotifyMemInfo(MemBlockFlags::WRITE, dataAddr, 4, "UtilityGetNetParam");
		break;
	default:
		return hleLogWarning(Log::sceUtility, SCE_ERROR_NETPARAM_BAD_PARAM, "invalid param=%d", param);
	}

	return hleLogDebug(Log::sceUtility, 0);
}

/**
* Get Current Net Configuration ID
*
* @param idAddr - Address to store the current net ID (ie. The actual Net Config ID when using ID=0 on sceUtilityGetNetParam ?)
* @return 0 on success
*/
static int sceUtilityGetNetParamLatestID(u32 idAddr) {
	DEBUG_LOG(Log::sceUtility, "sceUtilityGetNetParamLatestID(%08x)", idAddr);
	// This function is saving the last net param ID (non-zero ID?) and not the number of net configurations.
	Memory::Write_U32(netParamLatestId, idAddr);

	return 0;
}


//TODO: Implement all sceUtilityScreenshot* for real, it doesn't seem to be complex
//but it requires more investigation
static int sceUtilityScreenshotInitStart(u32 paramAddr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::SCREENSHOT) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::SCREENSHOT);
	return hleReportWarning(Log::sceUtility, screenshotDialog->Init(paramAddr));
}

static int sceUtilityScreenshotShutdownStart() {
	if (currentDialogType != UtilityDialogType::SCREENSHOT) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogWarning(Log::sceUtility, screenshotDialog->Shutdown());
}

static int sceUtilityScreenshotUpdate(u32 animSpeed) {
	if (currentDialogType != UtilityDialogType::SCREENSHOT) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogWarning(Log::sceUtility, screenshotDialog->Update(animSpeed));
}

static int sceUtilityScreenshotGetStatus() {
	if (currentDialogType != UtilityDialogType::SCREENSHOT) {
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = screenshotDialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogWarning(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}

static int sceUtilityScreenshotContStart(u32 paramAddr) {
	if (currentDialogType != UtilityDialogType::SCREENSHOT) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogWarning(Log::sceUtility, screenshotDialog->ContStart());
}

static int sceUtilityGamedataInstallInitStart(u32 paramsAddr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::GAMEDATAINSTALL) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::GAMEDATAINSTALL);
	int result = gamedataInstallDialog->Init(paramsAddr);
	if (result < 0)
		DeactivateDialog();
	return hleLogInfo(Log::sceUtility, result);
}

static int sceUtilityGamedataInstallShutdownStart() {
	if (!currentDialogActive || currentDialogType != UtilityDialogType::GAMEDATAINSTALL) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, gamedataInstallDialog->Shutdown());
}

static int sceUtilityGamedataInstallUpdate(int animSpeed) {
	if (!currentDialogActive || currentDialogType != UtilityDialogType::GAMEDATAINSTALL) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogDebug(Log::sceUtility, gamedataInstallDialog->Update(animSpeed));
}

static int sceUtilityGamedataInstallGetStatus() {
	if (currentDialogType != UtilityDialogType::GAMEDATAINSTALL) {
		// This is called incorrectly all the time by some games. So let's not bother warning.
		hleEatCycles(200);
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = gamedataInstallDialog->GetStatus();
	CleanupDialogThreads();
	return hleLogDebug(Log::sceUtility, status);
}

static int sceUtilityGamedataInstallAbort() {
	if (!currentDialogActive || currentDialogType != UtilityDialogType::GAMEDATAINSTALL) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, gamedataInstallDialog->Abort());
}

//TODO: should save to config file
static u32 sceUtilitySetSystemParamString(u32 id, u32 strPtr)
{
	WARN_LOG_REPORT(Log::sceUtility, "sceUtilitySetSystemParamString(%i, %08x)", id, strPtr);
	return 0;
}

static u32 sceUtilityGetSystemParamString(u32 id, u32 destAddr, int destSize) {
	if (!Memory::IsValidRange(destAddr, destSize)) {
		// TODO: What error code?
		return hleLogError(Log::sceUtility, -1);
	}
	DEBUG_LOG(Log::sceUtility, "sceUtilityGetSystemParamString(%i, %08x, %i)", id, destAddr, destSize);
	char *buf = (char *)Memory::GetPointerWriteUnchecked(destAddr);
	switch (id) {
	case PSP_SYSTEMPARAM_ID_STRING_NICKNAME:
		// If there's not enough space for the string and null terminator, fail.
		if (destSize <= (int)g_Config.sNickName.length())
			return SCE_ERROR_UTILITY_STRING_TOO_LONG;
		// TODO: should we zero-pad the output as strncpy does? And what are the semantics for the terminating null if destSize == length?
		strncpy(buf, g_Config.sNickName.c_str(), destSize);
		break;

	default:
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}

	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceUtilitySetSystemParamInt(u32 id, u32 value) {
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL:
		if (value != 0 && value != 1 && value != 6 && value != 11) {
			return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_ADHOC_CHANNEL);
		}
		// Save the setting? We don't really care about this one.
		break;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE:
		break;
	default:
		// PSP can only set above int parameters
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}
	return hleLogDebug(Log::sceUtility, 0);
}

static u32 sceUtilityGetSystemParamInt(u32 id, u32 destaddr) {
	u32 param = 0;
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL:
		param = g_Config.iWlanAdhocChannel;
		if (param == PSP_SYSTEMPARAM_ADHOC_CHANNEL_AUTOMATIC) {
			// FIXME: Actually.. it's always returning 0x800ADF4 regardless using Auto channel or Not, and regardless the connection state either,
			//        Not sure whether this error code only returned after Adhocctl Initialized (ie. netAdhocctlInited) or also before initialized.
			// FIXME: Outputted channel (might be unchanged?) either 0 when not connected to a group yet (ie. adhocctlState == ADHOCCTL_STATE_DISCONNECTED),
			//        or -1 (0xFFFFFFFF) when a scan is in progress (ie. adhocctlState == ADHOCCTL_STATE_SCANNING),
			//        or 0x60 early when in connected state (ie. adhocctlState == ADHOCCTL_STATE_CONNECTED) right after Creating a group, regardless the channel settings.
			Memory::Write_U32(param, destaddr);
			return 0x800ADF4;
		}
		break;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE:
		param = g_Config.bWlanPowerSave?PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON:PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF;
		break;
	case PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT:
		param = g_Config.iDateFormat;
		break;
	case PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT:
		if (g_Config.iTimeFormat == PSP_SYSTEMPARAM_TIME_FORMAT_12HR)
			param = PSP_SYSTEMPARAM_TIME_FORMAT_12HR;
		else
			param = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
		break;
	case PSP_SYSTEMPARAM_ID_INT_TIMEZONE:
		param = g_Config.iTimeZone;
		break;
	case PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS:
		param = g_Config.bDayLightSavings?PSP_SYSTEMPARAM_DAYLIGHTSAVINGS_SAVING:PSP_SYSTEMPARAM_DAYLIGHTSAVINGS_STD;
		break;
	case PSP_SYSTEMPARAM_ID_INT_LANGUAGE:
		param = g_Config.GetPSPLanguage();
		if (PSP_CoreParameter().compat.flags().EnglishOrJapaneseOnly) {
			if (param != PSP_SYSTEMPARAM_LANGUAGE_ENGLISH && param != PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
				param = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
			}
		}
		break;
	case PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE:
		param = PSP_CoreParameter().compat.flags().ForceCircleButtonConfirm ? PSP_SYSTEMPARAM_BUTTON_CIRCLE : g_Config.iButtonPreference;
		break;
	case PSP_SYSTEMPARAM_ID_INT_LOCK_PARENTAL_LEVEL:
		param = g_Config.iLockParentalLevel;
		break;
	default:
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}

	Memory::Write_U32(param, destaddr);
	return hleLogInfo(Log::sceUtility, 0, "param: %08x", param);
}

static u32 sceUtilityLoadNetModule(u32 module) {
	return hleLogInfo(Log::sceUtility, 0, "FAKE");
}

static u32 sceUtilityUnloadNetModule(u32 module) {
	return hleLogInfo(Log::sceUtility, 0, "FAKE");
}

static int sceUtilityNpSigninInitStart(u32 paramsPtr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::NPSIGNIN) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	ActivateDialog(UtilityDialogType::NPSIGNIN);
	return hleLogInfo(Log::sceUtility, npSigninDialog->Init(paramsPtr));
}

static int sceUtilityNpSigninShutdownStart() {
	if (currentDialogType != UtilityDialogType::NPSIGNIN) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, npSigninDialog->Shutdown());
}

static int sceUtilityNpSigninUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::NPSIGNIN) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogDebug(Log::sceUtility, npSigninDialog->Update(animSpeed));
}

static int sceUtilityNpSigninGetStatus() {
	if (currentDialogType != UtilityDialogType::NPSIGNIN) {
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	int status = npSigninDialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status);
	}
	return hleLogVerbose(Log::sceUtility, status);
}

static void sceUtilityInstallInitStart(u32 unknown) {
	WARN_LOG_REPORT(Log::sceUtility, "UNIMPL sceUtilityInstallInitStart()");
	return hleNoLogVoid();
}

static int sceUtilityStoreCheckoutShutdownStart() {
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static int sceUtilityStoreCheckoutInitStart(u32 paramsPtr) {
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static int sceUtilityStoreCheckoutUpdate(int drawSpeed) {
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static int sceUtilityStoreCheckoutGetStatus() {
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static int sceUtilityGameSharingShutdownStart() {
	if (currentDialogType != UtilityDialogType::GAMESHARING) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	DeactivateDialog();
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static int sceUtilityGameSharingInitStart(u32 paramsPtr) {
	if (currentDialogActive && currentDialogType != UtilityDialogType::GAMESHARING) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE);
	}

	ActivateDialog(UtilityDialogType::GAMESHARING);
	ERROR_LOG_REPORT(Log::sceUtility, "UNIMPL sceUtilityGameSharingInitStart(%08x)", paramsPtr);
	return hleNoLog(0);
}

static int sceUtilityGameSharingUpdate(int animSpeed) {
	if (currentDialogType != UtilityDialogType::GAMESHARING) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	return hleLogError(Log::sceUtility, 0, "UNIMPL sceUtilityGameSharingUpdate(%i)", animSpeed);
}

static int sceUtilityGameSharingGetStatus() {
	if (currentDialogType != UtilityDialogType::GAMESHARING) {
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	}

	CleanupDialogThreads();
	return hleLogError(Log::sceUtility, 0, "UNIMPL");
}

static u32 sceUtilityLoadUsbModule(u32 module)
{
	if (module < 1 || module > 5)
	{
		ERROR_LOG(Log::sceUtility, "sceUtilityLoadUsbModule(%i): invalid module id", module);
	}

	ERROR_LOG_REPORT(Log::sceUtility, "UNIMPL sceUtilityLoadUsbModule(%i)", module);
	return hleNoLog(0);
}

static u32 sceUtilityUnloadUsbModule(u32 module)
{
	if (module < 1 || module > 5)
	{
		ERROR_LOG(Log::sceUtility, "sceUtilityUnloadUsbModule(%i): invalid module id", module);
	}

	ERROR_LOG_REPORT(Log::sceUtility, "UNIMPL sceUtilityUnloadUsbModule(%i)", module);
	return hleNoLog(0);
}

const HLEFunction sceUtility[] =
{
	{0X1579A159, &WrapU_U<sceUtilityLoadNetModule>,                "sceUtilityLoadNetModule",                'x', "x"  },
	{0X64D50C56, &WrapU_U<sceUtilityUnloadNetModule>,              "sceUtilityUnloadNetModule",              'x', "x"  },

	{0XF88155F6, &WrapI_V<sceUtilityNetconfShutdownStart>,         "sceUtilityNetconfShutdownStart",         'i', ""   },
	{0X4DB1E739, &WrapI_U<sceUtilityNetconfInitStart>,             "sceUtilityNetconfInitStart",             'i', "x"  },
	{0X91E70E35, &WrapI_I<sceUtilityNetconfUpdate>,                "sceUtilityNetconfUpdate",                'i', "i"  },
	{0X6332AA39, &WrapI_V<sceUtilityNetconfGetStatus>,             "sceUtilityNetconfGetStatus",             'i', ""   },
	{0X5EEE6548, &WrapI_I<sceUtilityCheckNetParam>,                "sceUtilityCheckNetParam",                'i', "i"  },
	{0X434D4B3A, &WrapI_IIU<sceUtilityGetNetParam>,                "sceUtilityGetNetParam",                  'i', "iix"},
	{0X4FED24D8, &WrapI_U<sceUtilityGetNetParamLatestID>,          "sceUtilityGetNetParamLatestID",          'i', "x"  },

	{0X67AF3428, &WrapI_V<sceUtilityMsgDialogShutdownStart>,       "sceUtilityMsgDialogShutdownStart",       'i', ""   },
	{0X2AD8E239, &WrapI_U<sceUtilityMsgDialogInitStart>,           "sceUtilityMsgDialogInitStart",           'i', "x"  },
	{0X95FC253B, &WrapI_I<sceUtilityMsgDialogUpdate>,              "sceUtilityMsgDialogUpdate",              'i', "i"  },
	{0X9A1C91D7, &WrapI_V<sceUtilityMsgDialogGetStatus>,           "sceUtilityMsgDialogGetStatus",           'i', ""   },
	{0X4928BD96, &WrapI_V<sceUtilityMsgDialogAbort>,               "sceUtilityMsgDialogAbort",               'i', ""   },

	{0X9790B33C, &WrapI_V<sceUtilitySavedataShutdownStart>,        "sceUtilitySavedataShutdownStart",        'i', ""   },
	{0X50C4CD57, &WrapI_U<sceUtilitySavedataInitStart>,            "sceUtilitySavedataInitStart",            'i', "x"  },
	{0XD4B95FFB, &WrapI_I<sceUtilitySavedataUpdate>,               "sceUtilitySavedataUpdate",               'i', "i"  },
	{0X8874DBE0, &WrapI_V<sceUtilitySavedataGetStatus>,            "sceUtilitySavedataGetStatus",            'i', ""   },

	{0X3DFAEBA9, &WrapI_V<sceUtilityOskShutdownStart>,             "sceUtilityOskShutdownStart",             'i', ""   },
	{0XF6269B82, &WrapI_U<sceUtilityOskInitStart>,                 "sceUtilityOskInitStart",                 'i', "x"  },
	{0X4B85C861, &WrapI_I<sceUtilityOskUpdate>,                    "sceUtilityOskUpdate",                    'i', "i"  },
	{0XF3F76017, &WrapI_V<sceUtilityOskGetStatus>,                 "sceUtilityOskGetStatus",                 'i', ""   },

	{0X41E30674, &WrapU_UU<sceUtilitySetSystemParamString>,        "sceUtilitySetSystemParamString",         'x', "xx" },
	{0X45C18506, &WrapU_UU<sceUtilitySetSystemParamInt>,           "sceUtilitySetSystemParamInt",            'x', "xx"   },
	{0X34B78343, &WrapU_UUI<sceUtilityGetSystemParamString>,       "sceUtilityGetSystemParamString",         'x', "xxi"},
	{0XA5DA2406, &WrapU_UU<sceUtilityGetSystemParamInt>,           "sceUtilityGetSystemParamInt",            'x', "xx" },


	{0XC492F751, &WrapI_U<sceUtilityGameSharingInitStart>,         "sceUtilityGameSharingInitStart",         'i', "x"  },
	{0XEFC6F80F, &WrapI_V<sceUtilityGameSharingShutdownStart>,     "sceUtilityGameSharingShutdownStart",     'i', ""   },
	{0X7853182D, &WrapI_I<sceUtilityGameSharingUpdate>,            "sceUtilityGameSharingUpdate",            'i', "i"  },
	{0X946963F3, &WrapI_V<sceUtilityGameSharingGetStatus>,         "sceUtilityGameSharingGetStatus",         'i', ""   },

	{0X2995D020, nullptr,                                          "sceUtilitySavedataErrInitStart",         '?', ""   },
	{0XB62A4061, nullptr,                                          "sceUtilitySavedataErrShutdownStart",     '?', ""   },
	{0XED0FAD38, nullptr,                                          "sceUtilitySavedataErrUpdate",            '?', ""   },
	{0X88BC7406, nullptr,                                          "sceUtilitySavedataErrGetStatus",         '?', ""   },

	{0XBDA7D894, nullptr,                                          "sceUtilityHtmlViewerGetStatus",          '?', ""   },
	{0XCDC3AA41, nullptr,                                          "sceUtilityHtmlViewerInitStart",          '?', ""   },
	{0XF5CE1134, nullptr,                                          "sceUtilityHtmlViewerShutdownStart",      '?', ""   },
	{0X05AFB9E4, nullptr,                                          "sceUtilityHtmlViewerUpdate",             '?', ""   },

	{0X16A1A8D8, nullptr,                                          "sceUtilityAuthDialogGetStatus",          '?', ""   },
	{0X943CBA46, nullptr,                                          "sceUtilityAuthDialogInitStart",          '?', ""   },
	{0X0F3EEAAC, nullptr,                                          "sceUtilityAuthDialogShutdownStart",      '?', ""   },
	{0X147F7C85, nullptr,                                          "sceUtilityAuthDialogUpdate",             '?', ""   },

	{0XC629AF26, &WrapU_U<sceUtilityLoadAvModule>,                 "sceUtilityLoadAvModule",                 'x', "x"  },
	{0XF7D8D092, &WrapU_U<sceUtilityUnloadAvModule>,               "sceUtilityUnloadAvModule",               'x', "x"  },

	{0X2A2B3DE0, &WrapU_U<sceUtilityLoadModule>,                   "sceUtilityLoadModule",                   'x', "x"  },
	{0XE49BFE92, &WrapU_U<sceUtilityUnloadModule>,                 "sceUtilityUnloadModule",                 'x', "x"  },

	{0X0251B134, &WrapI_U<sceUtilityScreenshotInitStart>,          "sceUtilityScreenshotInitStart",          'i', "x"  },
	{0XF9E0008C, &WrapI_V<sceUtilityScreenshotShutdownStart>,      "sceUtilityScreenshotShutdownStart",      'i', ""   },
	{0XAB083EA9, &WrapI_U<sceUtilityScreenshotUpdate>,             "sceUtilityScreenshotUpdate",             'i', "i"  },
	{0XD81957B7, &WrapI_V<sceUtilityScreenshotGetStatus>,          "sceUtilityScreenshotGetStatus",          'i', ""   },
	{0X86A03A27, &WrapI_U<sceUtilityScreenshotContStart>,          "sceUtilityScreenshotContStart",          'i', "x"  },

	{0X0D5BC6D2, &WrapU_U<sceUtilityLoadUsbModule>,                "sceUtilityLoadUsbModule",                'x', "x"  },
	{0XF64910F0, &WrapU_U<sceUtilityUnloadUsbModule>,              "sceUtilityUnloadUsbModule",              'x', "x"  },

	{0X24AC31EB, &WrapI_U<sceUtilityGamedataInstallInitStart>,     "sceUtilityGamedataInstallInitStart",     'i', "x"  },
	{0X32E32DCB, &WrapI_V<sceUtilityGamedataInstallShutdownStart>, "sceUtilityGamedataInstallShutdownStart", 'i', ""   },
	{0X4AECD179, &WrapI_I<sceUtilityGamedataInstallUpdate>,        "sceUtilityGamedataInstallUpdate",        'i', "i"  },
	{0XB57E95D9, &WrapI_V<sceUtilityGamedataInstallGetStatus>,     "sceUtilityGamedataInstallGetStatus",     'i', ""   },
	{0X180F7B62, &WrapI_V<sceUtilityGamedataInstallAbort>,         "sceUtilityGamedataInstallAbort",         'i', ""   },

	{0X16D02AF0, &WrapI_U<sceUtilityNpSigninInitStart>,            "sceUtilityNpSigninInitStart",            'i', "x"  },
	{0XE19C97D6, &WrapI_V<sceUtilityNpSigninShutdownStart>,        "sceUtilityNpSigninShutdownStart",        'i', ""   },
	{0XF3FBC572, &WrapI_I<sceUtilityNpSigninUpdate>,               "sceUtilityNpSigninUpdate",               'i', "i"  },
	{0X86ABDB1B, &WrapI_V<sceUtilityNpSigninGetStatus>,            "sceUtilityNpSigninGetStatus",            'i', ""   },

	{0X1281DA8E, &WrapV_U<sceUtilityInstallInitStart>,             "sceUtilityInstallInitStart",             'v', "x"  },
	{0X5EF1C24A, nullptr,                                          "sceUtilityInstallShutdownStart",         '?', ""   },
	{0XA03D29BA, nullptr,                                          "sceUtilityInstallUpdate",                '?', ""   },
	{0XC4700FA3, nullptr,                                          "sceUtilityInstallGetStatus",             '?', ""   },

	{0X54A5C62F, &WrapI_V<sceUtilityStoreCheckoutShutdownStart>,   "sceUtilityStoreCheckoutShutdownStart",   'i', ""   },
	{0XDA97F1AA, &WrapI_U<sceUtilityStoreCheckoutInitStart>,       "sceUtilityStoreCheckoutInitStart",       'i', "x"  },
	{0XB8592D5F, &WrapI_I<sceUtilityStoreCheckoutUpdate>,          "sceUtilityStoreCheckoutUpdate",          'i', "i"  },
	{0X3AAD51DC, &WrapI_V<sceUtilityStoreCheckoutGetStatus>,       "sceUtilityStoreCheckoutGetStatus",       'i', ""   },

	{0XD17A0573, nullptr,                                          "sceUtilityPS3ScanShutdownStart",         '?', ""   },
	{0X42071A83, nullptr,                                          "sceUtilityPS3ScanInitStart",             '?', ""   },
	{0XD852CDCE, nullptr,                                          "sceUtilityPS3ScanUpdate",                '?', ""   },
	{0X89317C8F, nullptr,                                          "sceUtilityPS3ScanGetStatus",             '?', ""   },

	{0XE1BC175E, nullptr,                                          "sceUtility_E1BC175E",                    '?', ""   },
	{0X43E521B7, nullptr,                                          "sceUtility_43E521B7",                    '?', ""   },
	{0XDB4149EE, nullptr,                                          "sceUtility_DB4149EE",                    '?', ""   },
	{0XCFE7C460, nullptr,                                          "sceUtility_CFE7C460",                    '?', ""   },

	{0XC130D441, nullptr,                                          "sceUtilityPsnShutdownStart",             '?', ""   },
	{0XA7BB7C67, nullptr,                                          "sceUtilityPsnInitStart",                 '?', ""   },
	{0X0940A1B9, nullptr,                                          "sceUtilityPsnUpdate",                    '?', ""   },
	{0X094198B8, nullptr,                                          "sceUtilityPsnGetStatus",                 '?', ""   },

	{0X9F313D14, nullptr,                                          "sceUtilityAutoConnectShutdownStart",     '?', ""   },
	{0X3A15CD0A, nullptr,                                          "sceUtilityAutoConnectInitStart",         '?', ""   },
	{0XD23665F4, nullptr,                                          "sceUtilityAutoConnectUpdate",            '?', ""   },
	{0XD4C2BD73, nullptr,                                          "sceUtilityAutoConnectGetStatus",         '?', ""   },
	{0X0E0C27AF, nullptr,                                          "sceUtilityAutoConnectAbort",             '?', ""   },

	{0X06A48659, nullptr,                                          "sceUtilityRssSubscriberShutdownStart",   '?', ""   },
	{0X4B0A8FE5, nullptr,                                          "sceUtilityRssSubscriberInitStart",       '?', ""   },
	{0XA084E056, nullptr,                                          "sceUtilityRssSubscriberUpdate",          '?', ""   },
	{0X2B96173B, nullptr,                                          "sceUtilityRssSubscriberGetStatus",       '?', ""   },

	{0X149A7895, nullptr,                                          "sceUtilityDNASShutdownStart",            '?', ""   },
	{0XDDE5389D, nullptr,                                          "sceUtilityDNASInitStart",                '?', ""   },
	{0X4A833BA4, nullptr,                                          "sceUtilityDNASUpdate",                   '?', ""   },
	{0XA50E5B30, nullptr,                                          "sceUtilityDNASGetStatus",                '?', ""   },

	{0XE7B778D8, nullptr,                                          "sceUtilityRssReaderShutdownStart",       '?', ""   },
	{0X81C44706, nullptr,                                          "sceUtilityRssReaderInitStart",           '?', ""   },
	{0X6F56F9CF, nullptr,                                          "sceUtilityRssReaderUpdate",              '?', ""   },
	{0X8326AB05, nullptr,                                          "sceUtilityRssReaderGetStatus",           '?', ""   },
	{0XB0FB7FF5, nullptr,                                          "sceUtilityRssReaderContStart",           '?', ""   },

	{0XBC6B6296, nullptr,                                          "sceNetplayDialogShutdownStart",          '?', ""   },
	{0X3AD50AE7, nullptr,                                          "sceNetplayDialogInitStart",              '?', ""   },
	{0X417BED54, nullptr,                                          "sceNetplayDialogUpdate",                 '?', ""   },
	{0XB6CEE597, nullptr,                                          "sceNetplayDialogGetStatus",              '?', ""   },

	{0X28D35634, nullptr,                                          "sceUtility_28D35634",                    '?', ""   },
	{0X70267ADF, nullptr,                                          "sceUtility_70267ADF",                    '?', ""   },
	{0XECE1D3E5, nullptr,                                          "sceUtility_ECE1D3E5",                    '?', ""   },
	{0XEF3582B2, nullptr,                                          "sceUtility_EF3582B2",                    '?', ""   },

	// Fake functions for PPSSPP's use.
	{0xC0DE0001, &WrapI_I<UtilityFinishDialog>,                    "__UtilityFinishDialog",                  'i', "i"  },
	{0xC0DE0002, &WrapI_I<UtilityWorkUs>,                          "__UtilityWorkUs",                        'i', "i"  },
	{0xC0DE0003, &WrapI_I<UtilityInitDialog>,                      "__UtilityInitDialog",                    'i', "i"  },
};

void Register_sceUtility()
{
	RegisterHLEModule("sceUtility", ARRAY_SIZE(sceUtility), sceUtility);
}
