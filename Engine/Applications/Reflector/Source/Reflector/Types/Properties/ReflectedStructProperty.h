#pragma once
#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina
{
    class FReflectedStructProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "Struct"; }
        const char* GetPropertyParamType() const override { return "FStructPropertyParams"; }
        std::string_view GetLuaType() override;

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        bool CanDeclareCrossModuleReferences() const override { return true; }
        void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) override
        {
            const std::string Friendly = ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            const std::string FnName = "Construct_CStruct_" + Friendly;
            Reflection::Names::EmitGuardedCrossModuleDecl(Writer, API, "CStruct", FnName);
        }
    };
}
