#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandle.h"
#include "Core/Templates/CanBulkSerialize.h"
#include "Core/Templates/IsSigned.h"
#include "Core/Versioning/CoreVersion.h"
#include "Core/Math/Math.h"

#include "EASTL/bit.h"
#include "Log/Log.h"
#include "Types/BitFlags.h"

namespace Lumina
{
    class FField;
    class FGuid;
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
};

namespace Lumina
{
    class CObject;

    /**
     * Locates a payload inside its package's bulk-data region. Written inline in the export, so an object
     * knows where its heavy data lives without any of it being resident; the bytes are pulled later with
     * CPackage::ReadBulkData, which turns into one ranged file read.
     *
     * Offset is relative to the start of the region, NOT the file -- the region's own file offset moves
     * whenever the compressed container changes size, so a file-absolute offset would be invalidated by
     * every save.
     */
    struct FBulkDataRef
    {
        int64 Offset = 0;
        int64 Size   = 0;

        bool IsValid() const { return Size > 0; }
    };

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

        FORCEINLINE void SetHasError(bool bIsError) { bHasError = bIsError; }
        FORCEINLINE virtual bool HasError() const { return bHasError; }

        FORCEINLINE static FPackageFileVersion GetEngineVersion()
        {
            return GPackageFileLuminaVersion;
        }

        /** File version being read/written; LoadPackage sets it from the header so per-type
         *  Serialize can branch on source version to migrate older payloads. */
        FORCEINLINE int32 GetFileVersion() const { return FileVersion; }
        FORCEINLINE void SetFileVersion(int32 InVersion) { FileVersion = InVersion; }

        FORCEINLINE size_t GetMaxSerializeSize() const { return ArMaxSerializeSize; }

        
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

        /** True when WriteBulkData will actually do something. Only the package saver has somewhere to put a
         *  bulk region; every other archive (duplication, transient, network) must keep payloads inline, so
         *  a serializer that can split has to branch on this rather than assume. */
        virtual bool SupportsBulkData() const { return false; }

        /** Append Size bytes to the archive's bulk region and fill OutRef with where they landed. Returns
         *  false (leaving OutRef untouched) on archives with no region -- write the payload inline instead. */
        virtual bool WriteBulkData(FBulkDataRef& OutRef, const void* Data, int64 Size) { return false; }
    
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
            ByteOrderSerialize(reinterpret_cast<uint16&>(Value));
            return *this;
        }
    
        virtual FArchive& operator<<(uint32& Value)
        {
            ByteOrderSerialize(Value);
            return *this;
        }
    
        virtual FArchive& operator<<(int32& Value)
        {
            ByteOrderSerialize(reinterpret_cast<uint32&>(Value));
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
            ByteOrderSerialize(reinterpret_cast<uint64&>(Value));
            return *this;
        }

        virtual void SerializeBool(bool& D);
        
        virtual FArchive& operator<<(FString& Str)
        {
            if (IsReading())
            {
                size_t SaveNum = 0;
                *this << SaveNum;
            
                if (SaveNum > GetMaxSerializeSize())
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, string is too large! (Size: {0}, Max: {1})", SaveNum, GetMaxSerializeSize());
                    return *this;
                }
            
                if (SaveNum)
                {
                    Str.clear();
                    Str.shrink_to_fit();
                    Str.resize(SaveNum);
                    Serialize(Str.data(), SaveNum);
                }
                else
                {
                    Str.clear();
                }
            }
            else
            {
                size_t SaveNum = Str.size();
                *this << SaveNum;

                if (SaveNum)
                {
                    Serialize(Str.data(), SaveNum);
                }
            }
        
            return *this;
        }
        
        virtual FArchive& operator<<(FFixedString& Str)
        {
            if (IsReading())
            {
                size_t SaveNum = 0;
                *this << SaveNum;
            
                if (SaveNum > GetMaxSerializeSize())
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, string is too large! (Size: {0}, Max: {1})", SaveNum, GetMaxSerializeSize());
                    return *this;
                }
            
                if (SaveNum)
                {
                    Str.clear();
                    Str.shrink_to_fit();
                    Str.resize(SaveNum);
                    Serialize(Str.data(), SaveNum);
                }
                else
                {
                    Str.clear();
                }
            }
            else
            {
                size_t SaveNum = Str.size();
                *this << SaveNum;

                if (SaveNum)
                {
                    Serialize(Str.data(), SaveNum);
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
                // None's wire form is the empty string; c_str() renders "NAME_None", which is a
                // display string and must not round-trip into a real name.
                FString SavedString(Name.IsNone() ? "" : Name.c_str());
                *this << SavedString;
            }

            return *this;
        }

        template<typename ValueType>
        FArchive& operator << (TVector<ValueType>& Array)
        {
            size_t SerializeNum = IsReading() ? 0 : Array.size();
            *this << SerializeNum;
        
            if (SerializeNum == 0)
            {
                if (IsReading())
                {
                    Array.clear();
                    Array.shrink_to_fit();
                }
            
                return *this;
            }
        
            if (SerializeNum > GetMaxSerializeSize())
            {
                SetHasError(true);
                LOG_ERROR("Archiver is corrupted, attempted to serialize {} array elements. Max is: {}", SerializeNum, GetMaxSerializeSize());
                return *this;
            }
            
            if constexpr (sizeof(ValueType) == 1 || TCanBulkSerialize<ValueType>::value)
            {
                if (IsReading())
                {
                    Array.clear();
                    Array.shrink_to_fit();
                    Array.resize(SerializeNum);
                }
            
                Serialize(Array.data(), SerializeNum * sizeof(ValueType));
            }
            else
            {
                if (IsReading())
                {
                    Array.clear();
                    Array.shrink_to_fit();
                    Array.resize(SerializeNum);
                }

                for (size_t i = 0; i < SerializeNum; i++)
                {
                    *this << Array[i];
                }
            }

            return *this;
        }
        
        template<typename K, typename V>
        FArchive& operator << (TPair<K, V>& Pair)
        {
            *this << Pair.first;
            *this << Pair.second;
            
            return *this;
        }
        
        template<typename K, typename V>
        FArchive& operator<<(THashMap<K, V>& Map)
        {
            uint32 Count;

            if (IsWriting())
            {
                Count = static_cast<uint32>(Map.size());
                *this << Count;

                for (TPair<K, V> Pair : Map)
                {
                    *this << Pair;
                }
            }
            else
            {
                *this << Count;

                if (Count > GetMaxSerializeSize())
                {
                    SetHasError(true);
                    LOG_ERROR("Archiver is corrupted, attempted to serialize {} map entries. Max is: {}", Count, GetMaxSerializeSize());
                    return *this;
                }

                Map.clear();
                Map.reserve(Count);

                for (uint32 i = 0; i < Count; ++i)
                {
                    TPair<K, V> Pair;
                    *this << Pair;
                    Map.emplace(Pair.first, std::move(Pair.second));
                }
            }

            return *this;
        }
        
        template<typename EnumType>
        FORCEINLINE FArchive& operator<<(EnumType& Value)
        requires (eastl::is_enum_v<EnumType>)
        {
            using Underlying = eastl::underlying_type_t<EnumType>;
            return *this << reinterpret_cast<Underlying&>(Value);
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





