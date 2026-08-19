#include "ReflectedHeader.h"
#include <filesystem>
#include "ReflectedProject.h"
#include "Reflector/ProjectSolution.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace Lumina::Reflection
{
    namespace
    {
        // The Reflector executable's own last-write time, computed once. When the tool is newer than a
        // generated artifact the codegen itself may have changed (not just the source header), so every
        // header must be treated as dirty. This mirrors the outer Reflection.lua dirty-check, which
        // re-runs the tool whenever its binary is rebuilt; without it, an emitter change silently
        // produces stale bindings (the generated output never regenerates).
        std::filesystem::file_time_type GetToolWriteTime()
        {
            static const std::filesystem::file_time_type ToolTime = []() -> std::filesystem::file_time_type
            {
            #if defined(_WIN32)
                wchar_t Path[MAX_PATH] = {};
                const DWORD Length = ::GetModuleFileNameW(nullptr, Path, MAX_PATH);
                if (Length > 0 && Length < MAX_PATH)
                {
                    std::error_code Ec;
                    const auto Time = std::filesystem::last_write_time(std::filesystem::path(Path), Ec);
                    if (!Ec)
                    {
                        return Time;
                    }
                }
            #endif
                // Unknown (POSIX port, or query failed): the epoch sentinel is older than any real file
                // time, so the dirty check falls back to source-header-only behavior.
                return std::filesystem::file_time_type{};
            }();
            return ToolTime;
        }
    }

    FReflectedHeader::FReflectedHeader(FReflectedProject* InProject, const std::string& Path)
        : HeaderPath(Path)
        , Project(InProject)
    {
        std::filesystem::path FilesystemPath = Path.c_str();
        FileName = FilesystemPath.stem().string().c_str();

        const std::string& WorkspacePath = InProject->Workspace->GetPath();
        std::string ProjectReflectionPath = WorkspacePath + "/Intermediates/Reflection/" + InProject->Name;
        std::string PossibleReflectedHeaderPath = ProjectReflectionPath + "/" + FileName + ".generated.h";

        bool bReflectionFileExists = std::filesystem::exists(PossibleReflectedHeaderPath.c_str());
        if (!bReflectionFileExists)
        {
            bDirty = true;
            return;
        }

        StartingFileTime = std::filesystem::last_write_time(FilesystemPath);
        auto LastReflectionWrite = std::filesystem::last_write_time(PossibleReflectedHeaderPath.c_str());

        // Dirty when the source header OR the Reflector itself is newer than the last generated output.
        const auto NewestInput = (GetToolWriteTime() > StartingFileTime) ? GetToolWriteTime() : StartingFileTime;
        if (NewestInput > LastReflectionWrite)
        {
            bDirty = true;
        }
    }
}
