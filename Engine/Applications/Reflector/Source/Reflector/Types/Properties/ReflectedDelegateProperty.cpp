#include "ReflectedDelegateProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedDelegateProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        // Bindings are runtime-only; force NoSerialize regardless of declared specifiers.
        const EPropertyFlags Flags = PropertyFlags | EPropertyFlags::NoSerialize;
        const std::string PropertyFlagStr = PropertyFlagsToString(Flags);

        // NumArgs; the argument params the emitter wrote ahead of this one.
        const std::string CustomData = std::to_string(NumArgs);

        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Delegate", CustomData);
    }
}
