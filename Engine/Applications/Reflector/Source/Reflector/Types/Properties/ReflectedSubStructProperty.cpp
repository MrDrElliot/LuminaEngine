#include "ReflectedSubStructProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedSubStructProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        const std::string PropertyFlagStr = PropertyFlagsToString(PropertyFlags);
        const std::string CustomData = "Construct_CStruct_" + ClangUtils::MakeCodeFriendlyNamespace(TypeName);
        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::SubStruct", CustomData);
    }

    std::string_view FReflectedSubStructProperty::GetLuaType()
    {
        const size_t Pos = TypeName.find_last_of(':');
        if (Pos != std::string::npos)
        {
            return std::string_view(TypeName).substr(Pos + 1);
        }
        return TypeName;
    }
}
