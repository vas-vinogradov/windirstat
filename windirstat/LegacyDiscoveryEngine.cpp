#include "pch.h"
#include "LegacyDiscoveryEngine.h"

#include "FinderBasic.h"
#include "FinderNtfs.h"

namespace
{
std::wstring DiscoveryTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void DiscoveryLog(
    const std::wstring& path,
    std::wstring_view selectedFinder,
    const bool ntfsContextPresent,
    const bool ntfsLoaded,
    const bool basicModeForced)
{
    const std::wstring line = std::format(
        L"[DISCOVERY] ts={} path=\"{}\" selectedFinder={} ntfsContextPresent={} ntfsLoaded={} basicModeForced={}\n",
        DiscoveryTimestamp(),
        path,
        selectedFinder,
        ntfsContextPresent ? 1 : 0,
        ntfsLoaded ? 1 : 0,
        basicModeForced ? 1 : 0);
    OutputDebugStringW(line.c_str());
}
}

DiscoveryBatch LegacyDiscoveryEngine::Discover(const DirectoryDiscoveryRequest& request) {
    DiscoveryBatch batch;
    batch.scannedPath = request.path;

    // Legacy owns its Finder contexts now; keep the old diagnostic shape
    // without pushing those contexts through the shared discovery request.
    const bool ntfsContextPresent = true;
    const bool ntfsLoaded = m_contextNtfs.IsLoaded();
    const bool basicModeForced = false;
    Finder* finder = nullptr;
    if (ntfsLoaded && !basicModeForced) {
        ASSERT(m_contextNtfs.IsLoaded());
        DiscoveryLog(request.path, L"Ntfs", ntfsContextPresent, ntfsLoaded, basicModeForced);
        finder = new FinderNtfs(&m_contextNtfs);
    }
    else {
        DiscoveryLog(request.path, L"Basic", ntfsContextPresent, ntfsLoaded, basicModeForced);
        finder = new FinderBasic(&m_contextBasic);
    }
    if (!finder->FindFile(request.path, 0, 0)) {
        delete finder;
        return batch;
    }
    do {
        if (finder->IsDirectory()) {
            DiscoveredDirectory dir;
            dir.name = finder->GetFileName();
            dir.fullPath = finder->GetFilePath();
            dir.index = finder->GetIndex();
            dir.lastChange = finder->GetLastWriteTime();
            dir.attributes = finder->GetAttributes();
            dir.reparseTag = finder->GetReparseTag();
            dir.isReserved = finder->IsReserved();
            dir.isOffVolume = finder->IsOffVolumeReparsePoint();
            batch.directories.push_back(dir);
        }
        else {
            DiscoveredFile file;
            file.name = finder->GetFileName();
            file.fullPath = finder->GetFilePath();
            file.sizePhysical = finder->GetFileSizePhysical();
            file.sizeLogical = finder->GetFileSizeLogical();
            file.lastChange = finder->GetLastWriteTime();
            file.attributes = finder->GetAttributes();
            file.reparseTag = finder->GetReparseTag();
            file.isReserved = finder->IsReserved();
            batch.files.push_back(file);
        }
    } while (finder->FindNext());
    delete finder;
    return batch;
}
