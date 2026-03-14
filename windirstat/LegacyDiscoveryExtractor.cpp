#include "pch.h"
#include "LegacyDiscoveryExtractor.h"

#include "FinderBasic.h"
#include "FinderNtfs.h"

DiscoveryBatch LegacyDiscoveryExtractor::Extract(const LegacyDiscoveryRequest& request) {
    DiscoveryBatch batch;
    batch.scannedPath = request.path;
    Finder* finder = nullptr;
    if (request.ntfsContext->IsLoaded() && !request.forceBasic) {
        finder = new FinderNtfs(request.ntfsContext);
    }
    else {
        finder = new FinderBasic(request.basicContext);
    }
    if (!finder->FindFile(request.path,request.index,request.attributes)) {
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
