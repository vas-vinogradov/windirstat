#pragma once

#include "DirectoryDiscoveryEngine.h"
#include "DiscoveryBatch.h"

#include <memory>
#include <string>
#include <variant>

struct DirectoryWorkItemSuccess
{
    DiscoveryBatch discoveryBatch;
};

struct DirectoryWorkItemFailure
{
    std::wstring message;
    unsigned long errorCode = 0;
};

struct DirectoryWorkItemResult
{
    std::wstring path;
    std::variant<DirectoryWorkItemSuccess, DirectoryWorkItemFailure> outcome;
};

class IDirectoryWorkItemProcessor
{
public:
    virtual ~IDirectoryWorkItemProcessor() = default;
    virtual DirectoryWorkItemResult Process(const std::wstring& path) = 0;
};

class DiscoveryDirectoryWorkItemProcessor final : public IDirectoryWorkItemProcessor
{
public:
    explicit DiscoveryDirectoryWorkItemProcessor(std::unique_ptr<IDirectoryDiscoveryEngine> discoveryEngine);
    DirectoryWorkItemResult Process(const std::wstring& path) override;

private:
    std::unique_ptr<IDirectoryDiscoveryEngine> m_discoveryEngine;
};
