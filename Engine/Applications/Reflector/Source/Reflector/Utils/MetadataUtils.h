#pragma once
#include <string>
#include <vector>

struct FMetadataPair
{
    std::string Key;
    std::string Value;
};

class FMetadataParser
{
public:

    FMetadataParser(const std::string& Raw)
    {
        Parse(Raw);
    }

    void Parse(const std::string& Raw);

    auto begin() { return Metadata.begin(); }
    auto end() { return Metadata.end(); }

    auto begin() const { return Metadata.begin(); }
    auto end() const { return Metadata.end(); }
    
    std::vector<FMetadataPair> Metadata;
    
};
