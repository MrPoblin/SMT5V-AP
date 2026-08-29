#pragma once

#define CP_UTF8 65001

#include <Unreal/NameTypes.hpp>
#include <windows.h>
#include <string>

using namespace RC;
using namespace RC::Unreal;

namespace StringHelper {

    inline static StringType ToWide(const char* s) {
        StringType out;
        if (s) for (; *s; ++s) out.push_back(static_cast<wchar_t>(*s));
        return out;
    }

	inline static std::wstring StringToWide(const std::string& s) {
		if (s.empty()) return {};
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring w(len, 0);
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
		return w;
	}
}