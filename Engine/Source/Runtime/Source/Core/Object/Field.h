#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Assertions/Assert.h"
#include "Core/Variant/Variant.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FField;
    class CStruct;
    class FProperty;
    
    using FFieldOwner = TVariant<CStruct*, FField*>;
    
    class FField
    {
    public:

        FField(const FFieldOwner& InOwner)
            :Owner(InOwner)
        {
            Offset = 0;
            Next = nullptr;
        }

        virtual ~FField() = default;

        RUNTIME_API virtual void AddProperty(FProperty* Property) { UNREACHABLE(); }

        const FName& GetPropertyName() const { return Name; }
        const FString& GetPropertyDisplayName() const { return DisplayName; }
        
        
        FName               Name;
        FString             DisplayName;
        
        uint32              Offset;
        FField*             Next;
        FFieldOwner         Owner;
    };
    
}
