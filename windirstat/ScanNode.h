#pragma once
struct ScanNode
{
    std::wstring name;
    std::wstring path;
    uint64_t size = 0;
    bool isDirectory = false;

    std::vector<ScanNode> children;
};
