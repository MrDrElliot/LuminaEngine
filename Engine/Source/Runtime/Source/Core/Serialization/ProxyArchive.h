#pragma once
#include "Archiver.h"

namespace Lumina
{
    class FProxyArchive : public FArchive
    {
    public:

        FProxyArchive(FArchive& InInnerAr)
            :InnerArchive(InInnerAr)
        {
            // Inherited, or a proxy over a loaded package reads as the current engine version and misparses.
            SetFileVersion(InInnerAr.GetFileVersion());

            if (InInnerAr.IsReading())
            {
                SetFlag(EArchiverFlags::Reading);
            }
            else if (InInnerAr.IsWriting())
            {
                SetFlag(EArchiverFlags::Writing);
            }
        }

        // Non-copyable
        FProxyArchive(FProxyArchive&&) = delete;
        ~FProxyArchive() = default;
        FProxyArchive(const FProxyArchive&) = delete;
        FProxyArchive& operator=(FProxyArchive&&) = delete;
        FProxyArchive& operator=(const FProxyArchive&) = delete;

        void Serialize(void* V, int64 Length) override { InnerArchive.Serialize(V, Length); }
        void SerializeBool(bool& D) override { InnerArchive.SerializeBool(D); }
        
        
        void SerializeEntityId(uint32& PackedEntity) override { InnerArchive.SerializeEntityId(PackedEntity); }
        void Seek(int64 InPos) override { InnerArchive.Seek(InPos); }
        int64 Tell() override { return InnerArchive.Tell(); }
        int64 TotalSize() override { return InnerArchive.TotalSize(); }

        bool IsReading() const override { return InnerArchive.IsReading(); }
        bool IsWriting() const override { return InnerArchive.IsWriting(); }
        bool HasError() const override { return InnerArchive.HasError(); }
        void SetHasError(bool bIsError) override { InnerArchive.SetHasError(bIsError); }

        bool DescribeCutBytes(FPortableBytes& Cut) const override { return InnerArchive.DescribeCutBytes(Cut); }

        void RegisterSoftAssetReference(const FGuid& AssetGUID) override { InnerArchive.RegisterSoftAssetReference(AssetGUID); }
        FStringView SetSoftReferenceBundles(FStringView Bundles) override { return InnerArchive.SetSoftReferenceBundles(Bundles); }
        bool RewriteSoftAssetReference(FString& Path, FGuid& AssetGUID) override { return InnerArchive.RewriteSoftAssetReference(Path, AssetGUID); }

        bool SupportsBulkData() const override { return InnerArchive.SupportsBulkData(); }
        bool WriteBulkData(FBulkDataRef& OutRef, const void* Data, int64 Size) override { return InnerArchive.WriteBulkData(OutRef, Data, Size); }
        void FlagUnresolvedBulkData() override { InnerArchive.FlagUnresolvedBulkData(); }

        // Keeps the base overloads this class does not forward (FFixedString&, container and enum templates) visible.
        using FArchive::operator<<;

        // Each forward returns this archive, so a chained `Ar << A << B` never falls through to the inner one.
        FArchive& operator<<(uint8& Value)  override { InnerArchive << Value; return *this; }
        FArchive& operator<<(int8& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(uint16& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(int16& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(uint32& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(int32& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(uint64& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(int64& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(bool& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(float& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(double& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(FString& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(FName& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(FField*& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(FObjectHandle& Value) override { InnerArchive << Value; return *this; }
        FArchive& operator<<(CObject*& Value) override { InnerArchive << Value; return *this; }
    
    
    protected:
        
        FArchive& InnerArchive;
        
    };
}
