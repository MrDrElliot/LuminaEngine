#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
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

    // Rewriting a generated file recompiles every TU that includes it, so an unchanged write is skipped.
    inline void WriteFileIfChanged(const std::string& PathUtf8, const std::string& Contents)
    {
        const std::filesystem::path OutputPath(PathUtf8.c_str());

        std::error_code Ec;
        if (std::filesystem::file_size(OutputPath, Ec) == Contents.size() && !Ec)
        {
            std::string Current;
            if (ReadWholeFile(PathUtf8, Current) && Current == Contents)
            {
                return;
            }
        }

        std::filesystem::create_directories(OutputPath.parent_path(), Ec);

        // Binary mode, since text mode turns every newline into a pair and defeats the unchanged-file skip.
        std::ofstream Stream(OutputPath, std::ios::binary);
        Stream.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
    }
}
