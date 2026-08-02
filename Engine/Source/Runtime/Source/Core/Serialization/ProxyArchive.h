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
        FProxyArchive(const FProxyArchive&) = delete;
        FProxyArchive& operator=(FProxyArchive&&) = delete;
        FProxyArchive& operator=(const FProxyArchive&) = delete;

        void Serialize(void* V, int64 Length) override { InnerArchive.Serialize(V, Length); }
        void SerializeBool(bool& D) override { InnerArchive.SerializeBool(D); }
        
        
        void Seek(int64 InPos) override { InnerArchive.Seek(InPos); }
        int64 Tell() override { return InnerArchive.Tell(); }
        int64 TotalSize() override { return InnerArchive.Tell(); }

        bool IsReading() const override { return InnerArchive.IsReading(); }
        bool IsWriting() const override { return InnerArchive.IsWriting(); }
        bool HasError() const override { return InnerArchive.HasError(); }
        
        // Forward all standard types to the inner archive
        FArchive& operator<<(uint8& Value)  override { return InnerArchive << Value; }
        FArchive& operator<<(int8& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(uint16& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(int16& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(uint32& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(int32& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(uint64& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(int64& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(bool& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(float& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(double& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(FString& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(FName& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(FField*& Value) override { return InnerArchive << Value; }
        FArchive& operator<<(FObjectHandle& Value) override { return InnerArchive << Value; }
        
        template<typename ValueType>
        FArchive& operator<<(TVector<ValueType>& Array)
        {
            return InnerArchive << Array;
        }
    
    
    protected:
        
        FArchive& InnerArchive;
        
    };
}
