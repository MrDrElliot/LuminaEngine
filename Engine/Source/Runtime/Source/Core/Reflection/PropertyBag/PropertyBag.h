#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/LuminaMacros.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
    class FArchive;

    enum class EBagPropertyType : uint8
    {
        Bool,
        Int32,
        Int64,
        Float,
        Double,
        Vector2,
        Vector3,
        Vector4,
        Name,
        String,
        Object,

        Enum,    // TypeName = reflected CEnum name
        Struct,  // TypeName = reflected CStruct name
    };

    RUNTIME_API const char* BagPropertyTypeToString(EBagPropertyType Type);

    FORCEINLINE bool BagPropertyTypeNeedsTypeName(EBagPropertyType Type)
    {
        return Type == EBagPropertyType::Enum || Type == EBagPropertyType::Struct;
    }

    struct FPropertyBagField
    {
        FName            Name;
        EBagPropertyType Type = EBagPropertyType::Float;
        FName            TypeName;
    };

    class FPropertyBag
    {
    public:

        RUNTIME_API FPropertyBag();
        RUNTIME_API ~FPropertyBag();

        LE_NO_COPYMOVE(FPropertyBag);

        RUNTIME_API bool AddProperty(const FName& Name, EBagPropertyType Type, const FName& TypeName = FName());
        RUNTIME_API bool RemoveProperty(const FName& Name);
        RUNTIME_API bool RenameProperty(const FName& OldName, const FName& NewName);
        RUNTIME_API bool ChangePropertyType(const FName& Name, EBagPropertyType NewType, const FName& TypeName = FName());
        RUNTIME_API void Reset();

        RUNTIME_API int32 GetNumProperties() const { return (int32)Fields.size(); }
        RUNTIME_API bool HasProperty(const FName& Name) const;
        RUNTIME_API EBagPropertyType GetPropertyType(const FName& Name, bool* bOutFound = nullptr) const;
        RUNTIME_API const FName& GetPropertyNameAt(int32 Index) const { return Fields[Index].Name; }
        RUNTIME_API EBagPropertyType GetPropertyTypeAt(int32 Index) const { return Fields[Index].Type; }
        RUNTIME_API const FName& GetPropertyTypeNameAt(int32 Index) const { return Fields[Index].TypeName; }

        RUNTIME_API const TVector<FPropertyBagField>& GetSchema() const { return Fields; }
        RUNTIME_API void SetSchema(const TVector<FPropertyBagField>& NewFields, const FPropertyBag* Defaults = nullptr);

        RUNTIME_API CStruct* GetLayout() const { return Layout; }
        RUNTIME_API void* GetValueData() const { return ValueBuffer; }
        RUNTIME_API FProperty* FindProperty(const FName& Name) const;

        RUNTIME_API uint32 GetBufferSize() const { return BufferSize; }
        RUNTIME_API uint32 GetBufferAlignment() const { return BufferAlignment; }

        // Element lifecycle for external buffers (runtime component storage packed instances).
        RUNTIME_API static void ConstructValueInto(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst);
        RUNTIME_API static void DestructValueIn(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst);
        RUNTIME_API static void CopyValueInto(CStruct* Layout, const TVector<FPropertyBagField>& Fields, void* Dst, const void* Src);
        // Field copy with correct ref-count semantics; Dst/Src point at the field itself.
        RUNTIME_API static void CopyFieldValue(EBagPropertyType Type, FProperty* Property, void* Dst, const void* Src);

        template<typename T>
        requires (!eastl::is_pointer_v<T>)
        bool GetValue(const FName& Name, T& Out) const
        {
            FProperty* Property = FindProperty(Name);
            if (Property == nullptr || ValueBuffer == nullptr)
            {
                return false;
            }
            Out = *Property->GetValuePtr<T>(ValueBuffer);
            return true;
        }

        template<typename T>
        requires (!eastl::is_pointer_v<T>)
        bool SetValue(const FName& Name, const T& In)
        {
            FProperty* Property = FindProperty(Name);
            if (Property == nullptr || ValueBuffer == nullptr)
            {
                return false;
            }
            Property->SetValue<T>(ValueBuffer, In);
            return true;
        }

        RUNTIME_API void Serialize(FArchive& Ar);
        RUNTIME_API void CopyFrom(const FPropertyBag& Other);

    private:

        void RebuildLayout(const TVector<FPropertyBagField>& NewFields, const FPropertyBag* Defaults = nullptr);
        static void DestroyLayout(CStruct* InLayout, uint8* Buffer, const TVector<FPropertyBagField>& BufferFields);

        TVector<FPropertyBagField> Fields;
        CStruct*                   Layout = nullptr;
        uint8*                     ValueBuffer = nullptr;
        uint32                     BufferSize = 0;
        uint32                     BufferAlignment = 1;
    };
}
