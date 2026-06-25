#include "ReflectedDelegateProperty.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/Types/ReflectedType.h"

namespace Lumina
{
    void FReflectedDelegateProperty::AppendDefinition(Reflection::FCodeWriter& Writer) const
    {
        // Bindings are runtime-only; force NoSerialize regardless of declared specifiers.
        const EPropertyFlags Flags = PropertyFlags | EPropertyFlags::NoSerialize;
        const eastl::string PropertyFlagStr = PropertyFlagsToString(Flags);

        // PayloadStructFunc; the payload struct factory, or nullptr for a no-payload delegate.
        const eastl::string CustomData = bHasPayload
            ? ("Construct_CStruct_" + ClangUtils::MakeCodeFriendlyNamespace(TypeName))
            : eastl::string("nullptr");

        AppendPropertyDef(Writer, PropertyFlagStr.c_str(), "Lumina::EPropertyTypeFlags::Delegate", CustomData);
    }
}
