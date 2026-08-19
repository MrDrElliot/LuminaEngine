#pragma once

#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"


namespace Lumina
{
    class FReflectedEnumProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "Enum"; }
        const char* GetPropertyParamType() const override { return "FEnumPropertyParams"; }
        std::string_view GetLuaType() override { return "number"; }

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override
        {
            const std::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
            const std::string CustomData = "Construct_CEnum_" + ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Enum", CustomData);
        }

        bool CanDeclareCrossModuleReferences() const override { return true; }
        void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) override
        {
            const std::string Friendly = ClangUtils::MakeCodeFriendlyNamespace(TypeName);
            const std::string FnName = "Construct_CEnum_" + Friendly;
            Reflection::Names::EmitGuardedCrossModuleDecl(Writer, API, "CEnum", FnName);
        }
    };
}
