#include "pch.h"
#include "ScanEngineFactory.h"
#include "Engine/Rpc/NamedPipeRpcClient.h"
#include "Engine/Rpc/RPCScanEngine.h"
#include "IScanEngine.h"
#include "LegacyScanEngine.h"

namespace
{
void AppendRpcClientMetricsLog(const std::wstring_view line)
{
    const DWORD length = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", nullptr, 0);
    if (length == 0)
        return;

    std::wstring path(length, L'\0');
    const DWORD actualLength = GetEnvironmentVariableW(L"WINDIRSTAT_RPC_CLIENT_LOG_FILE", path.data(), length);
    if (actualLength == 0 || actualLength >= length)
        return;

    path.resize(actualLength);
    std::ofstream stream(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open())
        return;

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return;

    std::string utf8(utf8Length, '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), utf8Length, nullptr, nullptr);
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream.write("\n", 1);
}

bool UseRpcScanEngineFromCommandLine()
{
    for (int index = 1; index < __argc; ++index)
    {
        const std::wstring arg = __wargv[index];
        if (arg == L"--scan-engine" && index + 1 < __argc)
        {
            std::wstring value = __wargv[++index];
            _wcslwr_s(value.data(), value.size() + 1);
            return value == L"rpc";
        }
    }

    return false;
}

bool UseRpcScanEngine()
{
    std::array<wchar_t, 16> buffer{};
    const DWORD length = GetEnvironmentVariable(L"WINDIRSTAT_SCAN_ENGINE", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size())
    {
        std::wstring value(buffer.data(), length);
        _wcslwr_s(value.data(), value.size() + 1);
        if (value == L"rpc")
            return true;
    }

    return UseRpcScanEngineFromCommandLine();
}
}

std::unique_ptr<IScanEngine> ScanEngineFactory::Create()
{
    if (UseRpcScanEngine())
    {
        VTRACE(L"[RPC] ScanEngineFactory selected RPCScanEngine.");
        AppendRpcClientMetricsLog(L"[RPC] ScanEngineFactory selected RPCScanEngine");
        return std::make_unique<RPCScanEngine>(std::make_unique<NamedPipeRpcClient>());
    }

    VTRACE(L"[RPC] ScanEngineFactory selected LegacyScanEngine.");
    AppendRpcClientMetricsLog(L"[RPC] ScanEngineFactory selected LegacyScanEngine");
    return std::make_unique<LegacyScanEngine>();
}
