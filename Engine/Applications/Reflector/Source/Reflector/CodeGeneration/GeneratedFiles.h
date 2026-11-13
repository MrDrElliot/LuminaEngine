#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <vector>

#include "Reflector/Public/ReflectionGenerator.h"

// Where the built-in generators put their files, shared so the C++ and C# halves of one file agree on its path.
namespace Lumina::Reflection::GeneratedFiles
{
    // Pinned per target, or two targets sharing a module each see the other's output as a change.
    inline std::string ModuleDir(const FModuleView& Module)
    {
        return !Module.GeneratedDir().empty() ? Module.GeneratedDir() : Module.GetModel().WorkspacePath() + "/Intermediates/Reflection/" + Module.Name();
    }

    inline std::string HeaderPath(const FHeaderView& Header)
    {
        return ModuleDir(Header.Module()) + "/" + Header.FileName() + ".generated.h";
    }

    inline std::string SourcePath(const FHeaderView& Header)
    {
        return ModuleDir(Header.Module()) + "/" + Header.FileName() + ".generated.cpp";
    }

    inline std::string DefaultCSharpDir(const FModuleView& Module)
    {
        return Module.GetModel().WorkspacePath() + "/Intermediates/CSharpBindings/" + Module.Name();
    }

    // A unit compiling its own assembly routes its reflected-type bindings into it.
    inline bool RoutesTypeBindings(const FModuleView& Module)
    {
        return Module.RoutesTypeBindings() && !Module.CSharpBindingsDir().empty();
    }

    // bRoutable sends a binding to the owner's Scripts/Generated, to compile into that unit's own assembly.
    inline std::string CSharpPath(const FHeaderView& Header, bool bRoutable)
    {
        const FModuleView Module = Header.Module();
        const std::string Dir = bRoutable && !Module.CSharpBindingsDir().empty() ? Module.CSharpBindingsDir() : DefaultCSharpDir(Module);
        return Dir + "/" + Header.FileName() + ".generated.cs";
    }

    inline std::string LayoutStampFileName(const FModuleView& Module)
    {
        return Module.Name() + ".Layout.generated.cs";
    }

    // Next to the module's type bindings, so the stamp compiles into whichever assembly holds them.
    inline std::string LayoutStampPath(const FModuleView& Module)
    {
        return (RoutesTypeBindings(Module) ? Module.CSharpBindingsDir() : DefaultCSharpDir(Module)) + "/" + LayoutStampFileName(Module);
    }

    inline std::string HexLiteral(uint64_t Value)
    {
        char Buffer[32];
        std::snprintf(Buffer, sizeof(Buffer), "0x%016llX", static_cast<unsigned long long>(Value));
        return Buffer;
    }

    inline std::vector<FTypeView> ModuleTypes(const FModuleView& Module)
    {
        std::vector<FTypeView> Types;
        for (const FHeaderView Header : Module.Headers())
        {
            for (const FTypeView Type : Header.Types())
            {
                Types.push_back(Type);
            }
        }
        return Types;
    }

    // Every layout fact the C# bindings bake in, so a binding set and a native build can tell they disagree.
    inline uint64_t ModuleLayoutHash(const FModuleView& Module)
    {
        std::vector<FTypeView> Types = ModuleTypes(Module);
        std::erase_if(Types, [](const FTypeView& Type) { return Type.NativeSize() <= 0; });
        std::sort(Types.begin(), Types.end(), [](const FTypeView& A, const FTypeView& B) { return A.QualifiedName() < B.QualifiedName(); });

        uint64_t Hash = 14695981039346656037ULL;
        auto Mix = [&Hash](const std::string& Fact)
        {
            for (const char Char : Fact)
            {
                Hash = (Hash ^ static_cast<uint8_t>(Char)) * 1099511628211ULL;
            }
            Hash = (Hash ^ 0xFFu) * 1099511628211ULL;
        };
        for (const FTypeView& Type : Types)
        {
            Mix(Type.QualifiedName() + ":" + std::to_string(Type.NativeSize()) + ":" + std::to_string(Type.NativeAlign()));
            for (const FPropertyView Property : Type.Properties())
            {
                if (!Property.IsInner() && Property.NativeOffset() >= 0)
                {
                    Mix(Property.Name() + "@" + std::to_string(Property.NativeOffset()));
                }
            }
        }
        return Hash;
    }

    // A deleted header leaves stale generated files nothing else visits, so any unexpected one is removed.
    inline void SweepOrphans(FGeneratedOutputs& Outputs, const std::string& DirPath, const std::unordered_set<std::string>& Expected,
        std::initializer_list<const char*> Suffixes)
    {
        std::error_code Ec;
        for (const auto& Entry : std::filesystem::directory_iterator(std::filesystem::path(DirPath.c_str()), Ec))
        {
            if (!Entry.is_regular_file(Ec))
            {
                continue;
            }
            const std::string Filename = Entry.path().filename().string();
            const bool bGenerated = std::any_of(Suffixes.begin(), Suffixes.end(),
                [&Filename](const char* Suffix) { return Filename.find(Suffix) != std::string::npos; });
            if (bGenerated && !Expected.contains(Filename))
            {
                Outputs.Remove(Entry.path().string());
            }
        }
    }
}
