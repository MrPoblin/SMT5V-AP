#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <string>
#include <vector>
#include <functional>

using namespace RC;
using namespace RC::Unreal;

class GameState {
public:
	using WorldCallback = std::function<void(UWorld*)>;
	using MapCallback = std::function<void(const std::wstring& /* mapName */)>;
	using SaveCallback = std::function<void(bool /* isLoaded */)>;

	static void* World() { return m_World; }
	static const std::wstring& MapName() { return m_MapName; }
	static bool IsSaveLoaded() { return m_IsSaveLoaded; }

	static void OnWorldChanged(WorldCallback cb) { m_WorldCbs.push_back(std::move(cb)); }
	static void OnMapChanged(MapCallback cb) { m_MapCbs.push_back(std::move(cb)); }
	static void OnSaveLoaded(SaveCallback cb) { m_SaveCbs.push_back(std::move(cb)); }

	static void Update();

	static void SetupSaveLoadedHook();
private:
	inline static UWorld* m_World{};
	inline static std::wstring m_MapName{};
	inline static bool m_IsSaveLoaded{ false };

	inline static std::vector<WorldCallback> m_WorldCbs;
	inline static std::vector<MapCallback>   m_MapCbs;
	inline static std::vector<SaveCallback>  m_SaveCbs;

	static bool UpdateWorld();
	static void UpdateMapName();
};
