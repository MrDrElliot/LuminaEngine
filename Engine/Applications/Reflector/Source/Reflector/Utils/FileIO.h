#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace Lumina::Reflection
{
    // Reads a whole file into OutContents. False when the file could not be opened.
    inline bool ReadWholeFile(const std::string& AbsPath, std::string& OutContents)
    {
        std::FILE* File = std::fopen(AbsPath.c_str(), "rb");
        if (File == nullptr)
        {
            return false;
        }

        std::error_code Ec;
        const uintmax_t Size = std::filesystem::file_size(std::filesystem::path(AbsPath.c_str()), Ec);
        OutContents.resize(Ec ? 0 : static_cast<size_t>(Size));

        const size_t Read = OutContents.empty() ? 0 : std::fread(OutContents.data(), 1, OutContents.size(), File);
        OutContents.resize(Read);

        std::fclose(File);
        return true;
    }
}
