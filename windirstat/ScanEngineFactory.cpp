#include "pch.h"
#include "ScanEngineFactory.h"
#include "Engine/Rpc/NamedPipeRpcClient.h"
#include "Engine/Rpc/RPCScanEngine.h"
#include "IScanEngine.h"
#include "LegacyScanEngine.h"

namespace
{
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
        return std::make_unique<RPCScanEngine>(std::make_unique<NamedPipeRpcClient>());
    }

    VTRACE(L"[RPC] ScanEngineFactory selected LegacyScanEngine.");
    return std::make_unique<LegacyScanEngine>();
}
