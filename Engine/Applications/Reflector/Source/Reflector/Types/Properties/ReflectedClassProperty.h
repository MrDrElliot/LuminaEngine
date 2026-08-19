#pragma once
#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina
{
    // Emits an FClassPropertyParams for a TSubclassOf<T> field. TypeName is the inner T (the base
    // class filter); ClassFunc resolves to Construct_CClass_<T>.
    class FReflectedClassProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "Class"; }
        const char* GetPropertyParamType() const override { return "FClassPropertyParams"; }
        std::string_view GetLuaType() override;

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        bool CanDeclareCrossModuleReferences() const override { return true; }
        void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) override
        {
            const std::string Friendly = ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            const std::string FnName = "Construct_CClass_" + Friendly;
            Reflection::Names::EmitGuardedCrossModuleDecl(Writer, API, "CClass", FnName);
        }
    };
}
