#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace HookHelper {

// ======================================================================
// NATIVE FUNCTION HOOKS (require UFunction to be preloaded)
// ======================================================================

inline UFunction* FindFunc(const wchar_t* FuncPath) {
    return UObjectGlobals::FindObject<UFunction>(nullptr, FuncPath);
}

inline CallbackId HookPre(const wchar_t* FuncPath, UnrealScriptFunctionCallable Cb, void* CustomData = nullptr) {
    if (auto* Fn = FindFunc(FuncPath)) return Fn->RegisterPreHook(std::move(Cb), CustomData);
    return -1;
}

inline CallbackId HookPost(const wchar_t* FuncPath, UnrealScriptFunctionCallable Cb, void* CustomData = nullptr) {
    if (auto* Fn = FindFunc(FuncPath)) return Fn->RegisterPostHook(std::move(Cb), CustomData);
    return -1;
}

inline CallbackId HookPreFor(const wchar_t* FuncPath, UObject* Target, UnrealScriptFunctionCallable Cb, void* CustomData = nullptr) {
    if (auto* Fn = FindFunc(FuncPath)) return Fn->RegisterPreHookForInstance(std::move(Cb), CustomData, Target);
    return -1;
}

inline CallbackId HookPostFor(const wchar_t* FuncPath, UObject* Target, UnrealScriptFunctionCallable Cb, void* CustomData = nullptr) {
    if (auto* Fn = FindFunc(FuncPath)) return Fn->RegisterPostHookForInstance(std::move(Cb), CustomData, Target);
    return -1;
}

inline std::pair<int, int> HookBoth(const wchar_t* FuncPath, UnrealScriptFunctionCallable Pre, UnrealScriptFunctionCallable Post, void* Data = nullptr) {
    return UObjectGlobals::RegisterHook(FuncPath, std::move(Pre), std::move(Post), Data);
}

inline bool Unhook(const wchar_t* FuncPath, CallbackId Id) {
    if (auto* Fn = FindFunc(FuncPath)) return Fn->UnregisterHook(Id);
    return false;
}

inline void UnhookAll(const wchar_t* FuncPath) {
    if (auto* Fn = FindFunc(FuncPath)) Fn->UnregisterAllHooks();
}

inline CallbackId HookPostVoid(const wchar_t* FuncPath, std::function<void()> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext&, void*) { cb(); });
}

inline CallbackId HookPostBool(const wchar_t* FuncPath, std::function<void(bool)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<bool*>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

inline CallbackId HookPostInt(const wchar_t* FuncPath, std::function<void(int32)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<int32*>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

inline CallbackId HookPostFloat(const wchar_t* FuncPath, std::function<void(float)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<float*>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

inline CallbackId HookPostObject(const wchar_t* FuncPath, std::function<void(UObject*)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<UObject**>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

inline CallbackId HookPostString(const wchar_t* FuncPath, std::function<void(StringType)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<StringType*>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

template<typename T>
inline CallbackId HookPostResult(const wchar_t* FuncPath, std::function<void(T&)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        if (auto* Result = static_cast<T*>(Ctx.RESULT_DECL)) cb(*Result);
    });
}

template<typename T>
inline CallbackId HookPreParams(const wchar_t* FuncPath, std::function<void(T&)> Cb) {
    return HookPre(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        cb(Ctx.GetParams<T>());
    });
}

template<typename T>
inline CallbackId HookPostParams(const wchar_t* FuncPath, std::function<void(T&, void*)> Cb) {
    return HookPost(FuncPath, [cb = std::move(Cb)](UnrealScriptFunctionCallableContext& Ctx, void*) {
        cb(Ctx.GetParams<T>(), Ctx.RESULT_DECL);
    });
}
} // namespace HookHelper
