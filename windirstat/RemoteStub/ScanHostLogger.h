#pragma once

#include <string>
#include <string_view>

namespace ScanHostLogger
{
enum class Level
{
    Milestone,
    Verbose
};

bool Initialize(const std::wstring& logFilePath, Level level = Level::Milestone);
void Shutdown();
void Log(std::wstring_view message, Level level = Level::Milestone);
std::wstring GetLogFilePath();
Level GetLevel();
}
