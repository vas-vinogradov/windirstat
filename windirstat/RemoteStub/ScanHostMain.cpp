#include "pch.h"
#include "Version.h"
#include "RemoteStub/ScanHostLogger.h"
#include "RemoteStub/RemoteScanHost.h"

namespace
{
ScanHostLogger::Level ParseHostLogLevel(const std::wstring_view value)
{
    if (value == L"verbose")
        return ScanHostLogger::Level::Verbose;

    return ScanHostLogger::Level::Milestone;
}

void ScanHostStartupLog(std::wstring_view message, const ScanHostLogger::Level level = ScanHostLogger::Level::Milestone)
{
    ScanHostLogger::Log(std::format(L"[HOST] {}", message), level);
}
}

int wmain(const int argc, wchar_t* argv[])
{
    std::wstring pipeName;
    std::wstring logFilePath;
    ScanHostLogger::Level logLevel = ScanHostLogger::Level::Milestone;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring arg = argv[index];
        if (arg == L"--pipe" && index + 1 < argc)
        {
            pipeName = argv[++index];
        }
        else if (arg == L"--log-file" && index + 1 < argc)
        {
            logFilePath = argv[++index];
        }
        else if (arg == L"--log-level" && index + 1 < argc)
        {
            std::wstring value = argv[++index];
            _wcslwr_s(value.data(), value.size() + 1);
            logLevel = ParseHostLogLevel(value);
        }
    }

    const bool loggerInitialized = ScanHostLogger::Initialize(logFilePath, logLevel);
    (void)loggerInitialized;
    logFilePath = ScanHostLogger::GetLogFilePath();

    std::array<wchar_t, MAX_PATH> executablePath{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    const std::wstring executable(executablePath.data(), length);
    ScanHostStartupLog(std::format(L"ProcessStart executable=\"{}\"", executable));
    ScanHostStartupLog(std::format(L"ParsedArguments pipe=\"{}\" logFile=\"{}\" logLevel={}",
        pipeName, logFilePath, logLevel == ScanHostLogger::Level::Verbose ? L"verbose" : L"milestone"));
    ScanHostStartupLog(std::format(L"BuildInfo gitCount={} gitDate={} gitCommit={}",
        GIT_COUNT, std::wstring(CA2W(GIT_DATE)), std::wstring(CA2W(GIT_COMMIT))));

    ScanHostStartupLog(std::format(L"SelectedLogFile path=\"{}\"", logFilePath));

    if (!pipeName.empty())
        ScanHostStartupLog(std::format(L"SelectedPipe pipe=\"{}\"", pipeName));

    int exitCode = 0;
    try
    {
        if (pipeName.empty())
        {
            ScanHostStartupLog(L"ProcessStartFailed reason=MissingPipeArgument");
            exitCode = 2;
        }
        else
        {
            RemoteScanHost host;
            exitCode = host.Run(pipeName);
        }
    }
    catch (const std::exception& ex)
    {
        ScanHostStartupLog(std::format(L"UnhandledException type=std::exception message=\"{}\"", std::wstring(CA2W(ex.what()))));
        exitCode = 10;
    }
    catch (...)
    {
        ScanHostStartupLog(L"UnhandledException type=unknown");
        exitCode = 11;
    }

    ScanHostStartupLog(std::format(L"ProcessShutdown exitCode={}", exitCode));
    ScanHostLogger::Shutdown();
    return exitCode;
}
