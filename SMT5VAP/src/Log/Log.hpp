#pragma once

#include <DynamicOutput/Output.hpp>
#include <String/StringType.hpp>

#define LOG(fmt, ...)   Output::send<LogLevel::Normal>(STR("[SMT5VAP] " fmt L"\n"), ##__VA_ARGS__)
#define DEBUG(fmt, ...) Output::send<LogLevel::Verbose>(STR("[SMT5VAP] " fmt L"\n"), ##__VA_ARGS__)
#define WARN(fmt, ...)  Output::send<LogLevel::Warning>(STR("[SMT5VAP] " fmt L"\n"), ##__VA_ARGS__)
#define ERROR(fmt, ...) Output::send<LogLevel::Error>(STR("[SMT5VAP] " fmt L"\n"), ##__VA_ARGS__)
