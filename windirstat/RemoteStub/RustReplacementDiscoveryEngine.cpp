#include "pch.h"
#include "RemoteStub/RustReplacementDiscoveryEngine.h"

#include "RemoteStub/RustScanEngineBridge.h"
#include "RemoteStub/ScanHostLogger.h"

#include <stdexcept>

namespace
{
void RustDiscoveryLog(const std::wstring_view event,
    const std::wstring_view details = {},
    const ScanHostLogger::Level level = ScanHostLogger::Level::Milestone)
{
    std::wstring line = std::format(L"[RPC][HOST][RUST] event={}", event);
    if (!details.empty())
        line += std::format(L" {}", details);

    ScanHostLogger::Log(line, level);
}

std::wstring CopyWideString(const std::uint16_t* value, const std::size_t length)
{
    if (value == nullptr || length == 0)
        return {};

    return std::wstring(reinterpret_cast<const wchar_t*>(value), length);
}

FILETIME ToFileTime(const std::uint64_t value)
{
    FILETIME fileTime{};
    fileTime.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFull);
    fileTime.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return fileTime;
}

struct RustDiscoveryCallbackContext
{
    DiscoveryBatch* batch = nullptr;
    std::size_t callbackCount = 0;
};

void CollectRustLogMessage(const std::uint16_t* const message, const std::size_t messageLen, void* const)
{
    const std::wstring text = CopyWideString(message, messageLen);
    if (text.empty())
        return;

    RustDiscoveryLog(L"RustDetail", text, ScanHostLogger::Level::Verbose);
}

void CollectRustDiscoveryEntry(const RustDiscoveryEntryView* const entry, void* const userData)
{
    ASSERT(entry != nullptr);
    ASSERT(userData != nullptr);
    auto& context = *static_cast<RustDiscoveryCallbackContext*>(userData);
    ASSERT(context.batch != nullptr);
    auto& batch = *context.batch;
    ++context.callbackCount;

    const std::wstring name = CopyWideString(entry->name, entry->nameLen);
    const std::wstring fullPath = CopyWideString(entry->fullPath, entry->fullPathLen);

    if (entry->kind == RustDiscoveryEntryKind::Directory)
    {
        DiscoveredDirectory directory{};
        directory.name = name;
        directory.fullPath = fullPath;
        directory.index = entry->index;
        directory.lastChange = ToFileTime(entry->lastWriteTime);
        directory.attributes = entry->attributes;
        directory.reparseTag = entry->reparseTag;
        directory.isReserved = entry->isReserved != 0;
        directory.isOffVolume = entry->isOffVolume != 0;
        directory.isProtectedReparsePoint = entry->isProtectedReparsePoint != 0;
        batch.directories.push_back(std::move(directory));
        return;
    }

    DiscoveredFile file{};
    file.name = name;
    file.fullPath = fullPath;
    file.sizePhysical = entry->sizePhysical;
    file.sizeLogical = entry->sizeLogical;
    file.index = entry->index;
    file.lastChange = ToFileTime(entry->lastWriteTime);
    file.attributes = entry->attributes;
    file.reparseTag = entry->reparseTag;
    file.isReserved = entry->isReserved != 0;
    batch.files.push_back(std::move(file));
}
}

DiscoveryBatch RustReplacementDiscoveryEngine::Discover(const DirectoryDiscoveryRequest& request)
{
    DiscoveryBatch batch{};
    batch.scannedPath = request.path;
    // Contract: Rust discovery consumes only request.path. Legacy traversal
    // hints and Finder contexts stay legacy-only and are intentionally ignored.
    RustDiscoveryLog(L"DiscoverEnter", std::format(L"path=\"{}\"", request.path));

#if !defined(_M_X64)
    RustDiscoveryLog(L"DiscoverUnsupportedBuild", std::format(L"path=\"{}\"", request.path));
    throw std::runtime_error("Rust discovery engine is only available in x64 host builds.");
#else
    RustDiscoveryCallbackContext context{};
    context.batch = &batch;

    RustDiscoveryErrorView error{};
    const bool success = windirstat_rust_discover_directory(
        reinterpret_cast<const std::uint16_t*>(request.path.data()),
        request.path.size(),
        &CollectRustDiscoveryEntry,
        &context,
        &CollectRustLogMessage,
        nullptr,
        &error);
    if (success)
    {
        RustDiscoveryLog(L"DiscoverReturned",
            std::format(L"path=\"{}\" callbacks={} files={} directories={}",
                request.path, context.callbackCount, batch.files.size(), batch.directories.size()));
        return batch;
    }

    const std::string errorMessage = error.message != nullptr
        ? std::string(error.message, error.messageLen)
        : std::string("Rust discovery returned failure.");
    RustDiscoveryLog(L"DiscoverFailed",
        std::format(L"path=\"{}\" message=\"{}\"", request.path, std::wstring(CA2W(errorMessage.c_str()))));
    throw std::runtime_error(errorMessage);
#endif
}
