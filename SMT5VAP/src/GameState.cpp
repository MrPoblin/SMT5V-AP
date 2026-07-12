#include "GameState.hpp"
#include "HookHelper.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <Windows.h>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

void GameState::Update() {
	if (UpdateWorld()) {
		UpdateMapName();
		UpdatePosition();
	}
}

bool GameState::UpdateWorld() {
	UWorld* newWorld = static_cast<UWorld*>(UObjectGlobals::FindFirstOf(STR("World")));
	if ((m_World == nullptr) != (newWorld == nullptr)) {
		m_World = newWorld;
		for (auto& cb : m_WorldCbs) cb(m_World);
		return m_World;
	}
	m_World = newWorld;
	return m_World;
}

void GameState::UpdateMapName() {
	if (const std::wstring& newMapName{ m_World->GetName() }; newMapName != m_MapName) {
		m_MapName = newMapName;
		for (auto& cb : m_MapCbs) cb(m_MapName);
		if (m_MapName == L"LV_Title" && m_IsSaveLoaded) { 
			m_IsSaveLoaded = false;
			for (auto& cb : m_SaveCbs) cb(false); }
	}
}


void GameState::UpdatePosition() {
	if (!m_World) return;

	static UFunction* s_Fn = nullptr;
	if (!s_Fn) {
		s_Fn = UObjectGlobals::FindObject<UFunction>(nullptr,
			STR("/Script/Engine.Actor:K2_GetActorLocation"));
		if (!s_Fn) return;
	}

	// Find all PlayerBase_C instances and skip the CDO (no world)
	std::vector<UObject*> players;
	UObjectGlobals::FindAllOf(STR("PlayerBase_C"), players);
	UObject* Player = nullptr;
	for (auto* p : players) {
		if (p && p->GetWorld()) { Player = p; break; }
	}
	if (!Player) return;

	// K2_GetActorLocation takes no params, returns FVector (12 bytes)
	struct { float X, Y, Z; } Result{};
	Player->ProcessEvent(s_Fn, &Result);
	m_PosX = Result.X;
	m_PosY = Result.Y;
	m_PosZ = Result.Z;
}


void GameState::SetupSaveLoadedHook() {
	const auto* Path = STR("/Script/Project.SaveLoadBase:StartDataLoad");
	HookHelper::HookPostBool(Path, [](bool isLoaded) {
		m_IsSaveLoaded = true;
		for (auto& cb : m_SaveCbs) cb(true);
		});
}
