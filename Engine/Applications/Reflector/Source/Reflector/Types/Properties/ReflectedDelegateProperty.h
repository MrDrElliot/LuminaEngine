#pragma once
#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina
{
    // A TScriptDelegate<T> event member; bHasPayload distinguishes a payload delegate from a no-payload one.
    class FReflectedDelegateProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "Delegate"; }
        const char* GetPropertyParamType() const override { return "FDelegatePropertyParams"; }
        std::string_view GetLuaType() override { return "function"; }

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        bool CanDeclareCrossModuleReferences() const override { return bHasPayload; }
        void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) override
        {
            if (!bHasPayload)
            {
                return;
            }
            const std::string Friendly = ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            Reflection::Names::EmitGuardedCrossModuleDecl(Writer, API, "CStruct", "Construct_CStruct_" + Friendly);
        }

        bool bHasPayload = false;
    };
}
