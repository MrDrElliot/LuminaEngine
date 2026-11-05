#pragma once

#include "Core/Object/Field.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Serialization/Structured/StructuredArchive.h"
#include "Platform/GenericPlatform.h"
#include "Log/Log.h"
#include "Containers/Optional.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    class CStruct;
    class IStructuredArchive;
    class FNetArchive;
}

namespace Lumina
{
    /** Exactly one cache line; keep the member block packed when adding to it. */
    class LUMINA_VISIBLE_TYPE FProperty
    {
    public:
        
        explicit FProperty(const FPropertyDesc& InDesc)
            : Name(InDesc.Name)
            , Offset(InDesc.Offset)
            , Flags(InDesc.Flags | InDesc.Type->Flags)
            , TypeFlags(InDesc.Type->Kind)
            , Desc(&InDesc)
        {
        }
        
        virtual ~FProperty() = default;

        LE_NO_COPYMOVE(FProperty);

        const FName& GetPropertyName() const { return Name; }

        /** Empty until the metadata pass runs, which is editor-only. */
        FCStringView GetPropertyDisplayName() const { return DisplayName != nullptr ? FCStringView(DisplayName) : FCStringView(); }

        /** Attaches an inner property (array element, map key/value, enum or optional payload). */
        RUNTIME_API virtual void AddProperty(FProperty* Property) { UNREACHABLE(); }

        RUNTIME_API size_t GetElementSize() const { return ElementSize; }
        RUNTIME_API void SetElementSize(size_t Size) { ElementSize = (uint32)Size; }
        RUNTIME_API EPropertyTypeFlags GetType() const { return TypeFlags; }

        /** The struct that declares this property, or null for a container inner. */
        NODISCARD CStruct* GetOwnerStruct() const { return OwnerStruct; }

        template<typename ValueType>
        ValueType* SetValuePtr(void* ContainerPtr, const ValueType& Value, int32 ArrayIndex = 0) const
        {
            ValueType* ValuePtr = GetValuePtr<ValueType>(ContainerPtr, ArrayIndex);
            *ValuePtr = Value;
            return ValuePtr;
        }

        /** UB if ValueType doesn't match the property type. */
        template<typename ValueType>
        requires (!std::is_pointer_v<ValueType>)
        ValueType* GetValuePtr(void* ContainerPtr, int32 ArrayIndex = 0) const
        {
            return static_cast<ValueType*>(GetValuePtrInternal(ContainerPtr, ArrayIndex));
        }

        template<typename ValueType>
        requires (!std::is_pointer_v<ValueType>)
        const ValueType* GetValuePtr(const void* ContainerPtr, int32 ArrayIndex = 0) const
        {
            return static_cast<ValueType*>(GetValuePtrInternal(const_cast<void*>(ContainerPtr), ArrayIndex));
        }

        template<typename ValueType>
        void SetValue(void* InContainer, const ValueType& InValue, int32 ArrayIndex = 0) const
        {
            if (!HasSetter())
            {
                SetValuePtr<ValueType>(InContainer, InValue, ArrayIndex);
            }
            else
            {
                CallSetter(InContainer, &InValue);
            }
        }

        template<typename ValueType>
        void GetValue(void const* InContainer, ValueType* OutValue, int32 ArrayIndex = 0) const
        {
            if (!HasGetter())
            {
                const ValueType* Src = GetValuePtr<ValueType>(InContainer, ArrayIndex);
                *OutValue = *Src;
            }
            else
            {
                CallGetter(InContainer, OutValue);
            }
        }

        virtual void Serialize(FArchive& Ar, void* Value) { }

        /** Bring this property's storage up / tear it down in caller-owned memory. The default is correct for
         *  every trivially-constructible kind: their zeroed bytes are already a valid value, and they need no
         *  teardown. A kind that owns memory (a string, a container, a struct with either) overrides both.
         *
         *  This is what lets a holder of properties drive lifecycle without knowing the kinds it holds --
         *  adding a new property type teaches the type itself, not every walker. */
        virtual void ConstructValue(void* Value) const { }
        virtual void DestructValue(void* Value) const { }

        // Moves a live value into raw memory and ends the source; a kind whose identity a copy would lose overrides it.
        virtual void RelocateValue(void* Dest, void* Source) const
        {
            ConstructValue(Dest);
            CopyCompleteValue(Dest, Source);
            DestructValue(Source);
        }

        /** True when this property's value owns memory, i.e. it overrides the pair above. Lets a holder ask
         *  the property whether it needs lifecycle instead of testing a list of kinds. */
        virtual bool OwnsStorage() const { return false; }
        virtual void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults = nullptr) { }

        // Compact network serialization without a property header, which a type overrides to quantize.
        RUNTIME_API virtual void NetSerialize(FNetArchive& Ar, void* Value);

        /** Defaults to byte-wise memcmp; non-trivial types (FString, structs, arrays) override. */
        RUNTIME_API virtual bool Identical(const void* ValueA, const void* ValueB) const;

        /** Defaults to memcpy on ElementSize; non-trivial types override. */
        RUNTIME_API virtual void CopyCompleteValue(void* Dst, const void* Src) const;

        RUNTIME_API bool Identical_InContainer(const void* ContainerA, const void* ContainerB, int32 ArrayIndex = 0) const;
        RUNTIME_API void CopyCompleteValue_InContainer(void* DstContainer, const void* SrcContainer, int32 ArrayIndex = 0) const;

        RUNTIME_API bool IsA(EPropertyTypeFlags Flag) const { return TypeFlags == Flag; }

        // True when a value of Other can be copied into this property, the check before handing a value to a function parameter.
        RUNTIME_API bool HasSameValueType(const FProperty* Other) const;
        
        RUNTIME_API const FName& GetTypeName() const;
        
        NODISCARD bool IsReadOnly()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ReadOnly); }
        NODISCARD bool IsEditorOnly()   const       { return EnumHasAnyFlags(Flags, EPropertyFlags::EditorOnly); }
        NODISCARD bool IsReplicated()   const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Replicated); }
        NODISCARD bool IsDuplicateTransient() const { return EnumHasAnyFlags(Flags, EPropertyFlags::DuplicateTransient); }
        NODISCARD bool IsSaveGame()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::SaveGame); }
        NODISCARD bool IsOutParam()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::OutParam); }
        NODISCARD bool IsRefParam()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::RefParam); }
        NODISCARD bool ShouldSerialize()const       { return !EnumHasAnyFlags(Flags, EPropertyFlags::NoSerialize); }
        NODISCARD bool IsConst()        const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Const); }
        NODISCARD bool IsProtected()    const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Protected); }
        NODISCARD bool IsScriptHidden() const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ScriptHidden); }
        NODISCARD bool IsVisible()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ReadOnly | EPropertyFlags::Editable); }
        NODISCARD bool IsTrivial()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Trivial); }

        
        /** Empty when the key is absent. Null-terminated, so it hands straight to a C API. */
        RUNTIME_API FCStringView GetMetadata(FStringView Key) const;
        RUNTIME_API bool HasMetadata(FStringView Key) const;
        RUNTIME_API const FMetadataEntry* FindMetadata(FStringView Key) const;

        // Unset when the key is absent or its value is not a number, which the Reflector already refuses for a C++ property.
        RUNTIME_API TOptional<double> GetMetadataNumber(FStringView Key) const;

        template<typename T>
        TOptional<T> GetMetadataNumber(FStringView Key) const
        {
            const TOptional<double> Value = GetMetadataNumber(Key);
            return Value ? TOptional<T>(static_cast<T>(*Value)) : TOptional<T>();
        }

        NODISCARD bool HasEditorFlag(EPropertyEditorFlags Flag) const { return EnumHasAnyFlags(Desc->EditorFlags, Flag); }

        /** Places the display name in Arena, so it costs a pointer rather than an FString and a malloc. */
        RUNTIME_API void OnMetadataFinalized(FPropertyArena& Arena);
        static FString MakeDisplayNameFromName(EPropertyTypeFlags TypeFlags, const FName& InName);

        RUNTIME_API virtual FString ToString(const void* Data) const { return "<unknown>"; }
        
        NODISCARD const FPropertyDesc& GetDesc() const { return *Desc; }

        NODISCARD bool HasSetter() const { return Desc->Setter != nullptr; }
        NODISCARD bool HasGetter() const { return Desc->Getter != nullptr; }
        NODISCARD bool HasSetterOrGetter() const { return HasSetter() || HasGetter(); }

        void CallSetter(void* Container, const void* InValue) const { Desc->Setter(Container, InValue); }
        void CallGetter(const void* Container, void* OutValue) const { Desc->Getter(Container, OutValue); }

        
    private:

        RUNTIME_API void* GetValuePtrInternal(void* ContainerPtr, int32 ArrayIndex) const;
        
    public:

        FName               Name;

        uint32              Offset = 0;
        uint32              ElementSize = 0;
        EPropertyFlags      Flags = EPropertyFlags::None;
        EPropertyTypeFlags  TypeFlags = EPropertyTypeFlags::None;

        // Arena-owned, so a display name costs a pointer here instead of an FString and its allocation.
        const char*         DisplayName = nullptr;

        // Set when the property is attached; an inner is reached through the property that holds it instead.
        CStruct*            OwnerStruct = nullptr;

        // The generated descriptor, or one the arena built for a script or inner property.
        const FPropertyDesc* Desc = nullptr;
    };

    class FNumericProperty : public FProperty
    {
    public:

        explicit FNumericProperty(const FPropertyDesc& InDesc)
            :FProperty(InDesc)
        {}

        RUNTIME_API virtual void SetIntPropertyValue(void* Data, uint64 Value) const { }
        RUNTIME_API virtual void SetIntPropertyValue(void* Data, int64 Value) const { }
        
        RUNTIME_API virtual int64 GetSignedIntPropertyValue(void const* Data) const { return 0; }
        RUNTIME_API virtual int64 GetSignedIntPropertyValue_InContainer(void const* Container) const { return 0; }
        
        RUNTIME_API virtual uint64 GetUnsignedIntPropertyValue(void const* Data) const { return 0; }
        RUNTIME_API virtual uint64 GetUnsignedIntPropertyValue_InContainer(void const* Container) const { return 0; }
        
    };
    
    template<typename TCPPType>
    class TPropertyTypeLayout
    {
    public:

        enum : uint8
        {
            Size = sizeof(TCPPType),
            Alignment = alignof(TCPPType)
        };

        static const TCPPType* GetPropertyValuePtr(const void* Ptr)
        {
            return static_cast<const TCPPType*>(Ptr);
        }

        static TCPPType* GetPropertyValuePtr(void* Ptr)
        {
            return static_cast<TCPPType*>(Ptr);
        }

        static TCPPType const& GetPropertyValue(void const* A)
        {
            return *GetPropertyValuePtr(A);
        }

        static void SetPropertyValue(void* Ptr, const TCPPType& Value)
        {
            *GetPropertyValuePtr(Ptr) = Value;
        }

    };
    
    template<typename TBacking, typename TCPPType>
    class TProperty : public TBacking
    {
    public:

        using TTypeInfo = TPropertyTypeLayout<TCPPType>;
        
        explicit TProperty(const FPropertyDesc& InDesc)
            :TBacking(InDesc)
        {
            this->ElementSize = TTypeInfo::Size;
        }

        virtual void Serialize(FArchive& Ar, void* Value) override
        {
            Ar << *TTypeInfo::GetPropertyValuePtr(Value);
        }

        virtual void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults = nullptr) override
        {
            Slot.Serialize(*TTypeInfo::GetPropertyValuePtr(Value));
        }
        
    };


    template<typename TCPPType>
    requires std::is_arithmetic_v<TCPPType>
    class TProperty_Numeric : public TProperty<FNumericProperty, TCPPType>
    {
    public:
        
        using TTypeInfo = TPropertyTypeLayout<TCPPType>;
        using Super = TProperty<FNumericProperty, TCPPType>;

        explicit TProperty_Numeric(const FPropertyDesc& InDesc)
            :Super(InDesc)
        {}

        virtual FString ToString(const void* Data) const override;
        
        virtual void SetIntPropertyValue(void* Data, uint64 Value) const override;
        virtual void SetIntPropertyValue(void* Data, int64 Value) const override;
        
        virtual int64 GetSignedIntPropertyValue(void const* Data) const override;
        virtual int64 GetSignedIntPropertyValue_InContainer(void const* Container) const override;
        
        virtual uint64 GetUnsignedIntPropertyValue(void const* Data) const override;
        virtual uint64 GetUnsignedIntPropertyValue_InContainer(void const* Container) const override;

        
    };
    
    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    FString TProperty_Numeric<TCPPType>::ToString(const void* Data) const
    {
        return Format("{}", TTypeInfo::GetPropertyValue(Data));
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    void TProperty_Numeric<TCPPType>::SetIntPropertyValue(void* Data, uint64 Value) const
    {
        TTypeInfo::SetPropertyValue(Data, static_cast<TCPPType>(Value)); 
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    void TProperty_Numeric<TCPPType>::SetIntPropertyValue(void* Data, int64 Value) const
    {
        TTypeInfo::SetPropertyValue(Data, static_cast<TCPPType>(Value)); 
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    int64 TProperty_Numeric<TCPPType>::GetSignedIntPropertyValue(void const* Data) const
    {
        return static_cast<int64>(TTypeInfo::GetPropertyValue(Data));
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    int64 TProperty_Numeric<TCPPType>::GetSignedIntPropertyValue_InContainer(void const* Container) const
    {
        return static_cast<int64>(TTypeInfo::GetPropertyValue(Container));
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    uint64 TProperty_Numeric<TCPPType>::GetUnsignedIntPropertyValue(void const* Data) const
    {
        return static_cast<uint64>(TTypeInfo::GetPropertyValue(Data));
    }

    template <typename TCPPType> requires std::is_arithmetic_v<TCPPType>
    uint64 TProperty_Numeric<TCPPType>::GetUnsignedIntPropertyValue_InContainer(void const* Container) const
    {
        return static_cast<uint64>(TTypeInfo::GetPropertyValue(Container));
    }

    class FBoolProperty : public TProperty_Numeric<bool>
    {
    public:
        using Super = TProperty_Numeric<bool>;

        explicit FBoolProperty(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}

        // Tight: a bool is one bit on the wire.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;
    };
    
    class FInt8Property : public TProperty_Numeric<int8>
    {
    public:
        using Super = TProperty_Numeric<int8>;

        explicit FInt8Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FInt16Property : public TProperty_Numeric<int16>
    {
    public:
        using Super = TProperty_Numeric<int16>;

        explicit FInt16Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FInt32Property : public TProperty_Numeric<int32>
    {
    public:
        using Super = TProperty_Numeric<int32>;

        explicit FInt32Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FInt64Property : public TProperty_Numeric<int64>
    {
    public:
        using Super = TProperty_Numeric<int64>;

        explicit FInt64Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FUInt8Property : public TProperty_Numeric<uint8>
    {
    public:
        using Super = TProperty_Numeric<uint8>;

        explicit FUInt8Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FUInt16Property : public TProperty_Numeric<uint16>
    {
    public:
        using Super = TProperty_Numeric<uint16>;

        explicit FUInt16Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FUInt32Property : public TProperty_Numeric<uint32>
    {
    public:
        using Super = TProperty_Numeric<uint32>;

        explicit FUInt32Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    // An ECS::FEntity, stored as its packed uint32 so this layer needs no ECS header.
    class FEntityProperty : public TProperty_Numeric<uint32>
    {
    public:
        using Super = TProperty_Numeric<uint32>;

        explicit FEntityProperty(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}

        // Routed through the archive so a save game can translate ids between worlds.
        RUNTIME_API void Serialize(FArchive& Ar, void* Value) override;

        // Sent as the id both peers share when the archive can translate one, since a raw entity id is local.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;
    };

    class FUInt64Property : public TProperty_Numeric<uint64>
    {
    public:
        using Super = TProperty_Numeric<uint64>;

        explicit FUInt64Property(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FFloatProperty : public TProperty_Numeric<float>
    {
    public:
        using Super = TProperty_Numeric<float>;

        explicit FFloatProperty(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    class FDoubleProperty : public TProperty_Numeric<double>
    {
    public:
        using Super = TProperty_Numeric<double>;

        explicit FDoubleProperty(const FPropertyDesc& InDesc)
            : Super(InDesc)
        {}
    };

    

}
