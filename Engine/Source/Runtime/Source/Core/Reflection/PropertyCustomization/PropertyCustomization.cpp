#include "RuntimePCH.h"
#include "PropertyCustomization.h"

#include "imgui.h"
#include "Core/Assertions/Assert.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"

namespace Lumina
{
    FPropertyHandle::FPropertyHandle(void* InContainerPtr, FProperty* InProperty, int32 InIndex)
        : ContainerPtr(InContainerPtr)
        , Property(InProperty)
        , Index(InIndex)
    {
    }

    FPropertyHandle::FPropertyHandle(void* InContainerPtr, void* InDefaultContainerPtr, FProperty* InProperty, int32 InIndex)
        : ContainerPtr(InContainerPtr)
        , DefaultContainerPtr(InDefaultContainerPtr)
        , Property(InProperty)
        , Index(InIndex)
    {
    }

    FPropertyHandle::FPropertyHandle(FArrayProperty* InOwnerArray, void* InArrayPtr, void* InDefaultArrayPtr, FProperty* InElementProperty, int32 InIndex)
        : ContainerPtr(InArrayPtr)
        , DefaultContainerPtr(InDefaultArrayPtr)
        , Property(InElementProperty)
        , Index(InIndex)
        , OwnerArray(InOwnerArray)
    {
    }

    FPropertyHandle::FPropertyHandle(FMapProperty* InOwnerMap, void* InMapPtr, void* InDefaultMapPtr, FProperty* InProperty, int32 InIndex, bool bInIsKey)
        : ContainerPtr(InMapPtr)
        , DefaultContainerPtr(InDefaultMapPtr)
        , Property(InProperty)
        , Index(InIndex)
        , OwnerMap(InOwnerMap)
        , bMapKey(bInIsKey)
    {
    }

    // Resolve the slot for a map entry (key or value) at iteration Index against the given map instance.
    static void* ResolveMapSlot(FMapProperty* Map, void* Container, int32 Index, bool bKey)
    {
        if (Map == nullptr || Container == nullptr || Index < 0 || Index >= Map->Num(Container))
        {
            return nullptr;
        }
        return bKey ? const_cast<void*>(Map->GetKeyAt(Container, Index)) : Map->GetValueAt(Container, Index);
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
            return Index < OwnerArray->Num(ContainerPtr) ? OwnerArray->GetAt(ContainerPtr, Index) : nullptr;
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
            return Index < OwnerArray->Num(DefaultContainerPtr) ? OwnerArray->GetAt(DefaultContainerPtr, Index) : nullptr;
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

    EPropertyChangeOp IPropertyTypeCustomization::UpdateAndDraw(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        ImGui::PushID(Property.get());
        HandleExternalUpdate(Property);
        ImGui::BeginDisabled(Args.bReadOnly);
        EPropertyChangeOp Result = DrawProperty(Property, Args);
        ImGui::EndDisabled();
        ImGui::PopID();

        return Result;
    }

    void FPropertyCustomizationRegistry::RegisterPropertyCustomization(const FName& Name, PropertyCustomizationRegisterFn Callback)
    {
        ASSERT(!RegisteredProperties.Contains(Name));
        RegisteredProperties.TryAdd(Name, Callback);
    }

    void FPropertyCustomizationRegistry::UnregisterPropertyCustomization(const FName& Name)
    {
        ASSERT(RegisteredProperties.Contains(Name));
        RegisteredProperties.Remove(Name);
    }

    bool FPropertyCustomizationRegistry::IsTypeRegistered(const FName& Name)
    {
        return RegisteredProperties.Contains(Name);
    }

    TSharedPtr<IPropertyTypeCustomization> FPropertyCustomizationRegistry::GetPropertyCustomizationForType(const FName& Type)
    {
        auto It = RegisteredProperties.Find(Type);
        return It != nullptr ? (*It)() : nullptr;
    }
    
}
