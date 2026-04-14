#include "pch.h"
#include "RemoteStub/ScanHostLogger.h"

namespace
{
std::mutex g_logMutex;
std::ofstream g_logStream;
std::wstring g_logFilePath;
ScanHostLogger::Level g_logLevel = ScanHostLogger::Level::Milestone;

std::wstring CreateDefaultLogFilePath()
{
    std::array<wchar_t, MAX_PATH> tempPath{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
    const std::filesystem::path directory = (length == 0 || length >= tempPath.size())
        ? std::filesystem::temp_directory_path()
        : std::filesystem::path(std::wstring(tempPath.data(), length));

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::wstring fileName = std::format(L"windirstat-scan-host-{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.log",
        GetCurrentProcessId(),
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return (directory / fileName).wstring();
}

std::wstring BuildLogPrefix()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} pid={} tid={}",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId());
}

std::string ToUtf8(const std::wstring_view text)
{
    if (text.empty())
        return {};

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(length, '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

void FallbackToDebugOutput(const std::wstring_view line)
{
    std::wstring output(line);
    output += L"\n";
    OutputDebugStringW(output.c_str());
}
}

bool ScanHostLogger::Initialize(const std::wstring& logFilePath, const Level level)
{
    std::scoped_lock lock(g_logMutex);
    g_logLevel = level;
    g_logFilePath = logFilePath.empty() ? CreateDefaultLogFilePath() : logFilePath;
    g_logStream.open(g_logFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!g_logStream.is_open())
    {
        FallbackToDebugOutput(std::format(L"{} [HOST] LoggerInitializationFailed path=\"{}\" error={}",
            BuildLogPrefix(), g_logFilePath, GetLastError()));
        return false;
    }

    const std::string line = ToUtf8(std::format(L"{} [HOST] LoggerInitialized path=\"{}\"\n",
        BuildLogPrefix(), g_logFilePath));
    g_logStream.write(line.data(), static_cast<std::streamsize>(line.size()));
    g_logStream.flush();
    return true;
}

void ScanHostLogger::Shutdown()
{
    std::scoped_lock lock(g_logMutex);
    if (!g_logStream.is_open())
        return;

    const std::string line = ToUtf8(std::format(L"{} [HOST] LoggerShutdown path=\"{}\"\n",
        BuildLogPrefix(), g_logFilePath));
    g_logStream.write(line.data(), static_cast<std::streamsize>(line.size()));
    g_logStream.flush();
    g_logStream.close();
    g_logFilePath.clear();
}

void ScanHostLogger::Log(const std::wstring_view message, const Level level)
{
    std::scoped_lock lock(g_logMutex);
    if (level > g_logLevel)
        return;

    const std::wstring line = std::format(L"{} {}", BuildLogPrefix(), message);
    if (!g_logStream.is_open())
    {
        FallbackToDebugOutput(line);
        return;
    }

    const std::string utf8 = ToUtf8(line + L"\n");
    g_logStream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    g_logStream.flush();
}

std::wstring ScanHostLogger::GetLogFilePath()
{
    std::scoped_lock lock(g_logMutex);
    return g_logFilePath;
}

ScanHostLogger::Level ScanHostLogger::GetLevel()
{
    std::scoped_lock lock(g_logMutex);
    return g_logLevel;
}
