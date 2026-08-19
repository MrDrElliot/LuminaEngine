#pragma once
#include <limits>
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Renderer/RHI.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
    class RUNTIME_API FImmediateLineRenderer
    {
    public:

        enum EChannel : uint8
        {
            DepthTested = 0,
            XRay        = 1,
            NumChannels = 2,
        };

        static constexpr uint32 kSlots = RHI::kFramesInFlight + 2;

        // Verts per thread reservation: 8 KiB, so one atomic buys 256 lines.
        static constexpr uint32 kReserveVerts = 512;

        static constexpr uint32 kMinVerts = 64u * 1024u;
        static constexpr uint32 kMaxVerts = 4u * 1024u * 1024u;

        static FSimpleElementVertex MakePadVertex()
        {
            FSimpleElementVertex Pad;
            Pad.Position.x = std::numeric_limits<float>::infinity();
            Pad.Position.y = Pad.Position.x;
            Pad.Position.z = Pad.Position.x;
            Pad.Color      = 0u;
            return Pad;
        }

        struct FDrawRange
        {
            RHI::GPUPtr Vertices    = 0;
            uint32      VertexCount = 0;
        };

        FImmediateLineRenderer();
        ~FImmediateLineRenderer();

        FImmediateLineRenderer(const FImmediateLineRenderer&) = delete;
        FImmediateLineRenderer& operator=(const FImmediateLineRenderer&) = delete;

        void BeginFrame();

        /** Close the write window and report what to draw. Vertices is 0 when the channel is empty. */
        FDrawRange Snapshot(EChannel Channel);

        void CloseFrame();

        /** Verts actually requested last frame, whether or not they fit. Editor stats. */
        uint32 GetLastDemand(EChannel Channel) const { return Channels[Channel].LastDemand; }

        /** Bytes of CPU-visible VRAM this renderer is holding across all slots and channels. */
        uint64 GetResidentBytes() const;

        //~ Producers ------------------------------------------------------------------------

        FORCEINLINE void Line(const FVector3& Start, const FVector3& End, uint32 PackedColor, EChannel Channel = DepthTested)
        {
            FChannel& Ch = Channels[Channel];

            const uint32 ThreadSlot = Jobs::GetWorkerIndex();
            if (ThreadSlot >= Ch.Reservations.size())
            {
                return;
            }

            FReservation& R = Ch.Reservations[ThreadSlot];
            if (R.Remaining == 0 && !Refill(Ch, R))
            {
                R.Dropped += 2;
                return;
            }

            R.Write[0].Position = Start;
            R.Write[0].Color    = PackedColor;
            R.Write[1].Position = End;
            R.Write[1].Color    = PackedColor;
            R.Write     += 2;
            R.Remaining -= 2;
        }

        FORCEINLINE void Line(const FVector3& Start, const FVector3& End, const FVector4& Color, EChannel Channel = DepthTested)
        {
            Line(Start, End, PackColor(Color), Channel);
        }

        FSimpleElementVertex* AllocLines(uint32 LineCount, EChannel Channel = DepthTested);

        /** 12 edges of an oriented box. */
        void Box(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, uint32 PackedColor, EChannel Channel = DepthTested);

        /** Three axis-aligned rings. */
        void Sphere(const FVector3& Center, float Radius, uint32 PackedColor, uint8 Segments = 16, EChannel Channel = DepthTested);

    private:

        // Cache-line isolated: every producing thread holds one and they are written every line.
        struct alignas(64) FReservation
        {
            FSimpleElementVertex*   Write     = nullptr;
            uint32                  Remaining = 0;

            uint32                  Dropped   = 0;

            bool                    bExhausted = false;
        };

        struct FChannel
        {
            FChannel() = default;
            FChannel(const FChannel&) = delete;
            FChannel& operator=(const FChannel&) = delete;

            // Live slot's mapped range, republished each BeginFrame.
            FSimpleElementVertex*   Host     = nullptr;
            RHI::GPUPtr             Gpu      = 0;
            uint32                  Capacity = 0;

            alignas(64) TAtomic<uint32> Cursor{ 0 };

            TAtomic<bool>           bOpen{ false };

            uint32                  Allocated = 0;

            // Allocated + everything that did not fit. Only ever feeds the next frame's sizing.
            uint32                  LastDemand = 0;
            bool                    bWarnedOverflow = false;

            TVector<FReservation>   Reservations;

            RHI::GPUPtr             SlotGpu[kSlots]      = {};
            FSimpleElementVertex*   SlotHost[kSlots]     = {};
            uint32                  SlotCapacity[kSlots] = {};
            uint32                  SlotLowUsage[kSlots] = {};
        };

        // Hands back a contiguous run of Count verts, or null once the slot is full.
        FSimpleElementVertex* Reserve(FChannel& Channel, uint32 Count);
        bool Refill(FChannel& Channel, FReservation& Reservation);

        // Close the window, latch the demand, pad every thread's tail block.
        void RetireChannel(FChannel& Channel);

        void EnsureSlotCapacity(FChannel& Channel, uint32 Slot, uint32 NeededVerts);
        void FreeSlot(FChannel& Channel, uint32 Slot);

        FChannel    Channels[NumChannels];
        uint32      CurrentSlot = kSlots - 1;
    };
}
