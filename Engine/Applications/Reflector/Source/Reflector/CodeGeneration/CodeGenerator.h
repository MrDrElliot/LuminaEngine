#pragma once
#include "Reflector/ReflectionCore/ReflectionDatabase.h"

namespace Lumina::Reflection
{
    class FReflectedWorkspace;
    class FReflectedDatabase;
    class FCodeWriter;
    
    class FCodeGenerator
    {
    public:

        FCodeGenerator(FReflectedWorkspace* InWorkspace, const FReflectionDatabase& Database);

        void GenerateCode();

    private:

        void GenerateHeaderFile(FReflectedHeader* Header);
        void GenerateSourceFile(FReflectedHeader* Header);
        // bRoutable: header has no reflected types (a SCRIPT_EXPORT free-function facade) and may route its
        // .generated.cs into a plugin/game's Scripts/Generated. Type-bearing headers pass false.
        void GenerateCSharpFile(FReflectedHeader* Header, bool bRoutable);

        void WriteHeaderContent(FCodeWriter& Writer, FReflectedHeader* Header);
        void WriteSourceContent(FCodeWriter& Writer, FReflectedHeader* Header);

        // Returns true if any C#-exposed type was emitted (so the .cs file is only written when non-empty).
        bool WriteCSharpContent(FCodeWriter& Writer, FReflectedHeader* Header);

        // Distributes the project's .generated.cpp names over the fixed set of unity shards.
        void WriteUnityBuildFiles(FReflectedProject* Project, const eastl::vector<eastl::string>& SourceNames);

    private:

        FReflectedWorkspace*       Workspace = nullptr;
        const FReflectionDatabase* ReflectionDatabase = nullptr;
    };
}
