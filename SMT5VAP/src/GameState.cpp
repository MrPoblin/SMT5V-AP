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
	if (UpdateWorld()) UpdateMapName();
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


void GameState::SetupSaveLoadedHook() {
	const auto* Path = STR("/Script/Project.SaveLoadBase:StartDataLoad");
	HookHelper::HookPostBool(Path, [](bool isLoaded) {
		m_IsSaveLoaded = true;
		for (auto& cb : m_SaveCbs) cb(true);
		});
}
