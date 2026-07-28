#include "pch.h"
#include "PropertyBag.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ConstructObjectParams.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Templates/Align.h"
#include "Memory/Memory.h"
#include "Core/Math/Math.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        FFieldOwner MakeOwner(CStruct* Owner)
        {
            FFieldOwner FieldOwner;
            FieldOwner.emplace<CStruct*>(Owner);
            return FieldOwner;
        }

        void FillBaseParams(FPropertyParams& Params, EPropertyTypeFlags TypeFlags, uint32 Offset, const char* NameCStr)
        {
            Params.Name          = NameCStr;
            Params.PropertyFlags = EPropertyFlags::Editable;
            Params.TypeFlags     = TypeFlags;
            Params.SetterFunc    = nullptr;
            Params.GetterFunc    = nullptr;
            Params.Offset        = (uint16)Offset;
        }

        // Numeric / bool / name / string share the base FProperty ctor (they read no
        // extra params), so one templated maker covers them all.
        template<typename TPropertyType, EPropertyTypeFlags TypeFlags>
        FProperty* MakeSimpleProperty(CStruct* Owner, const FName& Name, uint32 Offset)
        {
            const FString NameStr = Name.ToString();
            FPropertyParams Params{};
            FillBaseParams(Params, TypeFlags, Offset, NameStr.c_str());
            return Memory::New<TPropertyType>(MakeOwner(Owner), &Params);
        }

        FProperty* MakeObjectProperty(CStruct* Owner, const FName& Name, uint32 Offset)
        {
            const FString NameStr = Name.ToString();
            FObjectPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Object, Offset, NameStr.c_str());
            Params.ClassFunc     = +[]() -> CClass* { return CObject::StaticClass(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FObjectProperty>(MakeOwner(Owner), &Params);
        }

        // StructFunc/EnumFunc are plain fn pointers that can't carry the type; the ctor calls
        // them synchronously, so stage the type in a thread-local the trampoline reads.
        thread_local CStruct* GPendingStruct = nullptr;
        thread_local CEnum*   GPendingEnum   = nullptr;

        FProperty* MakeStructProperty(CStruct* Owner, const FName& Name, uint32 Offset, CStruct* Resolved)
        {
            GPendingStruct = Resolved;
            const FString NameStr = Name.ToString();
            FStructPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Struct, Offset, NameStr.c_str());
            Params.StructFunc    = +[]() -> CStruct* { return GPendingStruct; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FStructProperty>(MakeOwner(Owner), &Params);
            GPendingStruct = nullptr;
            return Property;
        }

        FProperty* MakeEnumProperty(CStruct* Owner, const FName& Name, uint32 Offset, CEnum* Resolved)
        {
            GPendingEnum = Resolved;
            const FString NameStr = Name.ToString();
            FEnumPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Enum, Offset, NameStr.c_str());
            Params.EnumFunc      = +[]() -> CEnum* { return GPendingEnum; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FEnumProperty* EnumProperty = Memory::New<FEnumProperty>(MakeOwner(Owner), &Params);
            GPendingEnum = nullptr;

            // FEnumProperty has no element size; store as int64 with a matching inner numeric
            // attached via FEnumProperty::AddProperty (not the layout chain).
            EnumProperty->SetElementSize(sizeof(int64));
            FPropertyParams InnerParams{};
            FillBaseParams(InnerParams, EPropertyTypeFlags::Int64, Offset, NameStr.c_str());
            FFieldOwner InnerOwner;
            InnerOwner.emplace<FField*>(EnumProperty);
            // Attaches to EnumProperty via FEnumProperty::AddProperty; ownership lives there.
            (void)Memory::New<FInt64Property>(InnerOwner, &InnerParams);
            return EnumProperty;
        }

        template<typename T>
        void ConstructAt(void* Mem) { new (Mem) T(); }

        template<typename T>
        void DestructAt(void* Mem) { static_cast<T*>(Mem)->~T(); }

        // Resolved field layout + lifetime; Construct/Destruct null for trivially-relocatable
        // types (buffer zeroed first), real for FString and heap-owning structs.
        using FValueLifecycleFn = void(*)(void*);

        struct FResolvedField
        {
            uint32            Size = 0;
            uint32            Alignment = 1;
            FValueLifecycleFn Construct = nullptr;
            FValueLifecycleFn Destruct  = nullptr;
            CStruct*          StructType = nullptr;   // Struct / Vector fields
            CEnum*            EnumType   = nullptr;   // Enum fields
        };

        // Fills Out; returns false when an Enum/Struct type can't be resolved (renamed/unloaded),
        // so the caller drops the field rather than guess a layout.
        bool ResolveField(EBagPropertyType Type, const FName& TypeName, FResolvedField& Out)
        {
            switch (Type)
            {
            case EBagPropertyType::Bool:    Out.Size = sizeof(bool);   Out.Alignment = alignof(bool);   return true;
            case EBagPropertyType::Int32:   Out.Size = sizeof(int32);  Out.Alignment = alignof(int32);  return true;
            case EBagPropertyType::Int64:   Out.Size = sizeof(int64);  Out.Alignment = alignof(int64);  return true;
            case EBagPropertyType::Float:   Out.Size = sizeof(float);  Out.Alignment = alignof(float);  return true;
            case EBagPropertyType::Double:  Out.Size = sizeof(double); Out.Alignment = alignof(double); return true;
            case EBagPropertyType::Object:
                // FObjectProperty's slot is a ref-counted TObjectPtr<CObject> (matches sizeof(void*)),
                // so it needs real construct/destruct or it leaks a strong ref on teardown.
                Out.Size      = sizeof(TObjectPtr<CObject>);
                Out.Alignment = alignof(TObjectPtr<CObject>);
                Out.Construct = &ConstructAt<TObjectPtr<CObject>>;
                Out.Destruct  = &DestructAt<TObjectPtr<CObject>>;
                return true;
            case EBagPropertyType::Name:    Out.Size = sizeof(FName);  Out.Alignment = alignof(FName);  Out.Construct = &ConstructAt<FName>; return true;
            case EBagPropertyType::String:  Out.Size = sizeof(FString);Out.Alignment = alignof(FString);Out.Construct = &ConstructAt<FString>; Out.Destruct = &DestructAt<FString>; return true;

            case EBagPropertyType::Vector2: Out.StructType = TBaseStructure<FVector2>::Get(); Out.Size = sizeof(FVector2); Out.Alignment = alignof(FVector2); break;
            case EBagPropertyType::Vector3: Out.StructType = TBaseStructure<FVector3>::Get(); Out.Size = sizeof(FVector3); Out.Alignment = alignof(FVector3); break;
            case EBagPropertyType::Vector4: Out.StructType = TBaseStructure<FVector4>::Get(); Out.Size = sizeof(FVector4); Out.Alignment = alignof(FVector4); break;

            case EBagPropertyType::Struct:
                Out.StructType = FindObject<CStruct>(TypeName);
                if (Out.StructType == nullptr) { return false; }
                Out.Size      = Out.StructType->GetAlignedSize();
                Out.Alignment = Out.StructType->GetAlignment();
                break;

            case EBagPropertyType::Enum:
                Out.EnumType = FindObject<CEnum>(TypeName);
                if (Out.EnumType == nullptr) { return false; }
                Out.Size      = sizeof(int64);
                Out.Alignment = alignof(int64);
                return true;

            default:
                return false;
            }

            // Struct-backed tail (Vector* / Struct): heap-owning members need real ctor/dtor.
            if (Out.StructType == nullptr)
            {
                return false;
            }
            if (FStructOps* Ops = Out.StructType->GetStructOps())
            {
                Out.Construct = Ops->HasConstruct() ? Ops->Construct : nullptr;
                Out.Destruct  = Ops->HasDestruct()  ? Ops->Destruct  : nullptr;
            }
            return true;
        }

        FProperty* MakeFieldProperty(EBagPropertyType Type, const FName& Name, const FResolvedField& Resolved, CStruct* Owner, uint32 Offset)
        {
            switch (Type)
            {
            case EBagPropertyType::Bool:    return MakeSimpleProperty<FBoolProperty,   EPropertyTypeFlags::Bool>  (Owner, Name, Offset);
            case EBagPropertyType::Int32:   return MakeSimpleProperty<FInt32Property,  EPropertyTypeFlags::Int32> (Owner, Name, Offset);
            case EBagPropertyType::Int64:   return MakeSimpleProperty<FInt64Property,  EPropertyTypeFlags::Int64> (Owner, Name, Offset);
            case EBagPropertyType::Float:   return MakeSimpleProperty<FFloatProperty,  EPropertyTypeFlags::Float> (Owner, Name, Offset);
            case EBagPropertyType::Double:  return MakeSimpleProperty<FDoubleProperty, EPropertyTypeFlags::Double>(Owner, Name, Offset);
            case EBagPropertyType::Name:    return MakeSimpleProperty<FNameProperty,   EPropertyTypeFlags::Name>  (Owner, Name, Offset);
            case EBagPropertyType::String:  return MakeSimpleProperty<FStringProperty, EPropertyTypeFlags::String>(Owner, Name, Offset);
            case EBagPropertyType::Object:  return MakeObjectProperty(Owner, Name, Offset);
            case EBagPropertyType::Vector2:
            case EBagPropertyType::Vector3:
            case EBagPropertyType::Vector4:
            case EBagPropertyType::Struct:  return MakeStructProperty(Owner, Name, Offset, Resolved.StructType);
            case EBagPropertyType::Enum:    return MakeEnumProperty(Owner, Name, Offset, Resolved.EnumType);
            }
            return nullptr;
        }
    }

    const char* BagPropertyTypeToString(EBagPropertyType Type)
    {
        switch (Type)
        {
        case EBagPropertyType::Bool:    return "Bool";
        case EBagPropertyType::Int32:   return "Int32";
        case EBagPropertyType::Int64:   return "Int64";
        case EBagPropertyType::Float:   return "Float";
        case EBagPropertyType::Double:  return "Double";
        case EBagPropertyType::Vector2: return "Vector2";
        case EBagPropertyType::Vector3: return "Vector3";
        case EBagPropertyType::Vector4: return "Vector4";
        case EBagPropertyType::Name:    return "Name";
        case EBagPropertyType::String:  return "String";
        case EBagPropertyType::Object:  return "Object";
        case EBagPropertyType::Enum:    return "Enum";
        case EBagPropertyType::Struct:  return "Struct";
        }
        return "Unknown";
    }

    FPropertyBag::FPropertyBag()
    {
        RebuildLayout({});
    }

    FPropertyBag::~FPropertyBag()
    {
        DestroyLayout(Layout, ValueBuffer, Fields);
        Layout = nullptr;
        ValueBuffer = nullptr;
    }

    bool FPropertyBag::HasProperty(const FName& Name) const
    {
        for (const FPropertyBagField& Field : Fields)
        {
            if (Field.Name == Name)
            {
                return true;
            }
        }
        return false;
    }

    EBagPropertyType FPropertyBag::GetPropertyType(const FName& Name, bool* bOutFound) const
    {
        for (const FPropertyBagField& Field : Fields)
        {
            if (Field.Name == Name)
            {
                if (bOutFound) { *bOutFound = true; }
                return Field.Type;
            }
        }
        if (bOutFound) { *bOutFound = false; }
        return EBagPropertyType::Float;
    }

    FProperty* FPropertyBag::FindProperty(const FName& Name) const
    {
        return Layout ? Layout->GetProperty(Name) : nullptr;
    }

    bool FPropertyBag::AddProperty(const FName& Name, EBagPropertyType Type, const FName& TypeName)
    {
        if (Name.IsNone() || HasProperty(Name))
        {
            return false;
        }

        TVector<FPropertyBagField> NewFields = Fields;
        NewFields.push_back(FPropertyBagField{ Name, Type, TypeName });
        RebuildLayout(NewFields);

        // Reject the add if the type couldn't be resolved (RebuildLayout drops such fields).
        return HasProperty(Name);
    }

    bool FPropertyBag::RemoveProperty(const FName& Name)
    {
        TVector<FPropertyBagField> NewFields;
        NewFields.reserve(Fields.size());
        bool bRemoved = false;
        for (const FPropertyBagField& Field : Fields)
        {
            if (Field.Name == Name)
            {
                bRemoved = true;
                continue;
            }
            NewFields.push_back(Field);
        }

        if (!bRemoved)
        {
            return false;
        }

        RebuildLayout(NewFields);
        return true;
    }

    bool FPropertyBag::RenameProperty(const FName& OldName, const FName& NewName)
    {
        if (NewName.IsNone() || HasProperty(NewName) || !HasProperty(OldName))
        {
            return false;
        }

        // Migration is keyed by name, so the renamed field's value is intentionally
        // reset rather than carried across (it is now a different key).
        TVector<FPropertyBagField> NewFields = Fields;
        for (FPropertyBagField& Field : NewFields)
        {
            if (Field.Name == OldName)
            {
                Field.Name = NewName;
                break;
            }
        }
        RebuildLayout(NewFields);
        return true;
    }

    bool FPropertyBag::ChangePropertyType(const FName& Name, EBagPropertyType NewType, const FName& TypeName)
    {
        bool bChanged = false;
        TVector<FPropertyBagField> NewFields = Fields;
        for (FPropertyBagField& Field : NewFields)
        {
            if (Field.Name == Name)
            {
                if (Field.Type == NewType && Field.TypeName == TypeName)
                {
                    return false;
                }
                Field.Type = NewType;
                Field.TypeName = TypeName;
                bChanged = true;
                break;
            }
        }

        if (!bChanged)
        {
            return false;
        }

        // The field's value does not migrate (type differs) -- it resets to default.
        RebuildLayout(NewFields);
        return HasProperty(Name);
    }

    void FPropertyBag::Reset()
    {
        RebuildLayout({});
    }

    void FPropertyBag::SetSchema(const TVector<FPropertyBagField>& NewFields, const FPropertyBag* Defaults)
    {
        RebuildLayout(NewFields, Defaults);
    }

    void FPropertyBag::RebuildLayout(const TVector<FPropertyBagField>& NewFields, const FPropertyBag* Defaults)
    {
        // Snapshot the current state so values can be migrated by name, then freed.
        CStruct*           OldLayout = Layout;
        uint8*             OldBuffer = ValueBuffer;
        TVector<FPropertyBagField> OldFields = Fields;

        // Resolve every field up front; drop any whose Enum/Struct type can't be resolved so
        // Fields, the layout chain, and the buffer always agree.
        TVector<FResolvedField> ResolvedInfos;
        Fields.clear();
        Fields.reserve(NewFields.size());
        ResolvedInfos.reserve(NewFields.size());
        for (const FPropertyBagField& Field : NewFields)
        {
            FResolvedField Info;
            if (ResolveField(Field.Type, Field.TypeName, Info))
            {
                Fields.push_back(Field);
                ResolvedInfos.push_back(Info);
            }
            else
            {
                LOG_WARN("Property bag field '{}' dropped: type '{}' could not be resolved.",
                    Field.Name.ToString(), Field.TypeName.ToString());
            }
        }

        // Mint a fresh transient CStruct to own the new FProperty chain. The name is
        // uniqued per layout so transient-package bookkeeping never sees a collision.
        static TAtomic<uint64> LayoutSerial{ 0 };
        FString LayoutName = "DataAssetLayout_";
        LayoutName += eastl::to_string(LayoutSerial.fetch_add(1)).c_str();

        FConstructCObjectParams ObjectParams(CStruct::StaticClass());
        ObjectParams.Name    = FName(LayoutName);
        ObjectParams.Flags   = OF_Transient;
        ObjectParams.Package = CPackage::GetTransientPackage();
        ObjectParams.Guid    = FGuid::New();

        Layout = static_cast<CStruct*>(StaticAllocateObject(ObjectParams));
        CObjectForceRegistration(Layout);
        // Strong-ref rather than root: RemoveFromRoot locks the root mutex and would deadlock
        // if torn down inside the GC drain; ReleaseStrongRef takes no such lock.
        GObjectArray.AddStrongRef(Layout);

        // Lay the fields out contiguously, respecting each type's alignment. Creating an
        // FProperty auto-chains it onto Layout via FProperty::Init().
        uint32 RunningSize = 0;
        uint32 MaxAlign    = 1;
        for (int32 i = 0; i < (int32)Fields.size(); ++i)
        {
            const FResolvedField& Info = ResolvedInfos[i];
            const uint32 FieldOffset = Align(RunningSize, Info.Alignment);
            if (FProperty* Property = MakeFieldProperty(Fields[i].Type, Fields[i].Name, Info, Layout, FieldOffset))
            {
                // We bypass the codegen metadata path, so derive the display name (shown as
                // the property's label in the editor) from the field name ourselves.
                Property->OnMetadataFinalized();
            }
            RunningSize = FieldOffset + Info.Size;
            MaxAlign    = Math::Max(MaxAlign, Info.Alignment);
        }

        BufferSize        = (RunningSize > 0) ? Align(RunningSize, MaxAlign) : 0;
        BufferAlignment   = MaxAlign;
        Layout->Size      = BufferSize;
        Layout->Alignment = MaxAlign;
        Layout->Link();

        // Allocate + default-initialize the value buffer. Zeroing covers every trivially
        // relocatable type; FString / heap-owning structs get a real placement-new on top.
        ValueBuffer = (BufferSize > 0) ? static_cast<uint8*>(Memory::Malloc(BufferSize, MaxAlign)) : nullptr;
        if (ValueBuffer != nullptr)
        {
            Memory::Memzero(ValueBuffer, BufferSize);
            for (int32 i = 0; i < (int32)Fields.size(); ++i)
            {
                if (ResolvedInfos[i].Construct != nullptr)
                {
                    if (FProperty* Property = Layout->GetProperty(Fields[i].Name))
                    {
                        ResolvedInfos[i].Construct(ValueBuffer + Property->Offset);
                    }
                }
            }
        }

        // Per field: keep this bag's existing value if the field is unchanged; otherwise, for
        // a genuinely new field, seed it from the schema's default (when one is supplied).
        for (const FPropertyBagField& Field : Fields)
        {
            FProperty* NewProp = (ValueBuffer != nullptr) ? Layout->GetProperty(Field.Name) : nullptr;
            if (NewProp == nullptr)
            {
                continue;
            }

            // 1. Migrate from the prior layout when the field is unchanged (same type).
            const FPropertyBagField* Old = nullptr;
            for (const FPropertyBagField& Candidate : OldFields)
            {
                if (Candidate.Name == Field.Name) { Old = &Candidate; break; }
            }
            if (Old != nullptr && Old->Type == Field.Type && Old->TypeName == Field.TypeName
                && OldLayout != nullptr && OldBuffer != nullptr)
            {
                if (FProperty* OldProp = OldLayout->GetProperty(Field.Name))
                {
                    NewProp->CopyCompleteValue(ValueBuffer + NewProp->Offset, OldBuffer + OldProp->Offset);
                    continue;
                }
            }

            // 2. New (or retyped) field: seed from the schema default with a matching type.
            if (Defaults != nullptr && Defaults->Layout != nullptr && Defaults->ValueBuffer != nullptr)
            {
                if (FProperty* DefProp = Defaults->Layout->GetProperty(Field.Name))
                {
                    if (DefProp->GetTypeName() == NewProp->GetTypeName())
                    {
                        NewProp->CopyCompleteValue(ValueBuffer + NewProp->Offset, Defaults->ValueBuffer + DefProp->Offset);
                    }
                }
            }
        }

        DestroyLayout(OldLayout, OldBuffer, OldFields);
    }

    void FPropertyBag::DestroyLayout(CStruct* InLayout, uint8* Buffer, const TVector<FPropertyBagField>& BufferFields)
    {
        if (Buffer != nullptr)
        {
            for (const FPropertyBagField& Field : BufferFields)
            {
                FResolvedField Info;
                if (InLayout != nullptr && ResolveField(Field.Type, Field.TypeName, Info) && Info.Destruct != nullptr)
                {
                    if (FProperty* Property = InLayout->GetProperty(Field.Name))
                    {
                        Info.Destruct(Buffer + Property->Offset);
                    }
                }
            }
            Memory::Free((void*&)Buffer);
        }

        if (InLayout != nullptr)
        {
            // Free the FProperty objects we minted (the CStruct never owns them).
            FProperty* Property = InLayout->LinkedProperty;
            while (Property != nullptr)
            {
                FProperty* Next = static_cast<FProperty*>(Property->Next);
                Memory::Delete(Property);
                Property = Next;
            }
            InLayout->LinkedProperty = nullptr;

            // Drop our strong ref; the object system frees the CStruct when it's unreferenced
            // (no root mutex, so this is safe even mid-GC-drain).
            GObjectArray.ReleaseStrongRef(InLayout);
        }
    }

    void FPropertyBag::Serialize(FArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            uint32 NumFields = (uint32)Fields.size();
            Ar << NumFields;
            for (FPropertyBagField& Field : Fields)
            {
                uint8 TypeByte = (uint8)Field.Type;
                Ar << Field.Name;
                Ar << TypeByte;
                Ar << Field.TypeName;
            }

            if (Layout != nullptr)
            {
                Layout->SerializeTaggedProperties(Ar, ValueBuffer);
            }
        }
        else if (Ar.IsReading())
        {
            uint32 NumFields = 0;
            Ar << NumFields;

            if (NumFields > Ar.GetMaxSerializeSize())
            {
                LOG_ERROR("Property bag corrupted: claims {} fields (max {}).", NumFields, Ar.GetMaxSerializeSize());
                Ar.SetHasError(true);
                return;
            }

            TVector<FPropertyBagField> LoadedFields;
            LoadedFields.reserve(NumFields);
            for (uint32 i = 0; i < NumFields; ++i)
            {
                FPropertyBagField Field;
                uint8 TypeByte = 0;
                Ar << Field.Name;
                Ar << TypeByte;
                Ar << Field.TypeName;
                Field.Type = (EBagPropertyType)TypeByte;
                LoadedFields.push_back(Field);
            }

            RebuildLayout(LoadedFields);

            if (Layout != nullptr)
            {
                Layout->SerializeTaggedProperties(Ar, ValueBuffer);
            }
        }
    }

    void FPropertyBag::CopyFrom(const FPropertyBag& Other)
    {
        RebuildLayout(Other.Fields);

        if (Layout == nullptr || Other.Layout == nullptr || ValueBuffer == nullptr || Other.ValueBuffer == nullptr)
        {
            return;
        }

        for (const FPropertyBagField& Field : Fields)
        {
            FProperty* DstProp = Layout->GetProperty(Field.Name);
            FProperty* SrcProp = Other.Layout->GetProperty(Field.Name);
            if (DstProp != nullptr && SrcProp != nullptr)
            {
                DstProp->CopyCompleteValue(ValueBuffer + DstProp->Offset, Other.ValueBuffer + SrcProp->Offset);
            }
        }
    }

    void FPropertyBag::ConstructValueInto(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst)
    {
        if (Layout == nullptr || Dst == nullptr)
        {
            return;
        }

        // Zero covers every trivially relocatable field (and gives FString/TObjectPtr a valid
        // null state); non-trivial fields then get a real placement-new on top.
        Memory::Memzero(Dst, Layout->GetSize());
        for (const FPropertyBagField& Field : Fields)
        {
            FResolvedField Info;
            if (ResolveField(Field.Type, Field.TypeName, Info) && Info.Construct != nullptr)
            {
                if (FProperty* Property = Layout->GetProperty(Field.Name))
                {
                    Info.Construct(static_cast<uint8*>(Dst) + Property->Offset);
                }
            }
        }
    }

    void FPropertyBag::DestructValueIn(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst)
    {
        if (Layout == nullptr || Dst == nullptr)
        {
            return;
        }

        for (const FPropertyBagField& Field : Fields)
        {
            FResolvedField Info;
            if (ResolveField(Field.Type, Field.TypeName, Info) && Info.Destruct != nullptr)
            {
                if (FProperty* Property = Layout->GetProperty(Field.Name))
                {
                    Info.Destruct(static_cast<uint8*>(Dst) + Property->Offset);
                }
            }
        }
    }

    void FPropertyBag::CopyValueInto(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst, const void* Src)
    {
        if (Layout == nullptr || Dst == nullptr || Src == nullptr)
        {
            return;
        }

        for (const FPropertyBagField& Field : Fields)
        {
            if (FProperty* Property = Layout->GetProperty(Field.Name))
            {
                Property->CopyCompleteValue(
                    static_cast<uint8*>(Dst) + Property->Offset,
                    static_cast<const uint8*>(Src) + Property->Offset);
            }
        }
    }

    void FPropertyBag::CopyFieldValue(EBagPropertyType Type, FProperty* Property, void* Dst, const void* Src)
    {
        // Type is now redundant: FObjectProperty::CopyCompleteValue assigns through TObjectPtr.
        (void)Type;
        if (Property != nullptr && Dst != nullptr && Src != nullptr)
        {
            Property->CopyCompleteValue(Dst, Src);
        }
    }
}
