#pragma once

#include "Core/Object/Field.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/PropertyArena.h"
#include "Core/Serialization/Structured/StructuredArchive.h"
#include "Metadata/PropertyMetadata.h"
#include "Platform/GenericPlatform.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    struct FPropertyParams;
    class CStruct;
    class IStructuredArchive;
    class FNetArchive;
}

namespace Lumina
{

    #define DECLARE_FPROPERTY(Type) \
    static EPropertyTypeFlags StaticType() { return Type; }
    
    /** Exactly one cache line; keep the member block packed when adding to it. */
    class FProperty
    {
    public:
        
        explicit FProperty(const FPropertyParams* Params)
        {
            Offset      = Params->Offset;
            Name        = Params->Name;
            TypeFlags   = Params->TypeFlags;
            Flags       = Params->PropertyFlags;
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
        ValueType* SetValuePtr(void* ContainerPtr, const ValueType& Value, int64 ArrayIndex = 0) const
        {
            ValueType* ValuePtr = GetValuePtr<ValueType>(ContainerPtr, ArrayIndex);
            *ValuePtr = Value;
            return ValuePtr;
        }

        /** UB if ValueType doesn't match the property type. */
        template<typename ValueType>
        requires (!std::is_pointer_v<ValueType>)
        ValueType* GetValuePtr(void* ContainerPtr, int64 ArrayIndex = 0) const
        {
            return static_cast<ValueType*>(GetValuePtrInternal(ContainerPtr, ArrayIndex));
        }

        template<typename ValueType>
        requires (!std::is_pointer_v<ValueType>)
        const ValueType* GetValuePtr(const void* ContainerPtr, int64 ArrayIndex = 0) const
        {
            return static_cast<ValueType*>(GetValuePtrInternal(const_cast<void*>(ContainerPtr), ArrayIndex));
        }

        template<typename ValueType>
        void SetValue(void* InContainer, const ValueType& InValue, int64 ArrayIndex = 0) const
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
        void GetValue(void const* InContainer, ValueType* OutValue, int64 ArrayIndex = 0) const
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

        /** True when this property's value owns memory, i.e. it overrides the pair above. Lets a holder ask
         *  the property whether it needs lifecycle instead of testing a list of kinds. */
        virtual bool OwnsStorage() const { return false; }
        virtual void SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults = nullptr) { }

        /** Compact network serialization (no FName tag / size prefix). Defaults to the raw Serialize
         *  path; override per type to quantize (e.g. transforms/quats). Value points at the field. */
        RUNTIME_API virtual void NetSerialize(FNetArchive& Ar, void* Value);

        /** Defaults to byte-wise memcmp; non-trivial types (FString, structs, arrays) override. */
        RUNTIME_API virtual bool Identical(const void* ValueA, const void* ValueB) const;

        /** Defaults to memcpy on ElementSize; non-trivial types override. */
        RUNTIME_API virtual void CopyCompleteValue(void* Dst, const void* Src) const;

        RUNTIME_API bool Identical_InContainer(const void* ContainerA, const void* ContainerB, int64 ArrayIndex = 0) const;
        RUNTIME_API void CopyCompleteValue_InContainer(void* DstContainer, const void* SrcContainer, int64 ArrayIndex = 0) const;

        RUNTIME_API bool IsA(EPropertyTypeFlags Flag) const { return TypeFlags == Flag; }
        
        RUNTIME_API const FName& GetTypeName() const;
        
        NODISCARD bool IsReadOnly()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ReadOnly); }
        NODISCARD bool IsEditorOnly()   const       { return EnumHasAnyFlags(Flags, EPropertyFlags::EditorOnly); }
        NODISCARD bool IsReplicated()   const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Replicated); }
        NODISCARD bool IsEntityHandle() const       { return EnumHasAnyFlags(Flags, EPropertyFlags::EntityHandle); }
        NODISCARD bool IsDuplicateTransient() const { return EnumHasAnyFlags(Flags, EPropertyFlags::DuplicateTransient); }
        NODISCARD bool ShouldSerialize()const       { return !EnumHasAnyFlags(Flags, EPropertyFlags::NoSerialize); }
        NODISCARD bool IsEditable()     const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Editable); }
        NODISCARD bool IsConst()        const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Const); }
        NODISCARD bool IsInner()        const       { return EnumHasAnyFlags(Flags, EPropertyFlags::SubField); }
        NODISCARD bool IsProtected()    const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Protected); }
        NODISCARD bool IsPrivate()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Private); }
        NODISCARD bool IsScriptReadOnly() const     { return EnumHasAnyFlags(Flags, EPropertyFlags::ScriptReadOnly); }
        NODISCARD bool IsScriptWritable() const     { return EnumHasAnyFlags(Flags, EPropertyFlags::ScriptWritable); }
        NODISCARD bool IsScriptHidden() const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ScriptHidden); }
        NODISCARD bool IsVisible()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::ReadOnly | EPropertyFlags::Editable); }
        NODISCARD bool IsTrivial()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Trivial); }
        NODISCARD bool IsBuiltin()      const       { return EnumHasAnyFlags(Flags, EPropertyFlags::Builtin); }
        NODISCARD bool CanBeBulkSerialized() const  { return EnumHasAnyFlags(Flags, EPropertyFlags::BulkSerialize); }

        
        /** Empty when the key is absent. Null-terminated, so it hands straight to a C API. */
        RUNTIME_API FCStringView GetMetadata(FStringView Key) const;
        RUNTIME_API bool HasMetadata(FStringView Key) const;
        FCStringView TryGetMetadata(FStringView Key) const { return GetMetadata(Key); }

        /** The generated table outlives the binary, so this is stored rather than copied. */
        void SetMetadata(const FMetaDataPairParam* Entries, uint16 Count) { MetadataEntries = Entries; NumMetadata = Count; }

        /** Places the display name in Arena, so it costs a pointer rather than an FString and a malloc. */
        RUNTIME_API void OnMetadataFinalized(FPropertyArena& Arena);
        static FString MakeDisplayNameFromName(EPropertyTypeFlags TypeFlags, const FName& InName);

        RUNTIME_API virtual FString ToString(const void* Data) const { return "<unknown>"; }
        
        virtual bool HasSetter() const { return false; }

        virtual bool HasGetter() const { return false; }

        virtual bool HasSetterOrGetter() const { return false; }

        virtual void CallSetter(void* Container, const void* InValue) const;

        virtual void CallGetter(const void* Container, void* OutValue) const;

        
    private:

        RUNTIME_API void* GetValuePtrInternal(void* ContainerPtr, int64 ArrayIndex) const;
        
    public:

        FName               Name;

        uint32              Offset = 0;
        uint32              ElementSize = 0;
        EPropertyFlags      Flags = EPropertyFlags::None;
        EPropertyTypeFlags  TypeFlags = EPropertyTypeFlags::None;

        // Sits in the byte the flags above leave over rather than costing the property anything of its own.
        uint16              NumMetadata = 0;

        // Arena-owned, so a display name costs a pointer here instead of an FString and its allocation.
        const char*         DisplayName = nullptr;

        // Set when the property is attached; an inner is reached through the property that holds it instead.
        CStruct*            OwnerStruct = nullptr;

        // Points at the generated static table, or at one the arena built for a script-minted property.
        const FMetaDataPairParam* MetadataEntries = nullptr;
    };

    template <typename PropertyBaseClass>
    class TPropertyWithSetterAndGetter : public PropertyBaseClass
    {
    public:
        
        template <typename PropertyCodegenParams>
        explicit TPropertyWithSetterAndGetter(const PropertyCodegenParams* Prop)
            : PropertyBaseClass(Prop)
            , SetterFunc(Prop->SetterFunc)
            , GetterFunc(Prop->GetterFunc)
        {
        }

        virtual bool HasSetter() const override
        {
            return !!SetterFunc;
        }

        virtual bool HasGetter() const override
        {
            return !!GetterFunc;
        }

        virtual bool HasSetterOrGetter() const override
        {
            return !!SetterFunc || !!GetterFunc;
        }

        virtual void CallSetter(void* Container, const void* InValue) const override
        {
            if (SetterFunc == nullptr)
            {
                LOG_CRITICAL("Calling a setter but the property has no setter defined.");
                return;
            }
            SetterFunc(Container, InValue);
        }

        virtual void CallGetter(const void* Container, void* OutValue) const override
        {
            if (GetterFunc == nullptr)
            {
                LOG_CRITICAL("Calling a getter but the property has no getter defined.");
            }
            GetterFunc(Container, OutValue);
        }

    protected:

        SetterFuncPtr SetterFunc = nullptr;
        GetterFuncPtr GetterFunc = nullptr;
    };

    class FNumericProperty : public FProperty
    {
    public:

        explicit FNumericProperty(const FPropertyParams* Params)
            :FProperty(Params)
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
        
        explicit TProperty(const FPropertyParams* Params)
            :TBacking(Params)
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

        explicit TProperty_Numeric(const FPropertyParams* Params)
            :Super(Params)
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

        DECLARE_FPROPERTY(EPropertyTypeFlags::Bool)

        explicit FBoolProperty(const FPropertyParams* Params)
            : Super(Params)
        {}

        // Tight: a bool is one bit on the wire.
        RUNTIME_API void NetSerialize(FNetArchive& Ar, void* Value) override;
    };
    
    class FInt8Property : public TProperty_Numeric<int8>
    {
    public:
        using Super = TProperty_Numeric<int8>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Int8)

        explicit FInt8Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FInt16Property : public TProperty_Numeric<int16>
    {
    public:
        using Super = TProperty_Numeric<int16>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Int16)

        explicit FInt16Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FInt32Property : public TProperty_Numeric<int32>
    {
    public:
        using Super = TProperty_Numeric<int32>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Int32)

        explicit FInt32Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FInt64Property : public TProperty_Numeric<int64>
    {
    public:
        using Super = TProperty_Numeric<int64>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Int64)

        explicit FInt64Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FUInt8Property : public TProperty_Numeric<uint8>
    {
    public:
        using Super = TProperty_Numeric<uint8>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::UInt8)

        explicit FUInt8Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FUInt16Property : public TProperty_Numeric<uint16>
    {
    public:
        using Super = TProperty_Numeric<uint16>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::UInt16)

        explicit FUInt16Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FUInt32Property : public TProperty_Numeric<uint32>
    {
    public:
        using Super = TProperty_Numeric<uint32>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::UInt32)

        explicit FUInt32Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FUInt64Property : public TProperty_Numeric<uint64>
    {
    public:
        using Super = TProperty_Numeric<uint64>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::UInt64)

        explicit FUInt64Property(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FFloatProperty : public TProperty_Numeric<float>
    {
    public:
        using Super = TProperty_Numeric<float>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Float)

        explicit FFloatProperty(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    class FDoubleProperty : public TProperty_Numeric<double>
    {
    public:
        using Super = TProperty_Numeric<double>;
        DECLARE_FPROPERTY(EPropertyTypeFlags::Double)

        explicit FDoubleProperty(const FPropertyParams* Params)
            : Super(Params)
        {}
    };

    

}
