#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace Lumina::Reflection
{
    struct FReflectorModuleInput
    {
        std::string              Name;
        std::string              Path;
        std::string              GeneratedDir;
        std::string              PrecompiledHeader;
        std::string              CSharpBindingsDir;
        bool                     bRouteTypeBindings = false;
        bool                     bReferenceOnly     = false;
        std::vector<std::string> IncludeDirs;
        std::vector<std::string> Definitions;
        std::vector<std::string> ForceIncludes;
        std::vector<std::string> Headers;
    };

    struct FReflectorInput
    {
        std::string                        WorkspaceName;
        std::string                        WorkspacePath;
        std::vector<FReflectorModuleInput> Modules;
    };

    // Reads what LuminaBuildTool's ReflectorInput.cs writes. On failure OutError says why and OutLine where.
    bool ReadReflectorInput(std::istream& In, FReflectorInput& Out, std::string& OutError, uint32_t& OutLine);
}
