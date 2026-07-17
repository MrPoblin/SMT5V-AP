#include "GameState.hpp"
#include "HookHelper.hpp"
#include "src/Log/Log.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Windows.h>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

void GameState::UpdateIsSaveLoaded(bool isLoaded) {
	m_IsSaveLoaded = isLoaded;
	LOG("[GameState] Save {}", isLoaded ? L"loaded" : L"unloaded");
	for (auto& cb : m_SaveCbs) cb(isLoaded);
}

void GameState::SetTransitioning(bool value) {
	if (m_IsTransitioning == value) return;
	m_IsTransitioning = value;
	//LOG("[GameState] Transitioning = {}", value ? L"true" : L"false");
}

void GameState::SetupMapLoadHook() {
	RC::Unreal::HookLoadMap();

	Hook::RegisterLoadMapPostCallback(
		[](auto&, UEngine*, FWorldContext&, FURL URL, UPendingNetGame*, FString& Error) {
			std::wstring mapName(URL.Map.GetCharArray().GetData());
			LOG("[GameState] Level loaded: {}", mapName);
			m_MapName = mapName;
			SetTransitioning(false);
			for (auto& cb : m_MapCbs) cb(m_MapName);
			if (m_MapName.contains(L"LV_Title") && m_IsSaveLoaded) {
				UpdateIsSaveLoaded(false);
			}
			else if (m_MapName.contains(L"LV_m202") && !m_IsSaveLoaded) { // New game started
				UpdateIsSaveLoaded(true);
			}
		},
		Hook::FCallbackOptions{});
}

void GameState::SetupTransitionHooks() {
	// Proper transition-start signal: UE4SS hooks UEngine::LoadMap directly, which
	// fires before any world teardown on EVERY map/level change (engine-level, so
	// it does not depend on guessing game-specific Blueprint function names). We
	// raise the flag here and clear it in the LoadMap post-callback.
	Hook::RegisterLoadMapPreCallback(
		[](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
			SetTransitioning(true);
			for (auto& cb : m_TransCbs) cb();
		},
		Hook::FCallbackOptions{});

	LOG("[GameState] Transition-start (LoadMap pre) hook registered");
}

void GameState::UpdatePosition() {
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

	// K2_GetActorLocation returns FVector = 3 floats (12 bytes), which matches Vec3
	Player->ProcessEvent(s_Fn, &m_Pos);
}

void GameState::SetupSaveLoadedHook() {
	const auto* Path = STR("/Script/Project.SaveLoadBase:StartDataLoad");
	HookHelper::HookPostBool(Path, [](bool isLoaded) {
		// Might have to delay the change until a map has loaded
		UpdateIsSaveLoaded(true);
		});
}
