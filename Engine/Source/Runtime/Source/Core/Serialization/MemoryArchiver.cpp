#include "RuntimePCH.h"
#include "MemoryArchiver.h"
#include "Log/Log.h"

namespace Lumina
{
    FMemoryReader::FMemoryReader(const TVector<uint8>& InBytes, bool bIsPersistent)
        : Bytes    (InBytes)
        , LimitSize(INT64_MAX)
    {
        this->SetFlag(EArchiverFlags::Reading);
    }

    int64 FMemoryReader::TotalSize()
    {
        return std::min((int64)Bytes.size(), LimitSize);   
    }


    void FMemoryReader::SetLimitSize(int64 NewLimitSize)
    {
        LimitSize = NewLimitSize;
    }

    void FMemoryReader::Seek(int64 InPos)
    {
        // Clamp + flag: a corrupt offset reaching downstream Serialize() indexes a junk pointer.
        const int64 Total = TotalSize();
        if (InPos < 0 || InPos > Total)
        {
            LOG_ERROR("FMemoryReader::Seek out of range: pos={}, size={}", InPos, Total);
            SetHasError(true);
            Offset = (InPos < 0) ? 0 : Total;
            return;
        }
        Offset = InPos;
    }

    void FMemoryReader::Serialize(void* V, int64 Size)
    {
        if ((Size) && !HasError())
        {
            if (Size <= 0)
            {
                SetHasError(true);
                return;
            }
                
            // Only serialize if we have the requested amount of data
            if (Offset + Size <= TotalSize())
            {
                Memory::Memcpy(V, &Bytes[Offset], Size);
                Offset += Size;
            }
            else
            {
                SetHasError(true);
                LOG_ERROR("Archiver does not have enough size! Requested: {} - Total: {}", Offset + Size, TotalSize());
            }
        } 
    }

    FBufferReader::FBufferReader(void* InData, int64 InSize, bool bFreeAfterClose)
        : ReaderData(InData)
        , ReaderPos(0)
        , ReaderSize(InSize)
        , bFreeOnClose(bFreeAfterClose)
    {
        SetFlag(EArchiverFlags::Reading);
    }

    FBufferReader::~FBufferReader()
    {
        if (bFreeOnClose && ReaderData)
        {
            Memory::Free(ReaderData);
            ReaderData = nullptr;
        }
    }

    int64 FBufferReader::Tell()
    {
        return ReaderPos;
    }

    int64 FBufferReader::TotalSize()
    {
        return ReaderSize;
    }

    void FBufferReader::Seek(int64 InPos)
    {
        if (InPos < 0 || InPos > ReaderSize)
        {
            LOG_ERROR("FBufferReader::Seek out of range: pos={}, size={}", InPos, ReaderSize);
            SetHasError(true);
            ReaderPos = (InPos < 0) ? 0 : ReaderSize;
            return;
        }
        ReaderPos = InPos;
    }
    
    void FBufferReader::Serialize(void* Data, int64 Size)
    {
        DEBUG_ASSERT(ReaderPos >= 0);
        DEBUG_ASSERT(Size >= 0);

        if (Size == 0)
        {
            return;
        }

        // Check for overflow and out-of-bounds
        if (Size < 0 || ReaderPos + Size > ReaderSize)
        {
            SetHasError(true);
            Memory::Memzero(Data, Size > 0 ? Size : 0);
            LOG_ERROR("FBufferReader: Attempted to read {0} bytes at position {1}, but buffer size is {2}", Size, ReaderPos, ReaderSize);
            return;
        }

        Memory::Memcpy(Data, static_cast<const uint8*>(ReaderData) + ReaderPos, Size);
        ReaderPos += Size;
    }
    
    FMemoryWriter::FMemoryWriter(TVector<uint8>& InBytes, uint32 StartOffset)
        :Bytes(InBytes)
    {
        Offset = StartOffset;
        this->SetFlag(EArchiverFlags::Writing);
    }

    void FMemoryWriter::Serialize(void* Data, int64 Size)
    {
        const int64 NumBytesToAdd = Offset + Size - static_cast<int64>(Bytes.size());
        if (NumBytesToAdd > 0)
        {
            const int64 NewArrayCount = static_cast<int64>(Bytes.size()) + NumBytesToAdd;
                
            Bytes.resize(NewArrayCount);
        }

        ASSERT((Offset + Size) <= (int64)Bytes.size());
        
        Memory::Memcpy(&Bytes[Offset], Data, Size);
        Offset += Size;
    }
}
