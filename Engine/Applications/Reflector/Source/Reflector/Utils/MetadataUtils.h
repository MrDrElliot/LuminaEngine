#pragma once
#include <string>
#include <string_view>
#include <vector>

struct FMetadataPair
{
    std::string Key;
    std::string Value;
};

class FMetadataParser
{
public:

    explicit FMetadataParser(std::string_view Raw)
    {
        Parse(Raw);
    }

    void Parse(std::string_view Raw);

    auto begin() { return Metadata.begin(); }
    auto end() { return Metadata.end(); }

    auto begin() const { return Metadata.begin(); }
    auto end() const { return Metadata.end(); }

    std::vector<FMetadataPair> Metadata;

};
