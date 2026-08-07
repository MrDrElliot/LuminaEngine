#pragma once
#include <limits>
#include "Containers/Array.h"
#include "Core/Threading/Atomic.h"
#include "Renderer/RHI.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
    /**
     * Immediate-mode debug lines.
     *
     * Producers write finished GPU vertices straight into mapped (ReBAR) memory -- there is no
     * staging vector, no bucketing pass and no upload copy between DrawLine and the draw call.
     * A line costs one bounds check and two 16-byte stores in the steady state; a thread takes an
     * atomic once per kReserveVerts, not once per line.
     *
     * That speed is bought by dropping everything FLineBatcherComponent offers:
     *   - single frame only, no Duration
     *   - one thickness (thickness is raster state, so N widths would mean N draws)
     *   - no CPU frustum cull (nothing downstream would be skipped by it -- the verts are already final)
     *
     * Use CWorld::DrawLine for timed or thick lines. Use this for the volume cases: navmesh tiles,
     * distance fields, broadphase, cloth, particle debug.
     *
     * Threading: any thread may draw between BeginFrame and Snapshot. Each thread bump-reserves a
     * block with a single atomic and then owns it alone, so producers never contend after that.
     */
    class RUNTIME_API FImmediateLineRenderer
    {
    public:

        enum EChannel : uint8
        {
            DepthTested = 0,
            XRay        = 1,
            NumChannels = 2,
        };

        // Two more slots than frames in flight, because the write window opens EARLY: right after
        // ExtractWorlds, so it is already open when the next frame's physics step draws its debug
        // lines rather than leaving them outside any window. At that point frame N is recorded but not yet submitted and
        // frames N-1/N-2 may still be executing, so slots N, N-1 and N-2 are all spoken for and the
        // window has to take N+1. Its previous user, frame N-3, was fenced at frame N-1's FrameEnd --
        // which is why FreeSlot can call RHI::Free inline instead of going through DeferFree.
        static constexpr uint32 kSlots = RHI::kFramesInFlight + 2;

        // Verts per thread reservation: 8 KiB, so one atomic buys 256 lines.
        static constexpr uint32 kReserveVerts = 512;

        // Floor and ceiling on a channel's per-slot allocation (1 MiB .. 64 MiB). The floor is set so an
        // ordinary scene never overflows on its first frame, before the demand feedback has run once.
        static constexpr uint32 kMinVerts = 64u * 1024u;
        static constexpr uint32 kMaxVerts = 4u * 1024u * 1024u;

        // Padding vertices carry an infinite position; SimpleElementVertex.slang maps that to a
        // trivially-rejected clip position so a partly-filled tail block rasterizes nothing.
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

        /** Open the write window: advance the slot, grow to last frame's demand, reset the cursors.
         *  Once per frame per scene, before anything draws. */
        void BeginFrame();

        /** Close the write window and report what to draw. Vertices is 0 when the channel is empty. */
        FDrawRange Snapshot(EChannel Channel);

        /** Close without publishing, for a scene that stopped extracting (suspended world). Leaving a
         *  window open on one of those is how you get producers filling a buffer nobody ever resets. */
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
                // Charge the demand what this line actually wanted, not the block size Refill asked
                // for -- otherwise one dropped line reads as 512 and the next frame's buffer is sized
                // hundreds of times too large.
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

        /** Raw write cursor for 2*LineCount vertices, or null when the frame's budget is spent.
         *  The caller must fill every vertex it asked for -- unwritten ones rasterize as garbage. */
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

            // Verts this thread wanted after the slot filled up. Thread-private, so no atomic; summed
            // into the demand at retire time and fed back into next frame's size.
            uint32                  Dropped   = 0;

            // Latched once a block reservation fails. Stops every later line on this thread from
            // hammering the shared cursor for a slot that is already full.
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

            // Verts handed out this frame. Deliberately allowed to run past Capacity: the overshoot is
            // what BeginFrame grows against, so an overflowed frame self-corrects on the next one.
            alignas(64) TAtomic<uint32> Cursor{ 0 };

            TAtomic<bool>           bOpen{ false };

            // Verts actually handed out, latched at retire. This -- not LastDemand -- is what gets
            // drawn: everything in [0, Allocated) is written, everything past it is stale.
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
