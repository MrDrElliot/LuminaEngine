#include "ReflectorInput.h"

#include <cstdint>
#include <string_view>

namespace Lumina::Reflection
{
    namespace ReflectorInputInternal
    {
        constexpr std::string_view kMagic = "LuminaReflector";
        constexpr int kFormatVersion = 1;
    }

    bool ReadReflectorInput(std::istream& In, FReflectorInput& Out, std::string& OutError, uint32_t& OutLine)
    {
        using namespace ReflectorInputInternal;

        Out = FReflectorInput();
        OutLine = 0;
        bool bHeader = false;
        bool bEnded = false;
        std::string Line;

        const auto Fail = [&](std::string Message)
        {
            OutError = std::move(Message);
            return false;
        };

        while (std::getline(In, Line))
        {
            ++OutLine;
            if (!Line.empty() && Line.back() == '\r')
            {
                Line.pop_back();
            }
            if (Line.empty())
            {
                continue;
            }
            if (bEnded)
            {
                return Fail("content after End");
            }

            const size_t Space = Line.find(' ');
            const std::string_view Keyword = std::string_view(Line).substr(0, Space);
            const std::string Value = Space == std::string::npos ? std::string() : Line.substr(Space + 1);

            if (!bHeader)
            {
                if (Keyword != kMagic)
                {
                    return Fail("not a Reflector input stream (it has to start with 'LuminaReflector <version>')");
                }
                if (Value != std::to_string(kFormatVersion))
                {
                    return Fail("format version " + Value + " is not the " + std::to_string(kFormatVersion) + " this Reflector reads; rebuild the build tool and the Reflector together");
                }
                bHeader = true;
                continue;
            }

            if (Keyword == "End")
            {
                bEnded = true;
                continue;
            }
            if (Keyword == "Generation" || Keyword == "ReflectorFingerprint" || Keyword == "CoreFingerprint" || Keyword == "GeneratorFingerprint")
            {
                // Present so a generator change alters the build tool's command key; nothing here needs them.
                continue;
            }
            if (Keyword == "Generator")
            {
                Out.Generators.emplace_back().Library = Value;
                continue;
            }
            if (Keyword == "GeneratorRules" || Keyword == "GeneratorOption")
            {
                if (Out.Generators.empty())
                {
                    return Fail("'" + std::string(Keyword) + "' appears before any Generator");
                }
                const size_t Equals = Value.find('=');
                if (Keyword == "GeneratorRules")
                {
                    Out.Generators.back().RulesFile = Value;
                }
                else if (Equals == std::string::npos)
                {
                    return Fail("GeneratorOption '" + Value + "' is not written as key=value");
                }
                else
                {
                    Out.Generators.back().Options.emplace_back(Value.substr(0, Equals), Value.substr(Equals + 1));
                }
                continue;
            }
            if (Keyword == "Workspace")
            {
                Out.WorkspaceName = Value;
                continue;
            }
            if (Keyword == "WorkspacePath")
            {
                Out.WorkspacePath = Value;
                continue;
            }
            if (Keyword == "Module")
            {
                if (Value.empty())
                {
                    return Fail("a Module record has no name");
                }
                Out.Modules.emplace_back().Name = Value;
                continue;
            }

            if (Out.Modules.empty())
            {
                return Fail("'" + std::string(Keyword) + "' appears before any Module");
            }
            FReflectorModuleInput& Module = Out.Modules.back();

            if (Keyword == "Path")                   { Module.Path = Value; }
            else if (Keyword == "GeneratedDir")      { Module.GeneratedDir = Value; }
            else if (Keyword == "PrecompiledHeader") { Module.PrecompiledHeader = Value; }
            else if (Keyword == "CSharpBindingsDir") { Module.CSharpBindingsDir = Value; }
            else if (Keyword == "RouteTypeBindings") { Module.bRouteTypeBindings = true; }
            else if (Keyword == "ReferenceOnly")     { Module.bReferenceOnly = true; }
            else if (Keyword == "Include")           { Module.IncludeDirs.push_back(Value); }
            else if (Keyword == "Define")            { Module.Definitions.push_back(Value); }
            else if (Keyword == "ForceInclude")      { Module.ForceIncludes.push_back(Value); }
            else if (Keyword == "Header")            { Module.Headers.push_back(Value); }
            else
            {
                return Fail("unknown record '" + std::string(Keyword) + "'");
            }
        }

        if (!bHeader)
        {
            return Fail("the input is empty");
        }
        if (!bEnded)
        {
            return Fail("the input ends without End, so it was cut short");
        }
        if (Out.WorkspacePath.empty())
        {
            return Fail("no WorkspacePath record");
        }
        for (const FReflectorModuleInput& Module : Out.Modules)
        {
            if (Module.Path.empty())
            {
                OutLine = 0;
                return Fail("module '" + Module.Name + "' has no Path record");
            }
        }
        return true;
    }
}
