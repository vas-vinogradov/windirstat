#include "pch.h"
#include "Engine/Rpc/RpcMessages.h"

namespace
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string utf8(static_cast<size_t>(size), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(size), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

std::string EscapeJsonString(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(utf8.size() + 8);

    for (const unsigned char ch : utf8)
    {
        switch (ch)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(static_cast<char>(ch)); break;
        }
    }

    return escaped;
}

std::optional<std::string> ExtractJsonStringField(const std::string& json, const std::string& fieldName)
{
    const std::string token = "\"" + fieldName + "\":\"";
    size_t pos = json.find(token);
    if (pos == std::string::npos)
        return std::nullopt;

    pos += token.size();
    std::string value;
    bool escape = false;
    for (size_t i = pos; i < json.size(); ++i)
    {
        const char ch = json[i];
        if (escape)
        {
            switch (ch)
            {
            case '\\':
            case '"':
            case '/':
                value.push_back(ch);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
            escape = false;
            continue;
        }

        if (ch == '\\')
        {
            escape = true;
            continue;
        }

        if (ch == '"')
            return value;

        value.push_back(ch);
    }

    return std::nullopt;
}

template<typename TValue>
std::optional<TValue> ExtractJsonIntegralField(const std::string& json, const std::string& fieldName)
{
    const std::string token = "\"" + fieldName + "\":";
    size_t pos = json.find(token);
    if (pos == std::string::npos)
        return std::nullopt;

    pos += token.size();
    size_t end = pos;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])) != 0)
        ++end;

    if (end == pos)
        return std::nullopt;

    try
    {
        if constexpr (std::is_same_v<TValue, std::uint64_t>)
            return static_cast<TValue>(std::stoull(json.substr(pos, end - pos)));
        else
            return static_cast<TValue>(std::stoul(json.substr(pos, end - pos)));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<bool> ExtractJsonBoolField(const std::string& json, const std::string& fieldName)
{
    const std::string token = "\"" + fieldName + "\":";
    size_t pos = json.find(token);
    if (pos == std::string::npos)
        return std::nullopt;

    pos += token.size();
    if (json.compare(pos, 4, "true") == 0)
        return true;
    if (json.compare(pos, 5, "false") == 0)
        return false;
    return std::nullopt;
}

std::string SerializeJsonBool(const bool value)
{
    return value ? "true" : "false";
}

std::uint64_t FileTimeToUint64(const FILETIME& fileTime)
{
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

FILETIME Uint64ToFileTime(const std::uint64_t value)
{
    ULARGE_INTEGER largeValue{};
    largeValue.QuadPart = value;

    FILETIME fileTime{};
    fileTime.dwLowDateTime = largeValue.LowPart;
    fileTime.dwHighDateTime = largeValue.HighPart;
    return fileTime;
}

std::string SerializeDiscoveredFile(const DiscoveredFile& file)
{
    return std::string("{\"name\":\"") + EscapeJsonString(file.name) +
        "\",\"fullPath\":\"" + EscapeJsonString(file.fullPath) +
        "\",\"sizePhysical\":" + std::to_string(file.sizePhysical) +
        ",\"sizeLogical\":" + std::to_string(file.sizeLogical) +
        ",\"index\":" + std::to_string(file.index) +
        ",\"lastChange\":" + std::to_string(FileTimeToUint64(file.lastChange)) +
        ",\"attributes\":" + std::to_string(file.attributes) +
        ",\"reparseTag\":" + std::to_string(file.reparseTag) +
        ",\"isReserved\":" + SerializeJsonBool(file.isReserved) + "}";
}

std::string SerializeDiscoveredDirectory(const DiscoveredDirectory& directory)
{
    return std::string("{\"name\":\"") + EscapeJsonString(directory.name) +
        "\",\"fullPath\":\"" + EscapeJsonString(directory.fullPath) +
        "\",\"index\":" + std::to_string(directory.index) +
        ",\"lastChange\":" + std::to_string(FileTimeToUint64(directory.lastChange)) +
        ",\"attributes\":" + std::to_string(directory.attributes) +
        ",\"reparseTag\":" + std::to_string(directory.reparseTag) +
        ",\"isReserved\":" + SerializeJsonBool(directory.isReserved) +
        ",\"isOffVolume\":" + SerializeJsonBool(directory.isOffVolume) +
        ",\"isProtectedReparsePoint\":" + SerializeJsonBool(directory.isProtectedReparsePoint) + "}";
}

template<typename TValue>
std::string SerializeJsonArray(const std::vector<TValue>& values, const std::function<std::string(const TValue&)>& serializer)
{
    std::string json = "[";
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            json += ",";
        json += serializer(values[index]);
    }
    json += "]";
    return json;
}

std::string SerializeDirectoryProgressPayload(const RpcDirectoryProgressEvent& progress, const bool includeKind)
{
    std::string json = "{";
    if (includeKind)
        json += "\"kind\":\"DirectoryProgressEvent\",";

    json += "\"requestId\":" + std::to_string(progress.requestId) +
        ",\"directoryPath\":\"" + EscapeJsonString(progress.directoryPath) +
        "\",\"files\":" + SerializeJsonArray<DiscoveredFile>(progress.files, SerializeDiscoveredFile) +
        ",\"directories\":" + SerializeJsonArray<DiscoveredDirectory>(progress.directories, SerializeDiscoveredDirectory) +
        ",\"finished\":" + std::string(progress.finished ? "true" : "false") + "}";
    return json;
}

std::optional<std::string> ExtractJsonArrayField(const std::string& json, const std::string& fieldName)
{
    const std::string token = "\"" + fieldName + "\":[";
    size_t pos = json.find(token);
    if (pos == std::string::npos)
        return std::nullopt;

    pos += token.size() - 1;
    size_t index = pos;
    int depth = 0;
    bool inString = false;
    bool escape = false;

    for (; index < json.size(); ++index)
    {
        const char ch = json[index];
        if (escape)
        {
            escape = false;
            continue;
        }

        if (ch == '\\' && inString)
        {
            escape = true;
            continue;
        }

        if (ch == '"')
        {
            inString = !inString;
            continue;
        }

        if (inString)
            continue;

        if (ch == '[')
            ++depth;
        else if (ch == ']')
        {
            --depth;
            if (depth == 0)
                return json.substr(pos + 1, index - pos - 1);
        }
    }

    return std::nullopt;
}

std::vector<std::string> SplitJsonObjectArray(const std::string& json)
{
    std::vector<std::string> objects;
    size_t objectStart = std::string::npos;
    int depth = 0;
    bool inString = false;
    bool escape = false;

    for (size_t index = 0; index < json.size(); ++index)
    {
        const char ch = json[index];
        if (escape)
        {
            escape = false;
            continue;
        }

        if (ch == '\\' && inString)
        {
            escape = true;
            continue;
        }

        if (ch == '"')
        {
            inString = !inString;
            continue;
        }

        if (inString)
            continue;

        if (ch == '{')
        {
            if (depth == 0)
                objectStart = index;
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                objects.push_back(json.substr(objectStart, index - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}

std::optional<DiscoveredFile> TryParseDiscoveredFile(const std::string& json)
{
    const auto nameOpt = ExtractJsonStringField(json, "name");
    const auto fullPathOpt = ExtractJsonStringField(json, "fullPath");
    const auto sizePhysicalOpt = ExtractJsonIntegralField<std::uint64_t>(json, "sizePhysical");
    const auto sizeLogicalOpt = ExtractJsonIntegralField<std::uint64_t>(json, "sizeLogical");
    const auto indexOpt = ExtractJsonIntegralField<std::uint64_t>(json, "index");
    const auto lastChangeOpt = ExtractJsonIntegralField<std::uint64_t>(json, "lastChange");
    const auto attributesOpt = ExtractJsonIntegralField<unsigned long>(json, "attributes");
    const auto reparseTagOpt = ExtractJsonIntegralField<unsigned long>(json, "reparseTag");
    const auto isReservedOpt = ExtractJsonBoolField(json, "isReserved");
    if (!nameOpt.has_value() || !fullPathOpt.has_value() || !sizePhysicalOpt.has_value() ||
        !sizeLogicalOpt.has_value() || !indexOpt.has_value() || !lastChangeOpt.has_value() ||
        !attributesOpt.has_value() || !reparseTagOpt.has_value() || !isReservedOpt.has_value())
    {
        return std::nullopt;
    }

    DiscoveredFile file{};
    file.name = Utf8ToWide(nameOpt.value());
    file.fullPath = Utf8ToWide(fullPathOpt.value());
    file.sizePhysical = sizePhysicalOpt.value();
    file.sizeLogical = sizeLogicalOpt.value();
    file.index = indexOpt.value();
    file.lastChange = Uint64ToFileTime(lastChangeOpt.value());
    file.attributes = attributesOpt.value();
    file.reparseTag = reparseTagOpt.value();
    file.isReserved = isReservedOpt.value();
    return file;
}

std::optional<DiscoveredDirectory> TryParseDiscoveredDirectory(const std::string& json)
{
    const auto nameOpt = ExtractJsonStringField(json, "name");
    const auto fullPathOpt = ExtractJsonStringField(json, "fullPath");
    const auto indexOpt = ExtractJsonIntegralField<std::uint64_t>(json, "index");
    const auto lastChangeOpt = ExtractJsonIntegralField<std::uint64_t>(json, "lastChange");
    const auto attributesOpt = ExtractJsonIntegralField<unsigned long>(json, "attributes");
    const auto reparseTagOpt = ExtractJsonIntegralField<unsigned long>(json, "reparseTag");
    const auto isReservedOpt = ExtractJsonBoolField(json, "isReserved");
    const auto isOffVolumeOpt = ExtractJsonBoolField(json, "isOffVolume");
    const auto isProtectedOpt = ExtractJsonBoolField(json, "isProtectedReparsePoint");
    if (!nameOpt.has_value() || !fullPathOpt.has_value() || !indexOpt.has_value() ||
        !lastChangeOpt.has_value() || !attributesOpt.has_value() || !reparseTagOpt.has_value() ||
        !isReservedOpt.has_value() || !isOffVolumeOpt.has_value() || !isProtectedOpt.has_value())
    {
        return std::nullopt;
    }

    DiscoveredDirectory directory{};
    directory.name = Utf8ToWide(nameOpt.value());
    directory.fullPath = Utf8ToWide(fullPathOpt.value());
    directory.index = indexOpt.value();
    directory.lastChange = Uint64ToFileTime(lastChangeOpt.value());
    directory.attributes = attributesOpt.value();
    directory.reparseTag = reparseTagOpt.value();
    directory.isReserved = isReservedOpt.value();
    directory.isOffVolume = isOffVolumeOpt.value();
    directory.isProtectedReparsePoint = isProtectedOpt.value();
    return directory;
}

std::optional<RpcDirectoryProgressEvent> TryParseDirectoryProgressPayload(const std::string& json, const std::optional<std::uint64_t> fallbackRequestId = std::nullopt)
{
    const auto requestIdOpt = ExtractJsonIntegralField<std::uint64_t>(json, "requestId");
    const auto directoryPathOpt = ExtractJsonStringField(json, "directoryPath");
    const auto filesOpt = ExtractJsonArrayField(json, "files");
    const auto directoriesOpt = ExtractJsonArrayField(json, "directories");
    const auto finishedOpt = ExtractJsonBoolField(json, "finished");
    if ((!requestIdOpt.has_value() && !fallbackRequestId.has_value()) ||
        !directoryPathOpt.has_value() || !filesOpt.has_value() ||
        !directoriesOpt.has_value() || !finishedOpt.has_value())
    {
        return std::nullopt;
    }

    RpcDirectoryProgressEvent event{};
    event.requestId = requestIdOpt.value_or(fallbackRequestId.value_or(0));
    event.directoryPath = Utf8ToWide(directoryPathOpt.value());
    for (const auto& fileJson : SplitJsonObjectArray(filesOpt.value()))
    {
        const auto fileOpt = TryParseDiscoveredFile(fileJson);
        if (!fileOpt.has_value())
            return std::nullopt;
        event.files.push_back(std::move(fileOpt.value()));
    }

    for (const auto& directoryJson : SplitJsonObjectArray(directoriesOpt.value()))
    {
        const auto directoryOpt = TryParseDiscoveredDirectory(directoryJson);
        if (!directoryOpt.has_value())
            return std::nullopt;
        event.directories.push_back(std::move(directoryOpt.value()));
    }

    event.finished = finishedOpt.value();
    return event;
}
}

std::string SerializeRpcRequestMessage(const RpcRequestMessage& message)
{
    if (const auto* startScan = std::get_if<RpcStartScanRequest>(&message))
    {
        return std::string("{\"kind\":\"StartScanRequest\",\"requestId\":") +
            std::to_string(startScan->requestId) +
            ",\"rootPath\":\"" + EscapeJsonString(startScan->rootPath) +
            "\",\"followMountPoints\":" + SerializeJsonBool(startScan->followMountPoints) +
            ",\"followSymbolicLinks\":" + SerializeJsonBool(startScan->followSymbolicLinks) +
            ",\"followJunctions\":" + SerializeJsonBool(startScan->followJunctions) + "}";
    }

    if (const auto* enqueue = std::get_if<RpcEnqueueRequest>(&message))
    {
        return std::string("{\"kind\":\"EnqueueRequest\",\"requestId\":") +
            std::to_string(enqueue->requestId) +
            ",\"rootPath\":\"" + EscapeJsonString(enqueue->rootPath) + "\"}";
    }

    if (const auto* closeRequest = std::get_if<RpcCloseRequestInput>(&message))
    {
        return std::string("{\"kind\":\"CloseRequestInput\",\"requestId\":") +
            std::to_string(closeRequest->requestId) + "}";
    }

    const auto& cancelScan = std::get<RpcCancelScanRequest>(message);
    return std::string("{\"kind\":\"CancelScanRequest\",\"requestId\":") +
        std::to_string(cancelScan.requestId) +
        ",\"reason\":" + std::to_string(static_cast<unsigned int>(cancelScan.reason)) + "}";
}

std::string SerializeRpcEventMessage(const RpcEventMessage& message)
{
    if (const auto* progress = std::get_if<RpcDirectoryProgressEvent>(&message))
    {
        return SerializeDirectoryProgressPayload(*progress, true);
    }

    if (const auto* batch = std::get_if<RpcDirectoryProgressBatchEvent>(&message))
    {
        return std::string("{\"kind\":\"DirectoryProgressBatchEvent\",\"requestId\":") +
            std::to_string(batch->requestId) +
            ",\"items\":" + SerializeJsonArray<RpcDirectoryProgressEvent>(
                batch->items,
                [](const RpcDirectoryProgressEvent& item)
                {
                    return SerializeDirectoryProgressPayload(item, false);
                }) + "}";
    }

    if (const auto* completed = std::get_if<RpcScanCompletedEvent>(&message))
    {
        return std::string("{\"kind\":\"ScanCompletedEvent\",\"requestId\":") +
            std::to_string(completed->requestId) + "}";
    }

    if (const auto* canceled = std::get_if<RpcScanCanceledEvent>(&message))
    {
        return std::string("{\"kind\":\"ScanCanceledEvent\",\"requestId\":") +
            std::to_string(canceled->requestId) +
            ",\"reason\":" + std::to_string(static_cast<unsigned int>(canceled->reason)) + "}";
    }

    const auto& failed = std::get<RpcScanFailedEvent>(message);
    return std::string("{\"kind\":\"ScanFailedEvent\",\"requestId\":") +
        std::to_string(failed.requestId) +
        ",\"path\":\"" + EscapeJsonString(failed.path) +
        "\",\"message\":\"" + EscapeJsonString(failed.message) +
        "\",\"errorCode\":" + std::to_string(failed.errorCode) + "}";
}

std::optional<RpcRequestMessage> TryDeserializeRpcRequestMessage(const std::string& json)
{
    const auto kindOpt = ExtractJsonStringField(json, "kind");
    const auto requestIdOpt = ExtractJsonIntegralField<std::uint64_t>(json, "requestId");
    if (!kindOpt.has_value() || !requestIdOpt.has_value())
        return std::nullopt;

    if (kindOpt.value() == "StartScanRequest")
    {
        const auto rootPathOpt = ExtractJsonStringField(json, "rootPath");
        const auto followMountPointsOpt = ExtractJsonBoolField(json, "followMountPoints");
        const auto followSymbolicLinksOpt = ExtractJsonBoolField(json, "followSymbolicLinks");
        const auto followJunctionsOpt = ExtractJsonBoolField(json, "followJunctions");
        if (!rootPathOpt.has_value() || !followMountPointsOpt.has_value() ||
            !followSymbolicLinksOpt.has_value() || !followJunctionsOpt.has_value())
            return std::nullopt;

        RpcStartScanRequest request{};
        request.requestId = requestIdOpt.value();
        request.rootPath = Utf8ToWide(rootPathOpt.value());
        request.followMountPoints = followMountPointsOpt.value();
        request.followSymbolicLinks = followSymbolicLinksOpt.value();
        request.followJunctions = followJunctionsOpt.value();
        return RpcRequestMessage(request);
    }

    if (kindOpt.value() == "EnqueueRequest")
    {
        const auto rootPathOpt = ExtractJsonStringField(json, "rootPath");
        if (!rootPathOpt.has_value())
            return std::nullopt;

        RpcEnqueueRequest request{};
        request.requestId = requestIdOpt.value();
        request.rootPath = Utf8ToWide(rootPathOpt.value());
        return RpcRequestMessage(request);
    }

    if (kindOpt.value() == "CloseRequestInput")
    {
        RpcCloseRequestInput request{};
        request.requestId = requestIdOpt.value();
        return RpcRequestMessage(request);
    }

    if (kindOpt.value() == "CancelScanRequest")
    {
        const auto reasonOpt = ExtractJsonIntegralField<unsigned long>(json, "reason");
        if (!reasonOpt.has_value())
            return std::nullopt;

        RpcCancelScanRequest request{};
        request.requestId = requestIdOpt.value();
        request.reason = static_cast<ScanTerminalReason>(reasonOpt.value());
        return RpcRequestMessage(request);
    }

    return std::nullopt;
}

std::optional<RpcEventMessage> TryDeserializeRpcEventMessage(const std::string& json)
{
    const auto kindOpt = ExtractJsonStringField(json, "kind");
    const auto requestIdOpt = ExtractJsonIntegralField<std::uint64_t>(json, "requestId");
    if (!kindOpt.has_value() || !requestIdOpt.has_value())
        return std::nullopt;

    if (kindOpt.value() == "DirectoryProgressEvent")
    {
        const auto eventOpt = TryParseDirectoryProgressPayload(json, requestIdOpt);
        if (!eventOpt.has_value())
            return std::nullopt;

        return RpcEventMessage(eventOpt.value());
    }

    if (kindOpt.value() == "DirectoryProgressBatchEvent")
    {
        const auto itemsOpt = ExtractJsonArrayField(json, "items");
        if (!itemsOpt.has_value())
            return std::nullopt;

        RpcDirectoryProgressBatchEvent batch{};
        batch.requestId = requestIdOpt.value();
        for (const auto& itemJson : SplitJsonObjectArray(itemsOpt.value()))
        {
            const auto itemOpt = TryParseDirectoryProgressPayload(itemJson, batch.requestId);
            if (!itemOpt.has_value())
                return std::nullopt;
            batch.items.push_back(std::move(itemOpt.value()));
        }

        return RpcEventMessage(std::move(batch));
    }

    if (kindOpt.value() == "ScanCompletedEvent")
    {
        RpcScanCompletedEvent event{};
        event.requestId = requestIdOpt.value();
        return RpcEventMessage(event);
    }

    if (kindOpt.value() == "ScanCanceledEvent")
    {
        const auto reasonOpt = ExtractJsonIntegralField<unsigned long>(json, "reason");
        if (!reasonOpt.has_value())
            return std::nullopt;

        RpcScanCanceledEvent event{};
        event.requestId = requestIdOpt.value();
        event.reason = static_cast<ScanTerminalReason>(reasonOpt.value());
        return RpcEventMessage(event);
    }

    if (kindOpt.value() == "ScanFailedEvent")
    {
        const auto pathOpt = ExtractJsonStringField(json, "path");
        const auto messageOpt = ExtractJsonStringField(json, "message");
        const auto errorCodeOpt = ExtractJsonIntegralField<unsigned long>(json, "errorCode");
        if (!pathOpt.has_value() || !messageOpt.has_value() || !errorCodeOpt.has_value())
            return std::nullopt;

        RpcScanFailedEvent event{};
        event.requestId = requestIdOpt.value();
        event.path = Utf8ToWide(pathOpt.value());
        event.message = Utf8ToWide(messageOpt.value());
        event.errorCode = errorCodeOpt.value();
        return RpcEventMessage(event);
    }

    return std::nullopt;
}
