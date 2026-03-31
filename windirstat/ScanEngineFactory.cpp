#include "pch.h"
#include "ScanEngineFactory.h"
#include "Engine/Rpc/NamedPipeRpcClient.h"
#include "Engine/Rpc/RPCScanEngine.h"
#include "IScanEngine.h"
#include "LegacyScanEngine.h"

namespace
{
bool UseRpcScanEngine()
{
    std::array<wchar_t, 16> buffer{};
    const DWORD length = GetEnvironmentVariable(L"WINDIRSTAT_SCAN_ENGINE", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return false;

    std::wstring value(buffer.data(), length);
    _wcslwr_s(value.data(), value.size() + 1);
    return value == L"rpc";
}
}

std::unique_ptr<IScanEngine> ScanEngineFactory::Create()
{
    if (UseRpcScanEngine())
        return std::make_unique<RPCScanEngine>(std::make_unique<NamedPipeRpcClient>());

    return std::make_unique<LegacyScanEngine>();
}
