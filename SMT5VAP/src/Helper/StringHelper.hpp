#pragma once

#include <Unreal/NameTypes.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace StringHelper {

    inline static StringType ToWide(const char* s) {
        StringType out;
        if (s) for (; *s; ++s) out.push_back(static_cast<wchar_t>(*s));
        return out;
    }
}