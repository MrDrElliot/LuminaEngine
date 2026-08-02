#include "RuntimePCH.h"
#include "PropertyCustomization.h"

#include "imgui.h"
#include "Core/Assertions/Assert.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"

namespace Lumina
{
    FPropertyHandle::FPropertyHandle(void* InContainerPtr, FProperty* InProperty, int64 InIndex)
        : ContainerPtr(InContainerPtr)
        , Property(InProperty)
        , Index(InIndex)
    {
    }

    FPropertyHandle::FPropertyHandle(void* InContainerPtr, void* InDefaultContainerPtr, FProperty* InProperty, int64 InIndex)
        : ContainerPtr(InContainerPtr)
        , DefaultContainerPtr(InDefaultContainerPtr)
        , Property(InProperty)
        , Index(InIndex)
    {
    }

    FPropertyHandle::FPropertyHandle(FArrayProperty* InOwnerArray, void* InArrayPtr, void* InDefaultArrayPtr, FProperty* InElementProperty, int64 InIndex)
        : ContainerPtr(InArrayPtr)
        , DefaultContainerPtr(InDefaultArrayPtr)
        , Property(InElementProperty)
        , Index(InIndex)
        , OwnerArray(InOwnerArray)
    {
    }

    FPropertyHandle::FPropertyHandle(FMapProperty* InOwnerMap, void* InMapPtr, void* InDefaultMapPtr, FProperty* InProperty, int64 InIndex, bool bInIsKey)
        : ContainerPtr(InMapPtr)
        , DefaultContainerPtr(InDefaultMapPtr)
        , Property(InProperty)
        , Index(InIndex)
        , OwnerMap(InOwnerMap)
        , bMapKey(bInIsKey)
    {
    }

    // Resolve the slot for a map entry (key or value) at iteration Index against the given map instance.
    static void* ResolveMapSlot(FMapProperty* Map, void* Container, int64 Index, bool bKey)
    {
        if (Map == nullptr || Container == nullptr || Index < 0 || (size_t)Index >= Map->GetNum(Container))
        {
            return nullptr;
        }
        return bKey ? const_cast<void*>(Map->GetKeyAt(Container, (size_t)Index)) : Map->GetValueAt(Container, (size_t)Index);
    }

    void* FPropertyHandle::GetValuePtr() const
    {
        if (ContainerPtr == nullptr || Property == nullptr)
        {
            return nullptr;
        }
        // Resolve the element fresh each call so a vector reallocation can't leave us with a stale pointer.
        if (OwnerArray != nullptr)
        {
            return Index < (int64)OwnerArray->GetNum(ContainerPtr) ? OwnerArray->GetAt(ContainerPtr, (size_t)Index) : nullptr;
        }
        if (OwnerMap != nullptr)
        {
            return ResolveMapSlot(OwnerMap, ContainerPtr, Index, bMapKey);
        }
        return Property->GetValuePtr<void>(ContainerPtr, Index);
    }

    void* FPropertyHandle::GetDefaultValuePtr() const
    {
        if (DefaultContainerPtr == nullptr || Property == nullptr)
        {
            return nullptr;
        }
        if (OwnerArray != nullptr)
        {
            return Index < (int64)OwnerArray->GetNum(DefaultContainerPtr) ? OwnerArray->GetAt(DefaultContainerPtr, (size_t)Index) : nullptr;
        }
        if (OwnerMap != nullptr)
        {
            return ResolveMapSlot(OwnerMap, DefaultContainerPtr, Index, bMapKey);
        }
        return Property->GetValuePtr<void>(DefaultContainerPtr, Index);
    }

    bool FPropertyHandle::DiffersFromDefault() const
    {
        const void* Value = GetValuePtr();
        const void* Default = GetDefaultValuePtr();
        if (Value == nullptr || Default == nullptr)
        {
            return false;
        }
        return !Property->Identical(Value, Default);
    }

    void FPropertyHandle::ResetToDefault()
    {
        void* Value = GetValuePtr();
        const void* Default = GetDefaultValuePtr();
        if (Value == nullptr || Default == nullptr)
        {
            return;
        }
        Property->CopyCompleteValue(Value, Default);
    }

    EPropertyChangeOp IPropertyTypeCustomization::UpdateAndDraw(const TSharedPtr<FPropertyHandle>& Property, bool bReadOnly)
    {
        ImGui::PushID(Property.get());
        HandleExternalUpdate(Property);
        ImGui::BeginDisabled(bReadOnly);
        EPropertyChangeOp Result = DrawProperty(Property);
        ImGui::EndDisabled();
        ImGui::PopID();

        return Result;
    }

    void FPropertyCustomizationRegistry::RegisterPropertyCustomization(const FName& Name, PropertyCustomizationRegisterFn Callback)
    {
        ASSERT(RegisteredProperties.find(Name) == RegisteredProperties.end());
        RegisteredProperties.emplace(Name, Callback);
    }

    void FPropertyCustomizationRegistry::UnregisterPropertyCustomization(const FName& Name)
    {
        ASSERT(RegisteredProperties.find(Name) != RegisteredProperties.end());
        RegisteredProperties.erase(Name);
    }

    bool FPropertyCustomizationRegistry::IsTypeRegistered(const FName& Name)
    {
        return RegisteredProperties.find(Name) != RegisteredProperties.end();
    }

    TSharedPtr<IPropertyTypeCustomization> FPropertyCustomizationRegistry::GetPropertyCustomizationForType(const FName& Type)
    {
        if (RegisteredProperties.find(Type) != RegisteredProperties.end())
        {
            return RegisteredProperties[Type]();
        }

        return nullptr;
    }
    
}
