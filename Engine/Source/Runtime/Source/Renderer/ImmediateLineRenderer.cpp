#include "RuntimePCH.h"
#include "ImmediateLineRenderer.h"

namespace Lumina
{
    FImmediateLineRenderer::FImmediateLineRenderer()
    {
        // One reservation per addressable thread, so GetWorkerIndex() below is always in range.
        const uint32 NumSlots = Jobs::GetNumThreadSlots();
        for (FChannel& Channel : Channels)
        {
            Channel.Reservations.resize(NumSlots);
        }
    }

    FImmediateLineRenderer::~FImmediateLineRenderer()
    {
        // Scene teardown already waited the device, so every slot is idle.
        for (FChannel& Channel : Channels)
        {
            Channel.bOpen.store(false, std::memory_order_release);
            for (uint32 Slot = 0; Slot < kSlots; ++Slot)
            {
                FreeSlot(Channel, Slot);
            }
        }
    }

    void FImmediateLineRenderer::BeginFrame()
    {
        CurrentSlot = (CurrentSlot + 1) % kSlots;

        for (FChannel& Channel : Channels)
        {
            const uint32 Demand = Math::Min(Channel.LastDemand, kMaxVerts);
            const uint32 Needed = Math::Max(Demand + Demand / 2u, kMinVerts);

            EnsureSlotCapacity(Channel, CurrentSlot, Needed);

            Channel.Host     = Channel.SlotHost[CurrentSlot];
            Channel.Gpu      = Channel.SlotGpu[CurrentSlot];
            Channel.Capacity = Channel.SlotCapacity[CurrentSlot];
            Channel.Cursor.store(0, std::memory_order_relaxed);

            Channel.Allocated = 0;
            for (FReservation& Reservation : Channel.Reservations)
            {
                Reservation.Write      = nullptr;
                Reservation.Remaining  = 0;
                Reservation.Dropped    = 0;
                Reservation.bExhausted = false;
            }

            Channel.bOpen.store(Channel.Host != nullptr, std::memory_order_release);
        }
    }

    FImmediateLineRenderer::FDrawRange FImmediateLineRenderer::Snapshot(EChannel Channel)
    {
        FChannel& Ch = Channels[Channel];

        RetireChannel(Ch);

        if (Ch.LastDemand > Ch.Allocated && !Ch.bWarnedOverflow)
        {
            Ch.bWarnedOverflow = true;
            LOG_WARN("ImmediateLines: wanted {} verts, buffer holds {}. Dropped the overflow; growing next frame.",
                     Ch.LastDemand, Ch.Capacity);
        }

        return FDrawRange{ Ch.Allocated > 0 ? Ch.Gpu : 0, Ch.Allocated };
    }

    void FImmediateLineRenderer::CloseFrame()
    {
        for (FChannel& Channel : Channels)
        {
            RetireChannel(Channel);
        }
    }

    void FImmediateLineRenderer::RetireChannel(FChannel& Channel)
    {
        Channel.bOpen.store(false, std::memory_order_release);

        Channel.Allocated = Channel.Cursor.load(std::memory_order_acquire);

        uint64 Demand = Channel.Allocated;

        for (FReservation& Reservation : Channel.Reservations)
        {
            const uint32 Tail = Reservation.Remaining;
            for (uint32 i = 0; i < Tail; ++i)
            {
                Reservation.Write[i] = MakePadVertex();
            }

            Demand += Reservation.Dropped;

            Reservation.Write      = nullptr;
            Reservation.Remaining  = 0;
            Reservation.Dropped    = 0;
            Reservation.bExhausted = false;
        }

        Channel.LastDemand = (uint32)Math::Min<uint64>(Demand, (uint64)kMaxVerts);
    }

    uint64 FImmediateLineRenderer::GetResidentBytes() const
    {
        uint64 Bytes = 0;
        for (const FChannel& Channel : Channels)
        {
            for (uint32 Slot = 0; Slot < kSlots; ++Slot)
            {
                Bytes += (uint64)Channel.SlotCapacity[Slot] * sizeof(FSimpleElementVertex);
            }
        }
        return Bytes;
    }

    FSimpleElementVertex* FImmediateLineRenderer::Reserve(FChannel& Channel, uint32 Count)
    {
        if (!Channel.bOpen.load(std::memory_order_acquire))
        {
            return nullptr;
        }

        const uint32 Base = Channel.Cursor.fetch_add(Count, std::memory_order_relaxed);
        if (Base + Count > Channel.Capacity)
        {
            Channel.Cursor.fetch_sub(Count, std::memory_order_relaxed);
            return nullptr;
        }

        return Channel.Host + Base;
    }

    bool FImmediateLineRenderer::Refill(FChannel& Channel, FReservation& Reservation)
    {
        if (Reservation.bExhausted)
        {
            return false;
        }

        FSimpleElementVertex* Block = Reserve(Channel, kReserveVerts);
        if (Block == nullptr)
        {
            Reservation.bExhausted = true;
            Reservation.Write      = nullptr;
            Reservation.Remaining  = 0;
            return false;
        }

        Reservation.Write     = Block;
        Reservation.Remaining = kReserveVerts;
        return true;
    }

    FSimpleElementVertex* FImmediateLineRenderer::AllocLines(uint32 LineCount, EChannel Channel)
    {
        const uint32 VertexCount = LineCount * 2u;
        if (VertexCount == 0)
        {
            return nullptr;
        }

        FChannel& Ch = Channels[Channel];

        const uint32 ThreadSlot = Jobs::GetWorkerIndex();
        if (ThreadSlot >= Ch.Reservations.size())
        {
            return nullptr;
        }

        FReservation& Reservation = Ch.Reservations[ThreadSlot];
        if (Reservation.Remaining >= VertexCount)
        {
            FSimpleElementVertex* Out = Reservation.Write;
            Reservation.Write     += VertexCount;
            Reservation.Remaining -= VertexCount;
            return Out;
        }

        FSimpleElementVertex* Block = Reserve(Ch, VertexCount);
        if (Block == nullptr)
        {
            Reservation.Dropped += VertexCount;
        }

        return Block;
    }

    void FImmediateLineRenderer::Box(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation,
                                     uint32 PackedColor, EChannel Channel)
    {
        FSimpleElementVertex* V = AllocLines(12, Channel);
        if (V == nullptr)
        {
            return;
        }

        FVector3 Corners[8];
        for (uint32 i = 0; i < 8; ++i)
        {
            const FVector3 Local(
                (i & 1) ? HalfExtents.x : -HalfExtents.x,
                (i & 2) ? HalfExtents.y : -HalfExtents.y,
                (i & 4) ? HalfExtents.z : -HalfExtents.z);
            Corners[i] = Center + Math::Rotate(Rotation, Local);
        }

        static constexpr uint8 Edges[12][2] =
        {
            {0,1},{1,3},{3,2},{2,0},   // -Z face
            {4,5},{5,7},{7,6},{6,4},   // +Z face
            {0,4},{1,5},{2,6},{3,7},   // verticals
        };

        for (uint32 e = 0; e < 12; ++e)
        {
            V[e * 2 + 0] = FSimpleElementVertex{ Corners[Edges[e][0]], PackedColor };
            V[e * 2 + 1] = FSimpleElementVertex{ Corners[Edges[e][1]], PackedColor };
        }
    }

    void FImmediateLineRenderer::Sphere(const FVector3& Center, float Radius, uint32 PackedColor, uint8 Segments, EChannel Channel)
    {
        const uint32 Steps = Math::Max((uint32)Segments, 4u);

        FSimpleElementVertex* V = AllocLines(Steps * 3u, Channel);
        if (V == nullptr)
        {
            return;
        }

        const float Delta = Math::TwoPi<float>() / (float)Steps;

        for (uint32 i = 0; i < Steps; ++i)
        {
            const float A0 = Delta * (float)i;
            const float A1 = Delta * (float)(i + 1);

            const float C0 = Math::Cos(A0) * Radius;
            const float S0 = Math::Sin(A0) * Radius;
            const float C1 = Math::Cos(A1) * Radius;
            const float S1 = Math::Sin(A1) * Radius;

            // XY, XZ, YZ rings, laid out ring-major so each is a contiguous run.
            V[(i * 3 + 0) * 2 + 0] = FSimpleElementVertex{ Center + FVector3(C0, S0, 0.0f), PackedColor };
            V[(i * 3 + 0) * 2 + 1] = FSimpleElementVertex{ Center + FVector3(C1, S1, 0.0f), PackedColor };

            V[(i * 3 + 1) * 2 + 0] = FSimpleElementVertex{ Center + FVector3(C0, 0.0f, S0), PackedColor };
            V[(i * 3 + 1) * 2 + 1] = FSimpleElementVertex{ Center + FVector3(C1, 0.0f, S1), PackedColor };

            V[(i * 3 + 2) * 2 + 0] = FSimpleElementVertex{ Center + FVector3(0.0f, C0, S0), PackedColor };
            V[(i * 3 + 2) * 2 + 1] = FSimpleElementVertex{ Center + FVector3(0.0f, C1, S1), PackedColor };
        }
    }

    void FImmediateLineRenderer::EnsureSlotCapacity(FChannel& Channel, uint32 Slot, uint32 NeededVerts)
    {
        NeededVerts = Math::Min(NeededVerts, kMaxVerts);

        if (NeededVerts > Channel.SlotCapacity[Slot])
        {
            FreeSlot(Channel, Slot);

            const uint64 Bytes = (uint64)NeededVerts * sizeof(FSimpleElementVertex);
            const RHI::GPUPtr Gpu = RHI::Malloc(Bytes, RHI::kDefaultAlign, RHI::EMemoryType::CPUWrite);
            if (Gpu == 0)
            {
                LOG_ERROR("ImmediateLines: {} KiB CPU-visible allocation failed; debug lines are off this frame.", Bytes / 1024);
                return;
            }

            Channel.SlotGpu[Slot]      = Gpu;
            Channel.SlotHost[Slot]     = static_cast<FSimpleElementVertex*>(RHI::ToHost(Gpu));
            Channel.SlotCapacity[Slot] = NeededVerts;
            Channel.SlotLowUsage[Slot] = 0;
            Channel.bWarnedOverflow    = false;
            return;
        }

        if (NeededVerts * 4u < Channel.SlotCapacity[Slot] && Channel.SlotCapacity[Slot] > kMinVerts)
        {
            if (++Channel.SlotLowUsage[Slot] >= 240u)
            {
                const uint32 Target = Math::Max(NeededVerts, kMinVerts);
                FreeSlot(Channel, Slot);

                const RHI::GPUPtr Gpu = RHI::Malloc((uint64)Target * sizeof(FSimpleElementVertex), RHI::kDefaultAlign, RHI::EMemoryType::CPUWrite);
                if (Gpu != 0)
                {
                    Channel.SlotGpu[Slot]      = Gpu;
                    Channel.SlotHost[Slot]     = static_cast<FSimpleElementVertex*>(RHI::ToHost(Gpu));
                    Channel.SlotCapacity[Slot] = Target;
                }
                Channel.SlotLowUsage[Slot] = 0;
            }
        }
        else
        {
            Channel.SlotLowUsage[Slot] = 0;
        }
    }

    void FImmediateLineRenderer::FreeSlot(FChannel& Channel, uint32 Slot)
    {
        // No DeferFree: this slot's last submission finished at the previous frame's fence (see kSlots).
        if (Channel.SlotGpu[Slot] != 0)
        {
            RHI::Free(Channel.SlotGpu[Slot]);
        }
        Channel.SlotGpu[Slot]      = 0;
        Channel.SlotHost[Slot]     = nullptr;
        Channel.SlotCapacity[Slot] = 0;
    }
}
