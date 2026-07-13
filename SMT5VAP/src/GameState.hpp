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
	using MapCallback = std::function<void(const std::wstring& /* mapName */)>;
	using SaveCallback = std::function<void(bool /* isLoaded */)>;

	static const std::wstring& MapName() { return m_MapName; }
	static bool IsSaveLoaded() { return m_IsSaveLoaded; }

	static void OnMapChanged(MapCallback cb) { m_MapCbs.push_back(std::move(cb)); }
	static void OnSaveLoaded(SaveCallback cb) { m_SaveCbs.push_back(std::move(cb)); }

	// Position tracking
	struct Vec3 { float X{}, Y{}, Z{}; };
	static const Vec3& Pos() { return m_Pos; }
	static float PosX() { return m_Pos.X; }
	static float PosY() { return m_Pos.Y; }
	static float PosZ() { return m_Pos.Z; }
	static void UpdatePosition();

	static void SetupMapLoadHook();
	static void SetupSaveLoadedHook();

private:
	inline static std::wstring m_MapName{};
	inline static bool m_IsSaveLoaded{ false };
	inline static Vec3 m_Pos{};

	inline static std::vector<MapCallback>   m_MapCbs;
	inline static std::vector<SaveCallback>  m_SaveCbs;
};
