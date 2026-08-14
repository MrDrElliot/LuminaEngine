#pragma once

#include <filesystem>
#include "EASTL/shared_ptr.h"
#include "EASTL/string.h"
#include "EASTL/vector.h"

namespace Lumina::Reflection
{
    class FReflectedProject;

    class FReflectedWorkspace
    {
    public:

        FReflectedWorkspace(const std::filesystem::path& ReflectionPath);
        
        const eastl::string& GetPath() const { return Path; }

        void AddReflectedProject(eastl::unique_ptr<FReflectedProject>&& Project);
        bool HasProjects() const { return !ReflectedProjects.empty(); }
        
        eastl::string Path;
        eastl::vector<eastl::unique_ptr<FReflectedProject>> ReflectedProjects;
        
    };
}
