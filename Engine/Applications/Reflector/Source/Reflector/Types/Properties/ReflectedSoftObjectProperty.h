#pragma once
#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina
{
    /** Mirror of FReflectedObjectProperty for TSoftObjectPtr<T>/FSoftObjectPath.
     *  Emits a SoftObjectProperty so the runtime serializer routes through
     *  FSoftObjectPath::operator<<, which writes (Path, GUID) and registers
     *  the GUID with the saver for cook-graph Soft edges. */
    class FReflectedSoftObjectProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "SoftObject"; }
        const char* GetPropertyParamType() const override { return "FSoftObjectPropertyParams"; }
        std::string_view GetLuaType() override;

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;
        bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) override { return true; }

        void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) override
        {
            const std::string Friendly = ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            const std::string FnName = "Construct_CClass_" + Friendly;
            Reflection::Names::EmitGuardedCrossModuleDecl(Writer, API, "CClass", FnName);
        }
    };
}
