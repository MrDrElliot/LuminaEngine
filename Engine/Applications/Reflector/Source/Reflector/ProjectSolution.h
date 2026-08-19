#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Lumina::Reflection
{
    class FReflectedProject;

    class FReflectedWorkspace
    {
    public:

        FReflectedWorkspace(const std::filesystem::path& ReflectionPath);
        
        const std::string& GetPath() const { return Path; }

        void AddReflectedProject(std::unique_ptr<FReflectedProject>&& Project);
        bool HasProjects() const { return !ReflectedProjects.empty(); }
        
        std::string Path;
        std::vector<std::unique_ptr<FReflectedProject>> ReflectedProjects;
        
    };
}
