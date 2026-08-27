#include "RuntimePCH.h"
#include "ProceduralAudioStream.h"

#include "Core/Math/Scalar.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
    FProceduralAudioStream::FProceduralAudioStream(uint32 InSampleRate, uint32 InChannelCount, uint32 BufferFrames)
        : SampleRate(InSampleRate)
        , ChannelCount(InChannelCount)
    {
        if (InSampleRate == 0 || InChannelCount == 0 || BufferFrames == 0)
        {
            LOG_ERROR("FProceduralAudioStream: invalid parameters (rate={}, channels={}, frames={})",
                InSampleRate, InChannelCount, BufferFrames);
            return;
        }

        LUMINA_MEMORY_SCOPE("Audio");
        Storage.assign((size_t)BufferFrames * InChannelCount, 0.0f);

        CapacityFrames = BufferFrames;
        bInitialized = true;
    }

    uint32 FProceduralAudioStream::Write(const float* Samples, uint32 NumFrames)
    {
        if (!bInitialized || Samples == nullptr || NumFrames == 0)
        {
            return 0;
        }

        const uint64 Write = WriteCursor.load(Atomic::MemoryOrderRelaxed);
        const uint64 Read  = ReadCursor.load(Atomic::MemoryOrderAcquire);

        const uint32 Free = CapacityFrames - (uint32)(Write - Read);
        const uint32 FrameCount = Math::Min(NumFrames, Free);
        if (FrameCount == 0)
        {
            return 0;
        }

        const uint32 Offset = (uint32)(Write % CapacityFrames);
        const uint32 FirstRun = Math::Min(FrameCount, CapacityFrames - Offset);

        Memory::Memcpy(Storage.data() + (size_t)Offset * ChannelCount, Samples, (size_t)FirstRun * ChannelCount * sizeof(float));

        if (FrameCount > FirstRun)
        {
            Memory::Memcpy(Storage.data(), Samples + (size_t)FirstRun * ChannelCount,
                (size_t)(FrameCount - FirstRun) * ChannelCount * sizeof(float));
        }

        WriteCursor.store(Write + FrameCount, Atomic::MemoryOrderRelease);
        return FrameCount;
    }

    uint32 FProceduralAudioStream::Read(float* Out, uint32 NumFrames)
    {
        if (Out == nullptr || NumFrames == 0)
        {
            return 0;
        }

        if (!bInitialized)
        {
            Memory::Memzero(Out, (size_t)NumFrames * ChannelCount * sizeof(float));
            return 0;
        }

        const uint64 Read  = ReadCursor.load(Atomic::MemoryOrderRelaxed);
        const uint64 Write = WriteCursor.load(Atomic::MemoryOrderAcquire);

        const uint32 Available = (uint32)(Write - Read);
        const uint32 FrameCount = Math::Min(NumFrames, Available);

        if (FrameCount != 0)
        {
            const uint32 Offset = (uint32)(Read % CapacityFrames);
            const uint32 FirstRun = Math::Min(FrameCount, CapacityFrames - Offset);

            Memory::Memcpy(Out, Storage.data() + (size_t)Offset * ChannelCount, (size_t)FirstRun * ChannelCount * sizeof(float));

            if (FrameCount > FirstRun)
            {
                Memory::Memcpy(Out + (size_t)FirstRun * ChannelCount, Storage.data(),
                    (size_t)(FrameCount - FirstRun) * ChannelCount * sizeof(float));
            }

            ReadCursor.store(Read + FrameCount, Atomic::MemoryOrderRelease);
        }

        // A live stream has no end, so an underrun is silence rather than a stopped voice.
        if (FrameCount < NumFrames)
        {
            Memory::Memzero(Out + (size_t)FrameCount * ChannelCount,
                (size_t)(NumFrames - FrameCount) * ChannelCount * sizeof(float));
        }

        return FrameCount;
    }

    uint32 FProceduralAudioStream::GetAvailableWriteFrames() const
    {
        if (!bInitialized)
        {
            return 0;
        }

        const uint64 Write = WriteCursor.load(Atomic::MemoryOrderRelaxed);
        const uint64 Read  = ReadCursor.load(Atomic::MemoryOrderAcquire);
        return CapacityFrames - (uint32)(Write - Read);
    }

    uint32 FProceduralAudioStream::GetAvailableReadFrames() const
    {
        if (!bInitialized)
        {
            return 0;
        }

        const uint64 Read  = ReadCursor.load(Atomic::MemoryOrderRelaxed);
        const uint64 Write = WriteCursor.load(Atomic::MemoryOrderAcquire);
        return (uint32)(Write - Read);
    }
}
