#include "pch.h"
#include "RemoteStub/RemoteStubCommandLine.h"

#include "RemoteStub/RemoteScanHost.h"

bool TryRunRemoteStubFromCommandLine()
{
    std::wstring pipeName;
    bool runRemoteStub = false;

    for (int i = 1; i < __argc; ++i)
    {
        const std::wstring arg = __wargv[i];
        if (arg == L"--remote-stub")
        {
            runRemoteStub = true;
            continue;
        }

        if (arg == L"--pipe" && i + 1 < __argc)
            pipeName = __wargv[++i];
    }

    if (!runRemoteStub)
        return false;

    RemoteScanHost host;
    ExitProcess(static_cast<UINT>(host.Run(pipeName)));
    return true;
}
