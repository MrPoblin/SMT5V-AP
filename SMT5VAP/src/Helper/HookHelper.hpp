#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <UnrealCustom/CustomProperty.hpp>
#include <Windows.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <memory>

using namespace RC;
using namespace RC::Unreal;

// ── SignatureScanner: resolves runtime addresses from IDA-style byte signatures ──
class SignatureScanner {
public:
    // Scans the module's .text section for a byte pattern and returns its address.
    // signature: IDA-style hex bytes separated by spaces, '?' for wildcard (e.g. "48 8B ?? 48 85 C0")
    // Returns 0 if not found.
    static uintptr_t FindPattern(const char* signature) {
        auto pattern = Parse(signature);
        if (pattern.empty()) return 0;

        uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!moduleBase) return 0;

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);

        // Scan the full image range (all sections are readable in-process)
        size_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (imageSize == 0 || imageSize > 0x20000000) return 0;

        size_t patternLen = pattern.size();
        if (patternLen > imageSize) return 0;

        for (size_t i = 0; i < imageSize - patternLen; ++i) {
            bool found = true;
            for (size_t j = 0; j < patternLen; ++j) {
                if (pattern[j].required && *reinterpret_cast<const uint8_t*>(moduleBase + i + j) != pattern[j].value) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return moduleBase + i;
            }
        }

        return 0;
    }

private:
    struct PatternByte {
        uint8_t value;
        bool required; // false = wildcard
    };

    static std::vector<PatternByte> Parse(const char* sig) {
        std::vector<PatternByte> result;
        std::istringstream ss(sig);
        std::string byte;
        while (ss >> byte) {
            if (byte == "?" || byte == "??") {
                result.push_back({0, false});
            } else {
                result.push_back({static_cast<uint8_t>(std::stoul(byte, nullptr, 16)), true});
            }
        }
        return result;
    }
};

// ── PropertyField: type-safe access to a struct field at a known offset,
//     using direct offset arithmetic (simpler than CustomProperty which is
//     not exported from UE4SS.dll)                                   ──
template<typename T>
class PropertyField {
public:
    explicit PropertyField(int32_t offset) : m_Offset(offset) {}

    T& Get(void* container) const {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(container) + m_Offset);
    }

    const T& Get(const void* container) const {
        return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(container) + m_Offset);
    }

    void Set(void* container, T value) const {
        Get(container) = value;
    }

    T operator()(void* container) const { return Get(container); }

private:
    int32_t m_Offset;
};

// ── PropertyArrayAccessor: read TArray fields at known offsets ──
// Layout: +0 = Data*, +8 = int32 Count, +12 = int32 Capacity
template<typename T>
class PropertyArrayAccessor {
public:
    explicit PropertyArrayAccessor(int32_t baseOffset)
        : m_DataOff(baseOffset)
        , m_CountOff(baseOffset + 8)
        , m_CapOff(baseOffset + 12)
    {}

    T* GetData(void* container) const {
        return static_cast<T*>(*reinterpret_cast<void**>(static_cast<uint8_t*>(container) + m_DataOff));
    }

    int32_t GetCount(void* container) const {
        return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(container) + m_CountOff);
    }

    int32_t GetCapacity(void* container) const {
        return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(container) + m_CapOff);
    }

    void SetCount(void* container, int32_t count) const {
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(container) + m_CountOff) = count;
    }

private:
    int32_t m_DataOff;
    int32_t m_CountOff;
    int32_t m_CapOff;
};

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
