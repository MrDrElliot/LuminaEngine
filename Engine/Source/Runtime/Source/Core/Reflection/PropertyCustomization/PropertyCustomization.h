#pragma once
#include "Containers/Function.h"
#include "Memory/SmartPtr.h"
#include "Core/Reflection/PropertyCustomization/PropertyEditContext.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    class FArrayProperty;
    class FMapProperty;

    class RUNTIME_API FPropertyHandle
    {
    public:

        FPropertyHandle(void* InContainerPtr, FProperty* InProperty, int64 InIndex = 0);
        FPropertyHandle(void* InContainerPtr, void* InDefaultContainerPtr, FProperty* InProperty, int64 InIndex = 0);

        /** Array-element handle; ContainerPtr is the array instance (resolved via GetAt per access,
         *  so it survives reallocation), InProperty is the element property. */
        FPropertyHandle(FArrayProperty* InOwnerArray, void* InArrayPtr, void* InDefaultArrayPtr, FProperty* InElementProperty, int64 InIndex);

        /** Map-entry handle; ContainerPtr is the map instance, resolved by iteration Index each access. bInIsKey
         *  selects the key or value slot of that pair; InProperty is the key or value property accordingly. */
        FPropertyHandle(FMapProperty* InOwnerMap, void* InMapPtr, void* InDefaultMapPtr, FProperty* InProperty, int64 InIndex, bool bInIsKey);

        /** Resolves to the property's value (the member itself, or the live array element). Null container yields null. */
        void* GetValuePtr() const;

        /** As GetValuePtr but against the default container; null when no default is plumbed. */
        void* GetDefaultValuePtr() const;

        /** Accessor-aware typed read/write for plain members; direct (no accessor) for array elements. */
        template<typename T>
        void GetValue(T* OutValue) const
        {
            if (OwnerArray != nullptr || OwnerMap != nullptr)
            {
                if (const void* Ptr = GetValuePtr())
                {
                    *OutValue = *static_cast<const T*>(Ptr);
                }
            }
            else
            {
                Property->GetValue(ContainerPtr, OutValue, Index);
            }
        }

        template<typename T>
        void SetValue(const T& InValue) const
        {
            if (OwnerArray != nullptr || OwnerMap != nullptr)
            {
                if (void* Ptr = GetValuePtr())
                {
                    *static_cast<T*>(Ptr) = InValue;
                }
            }
            else
            {
                Property->SetValue(ContainerPtr, InValue, Index);
            }
        }

        /** False if no default container plumbed (e.g. struct edited without a CDO). */
        bool DiffersFromDefault() const;

        /** No-op without default. Caller handles change notifications. */
        void ResetToDefault();

        bool HasDefault() const { return DefaultContainerPtr != nullptr; }

        void* ContainerPtr;
        void* DefaultContainerPtr = nullptr;
        FProperty* Property;
        int64 Index = 0;

        /** Non-null marks this as an array element; ContainerPtr is then the array instance. */
        FArrayProperty* OwnerArray = nullptr;

        /** Non-null marks this as a map entry; ContainerPtr is then the map instance, addressed by Index. */
        FMapProperty* OwnerMap = nullptr;
        /** For a map entry, true resolves the key slot, false the value slot. */
        bool bMapKey = false;
    };

    enum class EPropertyChangeOp : uint8
    {
        None,
        Updated,
        Started,
        Finished,
    };

    // Brackets dirty frames into one edit session, since only Finished reaches FinishChangeCallback.
    class FPropertyEditSession
    {
    public:

        // bInteractionActive is a held drag or an open popup; false closes the session on the next frame.
        EPropertyChangeOp Advance(bool bDirty, bool bInteractionActive)
        {
            if (bDirty)
            {
                if (bOpen)
                {
                    return EPropertyChangeOp::Updated;
                }
                bOpen = true;
                return EPropertyChangeOp::Started;
            }

            if (bOpen && !bInteractionActive)
            {
                bOpen = false;
                return EPropertyChangeOp::Finished;
            }

            return EPropertyChangeOp::None;
        }

    private:

        bool bOpen = false;
    };
    
    struct RUNTIME_API IPropertyTypeCustomization : TSharedFromThis<IPropertyTypeCustomization>
    {
    public:

        virtual ~IPropertyTypeCustomization() = default;

        virtual EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) = 0;
        
        EPropertyChangeOp UpdateAndDraw(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args);

        virtual void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) = 0;

        virtual void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) = 0;
        
    };
    
    class RUNTIME_API FPropertyCustomizationRegistry
    {
    public:
        using PropertyCustomizationRegisterFn = TFunction<TSharedPtr<IPropertyTypeCustomization>()>;

        
        void RegisterPropertyCustomization(const FName& Name, PropertyCustomizationRegisterFn Callback);
        void UnregisterPropertyCustomization(const FName& Name);

        bool IsTypeRegistered(const FName& Name);
        
        TSharedPtr<IPropertyTypeCustomization> GetPropertyCustomizationForType(const FName& Type);
    

    private:
        
        THashMap<FName, PropertyCustomizationRegisterFn> RegisteredProperties;
    };
}
