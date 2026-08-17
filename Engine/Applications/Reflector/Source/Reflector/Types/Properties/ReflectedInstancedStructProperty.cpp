#include "ReflectedInstancedStructProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedInstancedStructProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        const eastl::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);

        // No type name means a bare FInstancedStruct: there is no base symbol to bind, so emit null.
        const eastl::string CustomData = TypeName.empty()
            ? eastl::string("nullptr")
            : ("Construct_CStruct_" + ClangUtils::MakeCodeFriendlyNamespace(TypeName));

        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::InstancedStruct", CustomData);
    }

    eastl::string_view FReflectedInstancedStructProperty::GetLuaType()
    {
        const size_t Pos = TypeName.find_last_of(':');
        if (Pos != eastl::string::npos)
        {
            return eastl::string_view(TypeName).substr(Pos + 1);
        }
        return TypeName;
    }
}
