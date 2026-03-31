#pragma once

#include "ScanTerminalReason.h"
#include "DiscoveredDirectory.h"
#include "DiscoveredFile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <variant>

struct RpcStartScanRequest
{
    std::uint64_t requestId = 0;
    std::wstring rootPath;
};

struct RpcEnqueueRequest
{
    std::uint64_t requestId = 0;
    std::wstring rootPath;
};

struct RpcCloseRequestInput
{
    std::uint64_t requestId = 0;
};

struct RpcCancelScanRequest
{
    std::uint64_t requestId = 0;
    ScanTerminalReason reason = ScanTerminalReason::EngineInterrupted;
};

struct RpcDirectoryProgressEvent
{
    std::uint64_t requestId = 0;
    std::wstring directoryPath;
    std::vector<DiscoveredFile> files;
    std::vector<DiscoveredDirectory> directories;
    bool finished = false;
};

struct RpcScanCompletedEvent
{
    std::uint64_t requestId = 0;
};

struct RpcScanCanceledEvent
{
    std::uint64_t requestId = 0;
    ScanTerminalReason reason = ScanTerminalReason::EngineInterrupted;
};

struct RpcScanFailedEvent
{
    std::uint64_t requestId = 0;
    std::wstring path;
    std::wstring message;
    unsigned long errorCode = 0;
};

using RpcRequestMessage = std::variant<RpcStartScanRequest, RpcEnqueueRequest, RpcCloseRequestInput, RpcCancelScanRequest>;
using RpcEventMessage = std::variant<RpcDirectoryProgressEvent, RpcScanCompletedEvent, RpcScanCanceledEvent, RpcScanFailedEvent>;

std::string SerializeRpcRequestMessage(const RpcRequestMessage& message);
std::string SerializeRpcEventMessage(const RpcEventMessage& message);
std::optional<RpcRequestMessage> TryDeserializeRpcRequestMessage(const std::string& json);
std::optional<RpcEventMessage> TryDeserializeRpcEventMessage(const std::string& json);
