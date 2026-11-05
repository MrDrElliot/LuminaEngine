#include "RuntimePCH.h"
#include "ComponentRefProperty.h"

#include "Core/Object/Class.h"
#include "World/Entity/Components/ComponentRef.h"

namespace Lumina
{
    FComponentRefProperty::FComponentRefProperty(const FPropertyDesc& InDesc)
        : FProperty(InDesc)
    {
        ComponentStruct = InDesc.Type->GetStruct();
        SetElementSize(sizeof(FComponentRef));
        EnumAddFlags(Flags, EPropertyFlags::NoSerialize);
    }

    FString FComponentRefProperty::ToString(const void* Data) const
    {
        const FComponentRef* Ref = static_cast<const FComponentRef*>(Data);
        FString Text = ComponentStruct != nullptr ? FString(*ComponentStruct->MakeDisplayName()) : FString("Component");
        Text += Ref != nullptr && Ref->Resolve() != nullptr ? " (bound)" : " (unbound)";
        return Text;
    }
}
