#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"

#if OBLIVION
#include "obse/GameAPI.h"

#else
#include "obse_editor/EditorAPI.h"
#endif

#include "obse/ParamInfos.h"
#include "obse/Script.h"
#include "obse/GameObjects.h"
#include "obse_common/SafeWrite.cpp"

#include <string>
#include <shared_mutex>

#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

IDebugLog		gLog("NorthernUIRumble.log");

PluginHandle				g_pluginHandle = kPluginHandle_Invalid;

OBSEMessagingInterface* msgIntfc;
OBSEEventManagerInterface* evntIntfc;
OBSETasksInterface* tskIntfc;

struct TimeInfo
{
	UInt8	disableCount;	// 00
	UInt8	pad1[3];		// 01
	float	fpsClamp;		// 04 - in seconds
	float	unk08;			// 08
	float	frameTime;		// 0C - in seconds
	float	unk10;			// 10
	UInt32	gameStartTime;	// 14
};

TimeInfo* g_timeInfo = (TimeInfo*)0x00B33E90;

bool init = false;

auto fRumbleBlockStrength = 0.5;
auto fRumbleBlockTime = 0.3;
auto fRumbleHitBlockedStrength = 1.0;
auto fRumbleHitBlockedTime = 0.3;
auto fRumbleHitStrength = 0.5;
auto fRumbleHitTime = 0.3;
auto fRumblePainStrength = 0.75;
auto fRumblePainTime = 0.5;
auto fRumbleStruckStrength = 0.75;
auto fRumbleStruckTime = 0.3;

std::shared_mutex rumbleStateLock;

struct RumbleState {
	float fRumbleBlockTime = 0;
	float fRumbleHitBlockedTime = 0;
	float fRumbleHitTime = 0;
	float fRumblePainTime = 0;
	float fRumbleStruckTime = 0;
};

RumbleState rumbleState;
bool registeredForUpdate = false;

DWORD gamepadIndex = -1;

void GetConnectedGamepad()
{
	DWORD dwResult;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		XINPUT_STATE state;
		ZeroMemory(&state, sizeof(XINPUT_STATE));

		// Simply get the state of the controller from XInput.
		dwResult = XInputGetState(i, &state);

		if (dwResult == ERROR_SUCCESS)
		{
			// Controller is connected
			gamepadIndex = i;
			return;
		}
		else
		{
			// Controller is not connected
		}
	}
}

void SetRumble(float percent)
{
	XINPUT_VIBRATION vibration;
	UInt16 speed = static_cast<UInt16>(65535 * percent);
	vibration.wLeftMotorSpeed = speed;
	vibration.wRightMotorSpeed = speed;
	XInputSetState(gamepadIndex, &vibration);
}

void RumbleUpdate() {
	auto interfaceManager = InterfaceManager::GetSingleton();
	if ((interfaceManager && !interfaceManager->IsGameMode()) || !registeredForUpdate) {
		SetRumble(0);
		return;
	}

	std::lock_guard<std::shared_mutex> lk(rumbleStateLock);

	float rumble = 0;
	rumble += fRumblePainStrength * (rumbleState.fRumblePainTime / fRumblePainTime);
	rumble += fRumbleHitBlockedStrength * (rumbleState.fRumbleHitBlockedTime / fRumbleHitBlockedTime);
	rumble += fRumbleHitStrength * (rumbleState.fRumbleHitTime / fRumbleHitTime);
	rumble += fRumbleBlockStrength * (rumbleState.fRumbleBlockTime / fRumbleBlockTime);
	rumble += fRumbleStruckStrength * (rumbleState.fRumbleStruckTime / fRumbleStruckTime);
	rumble = min(rumble, 1.0f);
	SetRumble(rumble);

	float delta = g_timeInfo->frameTime;
	rumbleState.fRumbleBlockTime = max(0, rumbleState.fRumbleBlockTime - delta);
	rumbleState.fRumbleHitBlockedTime = max(0, rumbleState.fRumbleHitBlockedTime - delta);
	rumbleState.fRumbleHitTime = max(0, rumbleState.fRumbleHitTime - delta);
	rumbleState.fRumblePainTime = max(0, rumbleState.fRumblePainTime - delta);
	rumbleState.fRumbleStruckTime = max(0, rumbleState.fRumbleStruckTime - delta);

	if (!rumble)
		registeredForUpdate = false;

}

enum class RumbleType {
	Block,
	HitBlocked,
	Hit,
	Pain,
	Struck
};

void ScheduleRumbleUpdate(RumbleType type) {
	std::lock_guard<std::shared_mutex> lk(rumbleStateLock);
	switch (type) {
	case RumbleType::Block:
		rumbleState.fRumbleBlockTime = fRumbleBlockTime;
		break;
	case RumbleType::HitBlocked:
		rumbleState.fRumbleHitBlockedTime = fRumbleHitBlockedTime;
		break;
	case RumbleType::Hit:
		rumbleState.fRumbleHitTime = fRumbleHitTime;
		break;
	case RumbleType::Pain:
		rumbleState.fRumblePainTime = fRumblePainTime;
		break;
	case RumbleType::Struck:
		rumbleState.fRumbleStruckTime = fRumbleStruckTime;
		break;
	}
	if (!registeredForUpdate) {
		registeredForUpdate = true;
	}
}

void RumbleAsync() {
	while (true) {
		float time = g_timeInfo->frameTime;
		if (time) {
			RumbleUpdate();
			std::this_thread::sleep_for(std::chrono::milliseconds((int)(g_timeInfo->frameTime * 1000)));
		}
		else
			std::this_thread::sleep_for(std::chrono::milliseconds((int)(1000)));
	}
}

static HighProcess* ExtractHighProcess(TESObjectREFR* thisObj)
{
	HighProcess* hiProc = NULL;
	MobileObject* mob = (MobileObject*)Oblivion_DynamicCast(thisObj, 0, RTTI_TESObjectREFR, RTTI_MobileObject, 0);
	if (mob)
		hiProc = (HighProcess*)Oblivion_DynamicCast(mob->process, 0, RTTI_BaseProcess, RTTI_HighProcess, 0);

	return hiProc;
}

TESObjectWEAP* GetEquippedWeapon(Actor* a_actor) {;
	auto equippedItems = a_actor->GetEquippedItems();
	for (auto item : equippedItems) {
		if (auto weapon = (TESObjectWEAP*)Oblivion_DynamicCast(item, 0, RTTI_TESForm, RTTI_TESObjectWEAP, 0)) {
			return weapon;
		}
	}
}

void OnHit(void* source, void* object, TESObjectREFR* thisObj) {
	TESObjectREFR* attacker = (TESObjectREFR*)object;
	TESObjectREFR* target = (TESObjectREFR*)source;
	if (attacker == *g_thePlayer) {
		if (target) {
			auto actor = (Actor*)Oblivion_DynamicCast(attacker, 0, RTTI_TESObjectREFR, RTTI_Actor, 0);
			if (auto weapon = GetEquippedWeapon(actor)) {
				if (weapon->type == TESObjectWEAP::kType_Staff || weapon->type == TESObjectWEAP::kType_Bow) { // ranged
					return;
				}
			}
			HighProcess* hiProc = ExtractHighProcess(attacker);
			if (hiProc && hiProc->IsBlocking()) {
				_DMESSAGE("Rumble Block");
				ScheduleRumbleUpdate(RumbleType::Block);
				return;
			}
			_DMESSAGE("Rumble Hit");
			ScheduleRumbleUpdate(RumbleType::Hit);
			return;
		}
	}
	else if (target == *g_thePlayer) {
		Actor* actor = (Actor*)Oblivion_DynamicCast(attacker, 0, RTTI_TESObjectREFR, RTTI_Actor, 0);
		if (actor){
			HighProcess* hiProc = ExtractHighProcess(target);
			if (hiProc && hiProc->IsBlocking()) {
				_DMESSAGE("Rumble Block");
				ScheduleRumbleUpdate(RumbleType::Block);
				return;
			}
			_DMESSAGE("Rumble Struck");
			ScheduleRumbleUpdate(RumbleType::Struck);
			return;
		}
		_DMESSAGE("Rumble Pain");
		ScheduleRumbleUpdate(RumbleType::Pain);
		return;
	}
}

void MessageHandler(OBSEMessagingInterface::Message* msg)
{
	switch (msg->type)
	{
	case OBSEMessagingInterface::kMessage_PreLoadGame:
		if (!init) {
			GetConnectedGamepad();
			evntIntfc->RegisterEvent("OnHit", OnHit, nullptr, nullptr, nullptr);
			init = true;
		}
		break;
	}
}

bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "NorthernUIRumble";
	info->version = 1;

	// version checks
	if (!obse->isEditor)
	{
		if (obse->obseVersion < OBSE_VERSION_INTEGER)
		{
			_ERROR("OBSE version too old (got %u expected at least %u)", obse->obseVersion, OBSE_VERSION_INTEGER);
			return false;
		}

#if OBLIVION
		if (obse->oblivionVersion != OBLIVION_VERSION)
		{
			_ERROR("incorrect Oblivion version (got %08X need %08X)", obse->oblivionVersion, OBLIVION_VERSION);
			return false;
		}
#endif
	}

	return true;
}



bool OBSEPlugin_Load(const OBSEInterface* obse)
{
	_MESSAGE("Plugin loaded");
	gLog.SetLogLevel(IDebugLog::LogLevel::kLevel_Message);

	g_pluginHandle = obse->GetPluginHandle();

	if (!obse->isEditor)
	{
		msgIntfc = (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);
		evntIntfc = (OBSEEventManagerInterface*)obse->QueryInterface(kInterface_EventManager);
		tskIntfc = (OBSETasksInterface*)obse->QueryInterface(kInterface_Tasks);

		msgIntfc->RegisterListener(g_pluginHandle, "OBSE", MessageHandler);

		std::jthread asyncRumbleThread(RumbleAsync);
		asyncRumbleThread.detach();
		_MESSAGE("Created rumble thread");
	}

	return true;
};