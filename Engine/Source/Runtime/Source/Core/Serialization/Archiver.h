#pragma once

#include <bit>

#include "Containers/HashTable.h"
#include "Containers/Function.h"
#include "Memory/SmartPtr.h"
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandle.h"
#include "Core/Serialization/BulkDataRef.h"
#include "Core/Templates/CanBulkSerialize.h"
#include "Core/Templates/IsSigned.h"
#include "Core/Versioning/CoreVersion.h"
#include "Core/Math/Math.h"

#include "Log/Log.h"
#include "Types/BitFlags.h"

namespace Lumina
{
    class FField;
    class FGuid;
    class FPortableBytes;
}

enum class EArchiverFlags : uint8
{
    None        = 0,
    Reading     = 1,
    Writing     = 2,
    Compress    = 3,
    Encrypt     = 4,
    NoFields    = 5,
    Cooking     = 6,

    /** The bulk region is being carried over verbatim from the file the package was loaded from, so an
     *  export must KEEP the FBulkDataRef it already has instead of re-emitting its payload. Region-relative
     *  offsets are what make this work: copy the whole region and every ref into it is still correct. */
    BulkPassthrough = 7,

    // Writing or reading a save game, where an object reference may carry the object itself.
    SaveGame    = 8,
};

namespace Lumina
{
    class CObject;

    class RUNTIME_API FArchive
    {
    public:
    
        FArchive() = default;
        FArchive(const FArchive&) = delete;
        FArchive(FArchive&&) = default;
        FArchive& operator=(const FArchive& ArchiveToCopy) = delete;
        FArchive& operator=(FArchive&&) = default;
        virtual ~FArchive() = default;

    
        virtual void Seek(int64 InPos) { }
        virtual int64 Tell() { return 0; }
        virtual int64 TotalSize() { return 0; }

        virtual void Serialize(void* V, int64 Length) {}

        FORCEINLINE void SetFlag(EArchiverFlags Flag) { Flags.SetFlag(Flag); }
        FORCEINLINE void RemoveFlag(EArchiverFlags Flag) { Flags.ClearFlag(Flag); }
        FORCEINLINE bool HasFlag(EArchiverFlags Flag) const { return Flags.IsFlagSet(Flag); }

        FORCEINLINE bool virtual IsWriting() const { return HasFlag(EArchiverFlags::Writing); }

        /** Reading from a buffer (i.e. deserializing into a class). */
        FORCEINLINE bool virtual IsReading() const { return HasFlag(EArchiverFlags::Reading); }

        /** True when producing cooked shipping output. Property serializers
         *  drop EditorOnly properties when this is set. */
        FORCEINLINE bool IsCooking() const { return HasFlag(EArchiverFlags::Cooking); }

        FORCEINLINE bool IsSaveGame() const { return HasFlag(EArchiverFlags::SaveGame); }

        // A packed ECS entity id, which an archive between two worlds may translate.
        virtual void SerializeEntityId(uint32& PackedEntity) { *this << PackedEntity; }

        // Virtual so a proxy can hand the error to the archive that actually reports it.
        virtual void SetHasError(bool bIsError) { bHasError = bIsError; }
        FORCEINLINE virtual bool HasError() const { return bHasError; }

        FORCEINLINE static FPackageFileVersion GetEngineVersion()
        {
            return GPackageFileLuminaVersion;
        }

        /** File version being read/written; LoadPackage sets it from the header so per-type
         *  Serialize can branch on source version to migrate older payloads. */
        FORCEINLINE int32 GetFileVersion() const { return FileVersion; }
        FORCEINLINE void SetFileVersion(int32 InVersion) { FileVersion = InVersion; }

        // Records what the name slots and object indices in bytes just read from here stand for, false when this archive cannot say.
        virtual bool DescribeCutBytes(FPortableBytes& Cut) const { return false; }

        FORCEINLINE size_t GetMaxSerializeSize() const { return ArMaxSerializeSize; }

        // A count past what is left to read is a corrupt length caught before it allocates, measured in bits since net archives pack bools.
        bool CanHoldCount(uint64 Count, uint64 MinBytes = 1)
        {
            const int64 Total = TotalSize();
            if (!IsReading() || Total <= 0)
            {
                return true;
            }
            const int64 Left = Total - Tell();
            return Left >= 0 && Count <= (uint64)Left * 8 / (MinBytes > 0 ? MinBytes : 1);
        }

        
        virtual FArchive& operator<<(CObject*& Value)
        {
            LOG_ERROR("Serializing CObjects is not supported by this archive.");
            return *this;
        }

        virtual FArchive& operator<<(FObjectHandle& Value)
        {
            LOG_ERROR("Serializing CObjects is not supported by this archive.");
            return *this;
        }

        virtual FArchive& operator<<(FField*& Value)
        {
            LOG_ERROR("Serializing FFields is not supported by this archive.");
            return *this;
        }

        /** Soft-ref hook: FSoftObjectPath::operator<< calls this on write so the saver records
         *  a Soft ImportTable entry. No-op on archives without an import table. */
        virtual void RegisterSoftAssetReference(const FGuid& AssetGUID) {}

        // The bundles a property names for the soft references inside it, returning the ones it replaces so nesting restores them.
        virtual FStringView SetSoftReferenceBundles(FStringView Bundles) { return {}; }

        // Offered its resolved path+GUID on write so a fixup archive can retarget a soft ref in place.
        virtual bool RewriteSoftAssetReference(FString& Path, FGuid& AssetGUID) { return false; }

        /** True when WriteBulkData will actually do something. Only the package saver has somewhere to put a
         *  bulk region; every other archive (duplication, transient, network) must keep payloads inline, so
         *  a serializer that can split has to branch on this rather than assume. */
        virtual bool SupportsBulkData() const { return false; }

        /** True when the bulk region is being copied over verbatim rather than rebuilt. A serializer must
         *  then leave its FBulkDataRef alone AND leave whatever describes the inline/bulk split alone: the
         *  region it is referring into is the old one, byte for byte, so re-deriving either would describe
         *  a layout the copied bytes do not have. WriteBulkData succeeds without copying anything. */
        bool IsBulkPassthrough() const { return HasFlag(EArchiverFlags::BulkPassthrough); }

        /** Append Size bytes to the archive's bulk region and fill OutRef with where they landed. Returns
         *  false (leaving OutRef untouched) on archives with no region -- write the payload inline instead. */
        virtual bool WriteBulkData(FBulkDataRef& OutRef, const void* Data, int64 Size) { return false; }

        // A payload the serializer cannot produce, so the bytes being built have it empty and must not commit.
        virtual void FlagUnresolvedBulkData() {}

        virtual FArchive& operator<<(uint8& Value)
        {
            Serialize(&Value, 1);
            return *this;
        }

        virtual FArchive& operator<<(int8& Value)
        {
            Serialize(&Value, 1);
            return *this;
        }
    
        virtual FArchive& operator<<(uint16& Value)
        {
            ByteOrderSerialize(Value);
            return *this;
        }
    
        virtual FArchive& operator<<(int16& Value)
        {
            uint16 Bits = std::bit_cast<uint16>(Value);
            ByteOrderSerialize(Bits);
            Value = std::bit_cast<int16>(Bits);
            return *this;
        }
    
        virtual FArchive& operator<<(uint32& Value)
        {
            ByteOrderSerialize(Value);
            return *this;
        }
    
        virtual FArchive& operator<<(int32& Value)
        {
            uint32 Bits = std::bit_cast<uint32>(Value);
            ByteOrderSerialize(Bits);
            Value = std::bit_cast<int32>(Bits);
            return *this;
        }

        virtual FArchive& operator<<( bool& D)
        {
            SerializeBool(D);
            return *this;
        }

        virtual FArchive& operator<<(float& Value)
        {
            static_assert(sizeof(float) == sizeof(uint32), "Unexpected float size");
            uint32 Temp = std::bit_cast<uint32>(Value);
            ByteOrderSerialize(Temp);
            Value = std::bit_cast<float>(Temp);
            return *this;
        }

        virtual FArchive& operator<<(double& Value)
        {
            static_assert(sizeof(double) == sizeof(uint64), "Unexpected double size");
            uint64 Temp = std::bit_cast<uint64>(Value);
            ByteOrderSerialize(Temp);
            Value = std::bit_cast<double>(Temp);
            return *this;
        }
        
        virtual FArchive& operator<<(uint64& Value)
        {
            ByteOrderSerialize(Value);
            return *this;
        }

        virtual FArchive& operator<<(int64& Value)
        {
            uint64 Bits = std::bit_cast<uint64>(Value);
            ByteOrderSerialize(Bits);
            Value = std::bit_cast<int64>(Bits);
            return *this;
        }

        virtual void SerializeBool(bool& D);
        
        virtual FArchive& operator<<(FString& Str)
        {
            if (IsReading())
            {
                uint64 SaveNum = 0;
                *this << SaveNum;
            
                if (SaveNum > GetMaxSerializeSize() || !CanHoldCount(SaveNum))
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, string is too large! (Size: {0}, Max: {1})", SaveNum, GetMaxSerializeSize());
                    return *this;
                }
            
                if (SaveNum)
                {
                    Str.Reset();
                    Str.Shrink();
                    Str.SetLen(IntCastChecked<int32>(SaveNum));
                    Serialize(Str.GetData(), SaveNum);
                }
                else
                {
                    Str.Reset();
                }
            }
            else
            {
                uint64 SaveNum = static_cast<uint64>(Str.Len());
                *this << SaveNum;

                if (SaveNum)
                {
                    Serialize(Str.GetData(), SaveNum);
                }
            }
        
            return *this;
        }
        
        virtual FArchive& operator<<(FFixedString& Str)
        {
            if (IsReading())
            {
                uint64 SaveNum = 0;
                *this << SaveNum;
            
                if (SaveNum > GetMaxSerializeSize() || !CanHoldCount(SaveNum))
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, string is too large! (Size: {0}, Max: {1})", SaveNum, GetMaxSerializeSize());
                    return *this;
                }
            
                if (SaveNum)
                {
                    Str.Reset();
                    Str.Shrink();
                    Str.SetLen(IntCastChecked<int32>(SaveNum));
                    Serialize(Str.GetData(), SaveNum);
                }
                else
                {
                    Str.Reset();
                }
            }
            else
            {
                uint64 SaveNum = static_cast<uint64>(Str.Len());
                *this << SaveNum;

                if (SaveNum)
                {
                    Serialize(Str.GetData(), SaveNum);
                }
            }
        
            return *this;
        }

        virtual FArchive& operator<<(FName& Name)
        {
            if (IsReading())
            {
                FString LoadedString;
                *this << LoadedString;

                // Files older than FNAME_NONE_EMPTY_STRING stored None as its "NAME_None" display
                // rendering; decode it back so those names don't load as real non-None names.
                if (FileVersion < (int32)ELuminaEngineVersion::FNAME_NONE_EMPTY_STRING && LoadedString == "NAME_None")
                {
                    Name = FName();
                }
                else
                {
                    Name = FName(LoadedString);
                }
            }
            else
            {
                // None's wire form is the empty string; operator* renders "NAME_None", which is a
                // display string and must not round-trip into a real name.
                FString SavedString(Name.IsNone() ? "" : *Name);
                *this << SavedString;
            }

            return *this;
        }

        template<typename ValueType>
        FArchive& operator << (TVector<ValueType>& Array)
        {
            // A stream that already failed reports garbage sizes forever, so the first report is the one.
            if (IsReading() && HasError())
            {
                Array.Reset();
                return *this;
            }

            uint64 SerializeNum = IsReading() ? 0 : Array.Num();
            *this << SerializeNum;
        
            if (SerializeNum == 0)
            {
                if (IsReading())
                {
                    Array.Reset();
                    Array.Shrink();
                }
            
                return *this;
            }
        
            if (SerializeNum > GetMaxSerializeSize() || !CanHoldCount(SerializeNum))
            {
                SetHasError(true);
                LOG_ERROR("Archiver is corrupted, attempted to serialize {} array elements. Max is: {}", SerializeNum, GetMaxSerializeSize());
                return *this;
            }
            
            if constexpr (sizeof(ValueType) == 1 || TCanBulkSerialize<ValueType>::value)
            {
                if (IsReading())
                {
                    Array.Reset();
                    Array.Shrink();
                    Array.SetNum(IntCastChecked<int32>(SerializeNum));
                }
            
                Serialize(Array.GetData(), SerializeNum * sizeof(ValueType));
            }
            else
            {
                if (IsReading())
                {
                    Array.Reset();
                    Array.Shrink();
                    Array.SetNum(IntCastChecked<int32>(SerializeNum));
                }

                for (int32 i = 0, Limit = IntCastChecked<int32>(SerializeNum); i < Limit && !HasError(); i++)
                {
                    *this << Array[i];
                }
            }

            return *this;
        }
        
        template<typename K, typename V>
        FArchive& operator << (TPair<K, V>& Pair)
        {
            *this << Pair.Key;
            *this << Pair.Value;
            
            return *this;
        }
        
        template<typename K, typename V>
        FArchive& operator<<(THashMap<K, V>& Map)
        {
            uint32 Count = 0;

            if (IsWriting())
            {
                Count = static_cast<uint32>(Map.Num());
                *this << Count;

                for (TPair<K, V> Pair : Map)
                {
                    *this << Pair;
                }
            }
            else
            {
                *this << Count;

                if (HasError() || Count > GetMaxSerializeSize() || !CanHoldCount(Count, 8))
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, attempted to serialize {} map entries. Max is: {}", Count, GetMaxSerializeSize());
                    return *this;
                }

                Map.Reset();
                Map.Reserve(Count);

                for (uint32 i = 0; i < Count && !HasError(); ++i)
                {
                    TPair<K, V> Pair;
                    *this << Pair;
                    Map.TryAdd(Pair.Key, std::move(Pair.Value));
                }
            }

            return *this;
        }
        
        template<typename EnumType>
        FORCEINLINE FArchive& operator<<(EnumType& Value)
        requires (std::is_enum_v<EnumType>)
        {
            using Underlying = std::underlying_type_t<EnumType>;
            Underlying Bits = std::bit_cast<Underlying>(Value);
            FArchive& Archive = *this << Bits;
            Value = std::bit_cast<EnumType>(Bits);
            return Archive;
        }

    private:
    
        template<typename T>
        requires(!TIsSigned<T>::Value)
        FArchive& ByteOrderSerialize(T& Value)
        {
            Serialize(&Value, sizeof(T));
            return *this;
        }
    
    private:

        size_t                      ArMaxSerializeSize = INT32_MAX;

        int32                       FileVersion = GPackageFileLuminaVersion.FileVersion;

        TBitFlags<EArchiverFlags>   Flags;
        uint8                       bHasError:1 = false;

    };

    inline FArchive& operator<<(FArchive& Ar, FBulkDataRef& Data)
    {
        Ar << Data.Offset;
        Ar << Data.Size;
        return Ar;
    }

    inline void FArchive::SerializeBool(bool& D)
    {
        uint8 BoolValue = D ? 1 : 0;
        Serialize(&BoolValue, sizeof(BoolValue));

        if (BoolValue > 1)
        {
            LOG_ERROR("Invalid boolean encountered while reading archive - stream is most likely corrupted.");
            SetHasError(true);
        }
        D = !!BoolValue;
    }

    inline FArchive& operator<<(FArchive& Ar, FVector2& v)
    {
        Ar << v.x << v.y;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FVector3& v)
    {
        Ar << v.x << v.y << v.z;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FVector4& v)
    {
        Ar << v.x << v.y << v.z << v.w;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FIntVector2& v)
    {
        Ar << v.x << v.y;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FIntVector3& v)
    {
        Ar << v.x << v.y << v.z;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FIntVector4& v)
    {
        Ar << v.x << v.y << v.z << v.w;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FUIntVector2& v)
    {
        Ar << v.x << v.y;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FUIntVector3& v)
    {
        Ar << v.x << v.y << v.z;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FUIntVector4& v)
    {
        Ar << v.x << v.y << v.z << v.w;
        return Ar;
    }
    
    inline FArchive& operator<<(FArchive& Ar, FU8Vector2& v)
    {
        Ar << v.x << v.y;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FU8Vector3& v)
    {
        Ar << v.x << v.y << v.z;
        return Ar;
    }
    
    inline FArchive& operator<<(FArchive& Ar, FU8Vector4& v)
    {
        Ar << v.x << v.y << v.z << v.w;
        return Ar;
    }
    
    inline FArchive& operator<<(FArchive& Ar, FU16Vector2& v)
    {
        Ar << v.x << v.y;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FU16Vector3& v)
    {
        Ar << v.x << v.y << v.z;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FU16Vector4& v)
    {
        Ar << v.x << v.y << v.z << v.w;
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FMatrix2& m)
    {
        Ar << m[0] << m[1];
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FMatrix3& m)
    {
        Ar << m[0] << m[1] << m[2];
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FMatrix4& m)
    {
        Ar << m[0] << m[1] << m[2] << m[3];
        return Ar;
    }

    inline FArchive& operator<<(FArchive& Ar, FQuat& q)
    {
        Ar << q.x << q.y << q.z << q.w;
        return Ar;
    }

    inline FArchive& operator << (FArchive& Ar, FBitFlags& Data)
    {
        Ar << Data.Flags;
        return Ar;
    }
    
    
}





