#include "RuntimePCH.h"
#include "MeshletHeaderSlab.h"

#include "RHI.h"
#include "RHICore.h"
#include "RHIUpload.h"

#include "Core/Threading/Thread.h"
#include "Log/Log.h"

namespace Lumina::MeshletHeaderSlab
{
    namespace
    {
        // Meshes with GPU buffers number in the hundreds, so this is usually the only allocation made.
        constexpr uint32 kInitialCapacity = 1024;

        // A wrap guard, since doubling past this overflows and spins the reserve loop forever.
        constexpr uint32 kMaxCapacity = 1u << 30;

        FMutex GMutex;

        // 80 bytes per mesh, so a mirror that lets a grow repopulate without a GPU copy is worth keeping.
        TVector<FMeshletHeaderGPU> GMirror;

        RHI::GPUPtr GSlab     = 0;
        uint32      GCapacity = 0;

        // A grown slab whose mirror upload has not run; publishing before it does reads recycled VRAM as headers.
        RHI::GPUPtr GPendingSlab     = 0;
        uint32      GPendingCapacity = 0;
        uint64      GPendingBatch    = 0;

        // Slots kept in hand when a grow starts, so the swap can land before the published capacity runs out.
        constexpr uint32 kGrowSlack = 64;

        // FIFO, with Release pushing to the back and Acquire taking from the front. See the header.
        TVector<uint32> GFreeList;
        SIZE_T          GFreeHead = 0;

        // The version says this is no longer the header the deferred reset was told to retire.
        TVector<uint32> GSlotVersion;

        // Highest slot ever handed out. Everything below it is either live or in the free list.
        uint32 GNextSlot = kNullSlot + 1u;

        // Not null, since VisBufferSurface clamps its index and reaches element 0 of a zero-count array.
        constexpr uint64 kNullGeometryBytes = 4096;
        RHI::GPUPtr GNullGeometry = 0;

        /** Caller holds GMutex. */
        void EnsureNullGeometryLocked()
        {
            if (GNullGeometry != 0)
            {
                return;
            }

            GNullGeometry = RHI::Malloc(kNullGeometryBytes, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            if (GNullGeometry == 0)
            {
                // Readers that clamp instead of rejecting would dereference address 0, so no slab is better than one.
                LOG_ERROR("MeshletHeaderSlab: the {} B null-geometry page failed to allocate; no header slots "
                          "will be handed out.", kNullGeometryBytes);
                return;
            }

            RHI::SetDebugName(GNullGeometry, "Mesh.NullGeometry");

            const TVector<uint8> Zeroes((SIZE_T)kNullGeometryBytes, (uint8)0);
            RHI::UploadBuffer(GNullGeometry, Zeroes.data(), (SIZE_T)kNullGeometryBytes);
        }

        /** Caller holds GMutex and has called EnsureNullGeometryLocked. */
        FMeshletHeaderGPU MakeNullHeader()
        {
            FMeshletHeaderGPU Header = {};

            // MeshletCount stays 0, and these addresses exist only for readers that clamp instead.
            Header.MeshletsAddress  = GNullGeometry;
            Header.SpheresAddress   = GNullGeometry;
            Header.VerticesAddress  = GNullGeometry;
            Header.TrianglesAddress = GNullGeometry;
            Header.ConesAddress     = GNullGeometry;

            // Zero is a VALID heap slot, so the sentinel has to be written explicitly.
            Header.DistanceFieldIndex = DistanceField::kInvalidIndex;
            return Header;
        }

        // Swaps a staged slab in once its mirror copy has run on the GPU. Caller holds GMutex.
        void PublishPendingLocked()
        {
            if (GPendingSlab == 0 || !RHI::Upload::IsBatchComplete(GPendingBatch))
            {
                return;
            }

            // Scene roots are rebuilt every frame and nothing caches the old slab.
            RHI::Core::Retire(GSlab);

            GSlab            = GPendingSlab;
            GCapacity        = GPendingCapacity;
            GPendingSlab     = 0;
            GPendingCapacity = 0;
            GPendingBatch    = 0;
        }

        // Stages a slab holding at least Needed slots. Caller holds GMutex; publishing is separate.
        void ReserveLocked(uint32 Needed)
        {
            PublishPendingLocked();

            if (Needed <= GCapacity || Needed <= GPendingCapacity)
            {
                return;
            }

            uint32 NewCapacity = GCapacity == 0 ? kInitialCapacity : GCapacity;
            while (NewCapacity < Needed && NewCapacity <= kMaxCapacity / 2u)
            {
                NewCapacity *= 2u;
            }

            if (NewCapacity < Needed)
            {
                LOG_ERROR("MeshletHeaderSlab: {} slots requested, past the {} slot ceiling.", Needed, kMaxCapacity);
                return;
            }

            const uint64 Bytes = (uint64)NewCapacity * sizeof(FMeshletHeaderGPU);
            const RHI::GPUPtr NewSlab = RHI::Malloc(Bytes, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            if (NewSlab == 0)
            {
                LOG_ERROR("MeshletHeaderSlab: allocation of {} KiB failed; meshes acquiring a header slot "
                          "from here on will render as no geometry.", Bytes / 1024);
                return;
            }

            RHI::SetDebugName(NewSlab, "Mesh.MeshletHeaderSlab");

            // The whole mirror, so the staged allocation is fully defined by the time it is published.
            GMirror.resize(NewCapacity, MakeNullHeader());
            GSlotVersion.resize(NewCapacity, 0u);
            if (!RHI::UploadBuffer(NewSlab, GMirror.data(), (SIZE_T)Bytes))
            {
                // Nothing was queued, so no batch names this copy and publishing would swap in raw Malloc bytes.
                LOG_ERROR("MeshletHeaderSlab: the mirror copy into a {} slot slab was dropped; the grow is abandoned.",
                          NewCapacity);
                RHI::Core::Retire(NewSlab);
                return;
            }

            // A staged slab this one supersedes was never published, so nothing can be reading it.
            RHI::Core::Retire(GPendingSlab);

            // Read AFTER the upload is queued, so it names that upload's batch or a later one.
            GPendingSlab     = NewSlab;
            GPendingCapacity = NewCapacity;
            GPendingBatch    = RHI::Upload::BatchForQueuedOps();

            // Nothing is published yet, so the first slab waits rather than hand out slots into undefined memory.
            if (GSlab == 0)
            {
                RHI::FlushUploadsAndWait();
                PublishPendingLocked();
            }
        }

        /** Creates the slab and burns slot 0 on the null header. Caller holds GMutex. */
        void EnsureInitializedLocked()
        {
            if (GSlab != 0)
            {
                return;
            }

            // ReserveLocked fills the new allocation with null headers, and those name it.
            EnsureNullGeometryLocked();
            if (GNullGeometry == 0)
            {
                // Every null header would name address 0, and a clamping reader dereferences it. No slab is safer.
                return;
            }

            ReserveLocked(kInitialCapacity);
            if (GSlab == 0)
            {
                return;
            }

            // Handing out slots from 1 is the whole of what reserves the null slot, so nothing overwrites it.
            GNextSlot = kNullSlot + 1u;
        }
    }

    uint32 Acquire()
    {
        FScopeLock Lock(GMutex);
        EnsureInitializedLocked();

        if (GSlab == 0)
        {
            return kNullSlot;
        }

        PublishPendingLocked();

        if (GFreeHead < GFreeList.size())
        {
            const uint32 Recycled = GFreeList[GFreeHead++];

            // Compact once the dead prefix outweighs the live tail, so the FIFO does not grow forever.
            if (GFreeHead > GFreeList.size() / 2)
            {
                GFreeList.erase(GFreeList.begin(), GFreeList.begin() + (SIZE_T)GFreeHead);
                GFreeHead = 0;
            }
            return Recycled;
        }

        const uint32 Slot = GNextSlot;

        // Slack, so the staged slab has frames to land in before the published one is actually full.
        ReserveLocked(Slot + 1u + kGrowSlack);

        if (Slot >= GCapacity)
        {
            // With nothing staged the wait is a GPU stall that cannot change the answer.
            if (GPendingSlab != 0)
            {
                RHI::FlushUploadsAndWait();
                PublishPendingLocked();
            }

            if (Slot >= GCapacity)
            {
                // Growth failed; the caller gets the null header rather than a slot outside the allocation.
                return kNullSlot;
            }
        }

        ++GNextSlot;
        return Slot;
    }

    namespace
    {
        /** Caller holds GMutex. Skips a slot rewritten since the retire was requested. */
        void WriteNullLocked(uint32 Slot, uint32 ExpectedVersion)
        {
            if (Slot == kNullSlot || Slot >= GCapacity || GSlab == 0)
            {
                return;
            }

            if (GSlotVersion[Slot] != ExpectedVersion)
            {
                return;   // republished in the meantime; the header there is not the one being retired
            }

            const FMeshletHeaderGPU Null = MakeNullHeader();
            GMirror[Slot] = Null;
            RHI::UploadBuffer(GSlab + (uint64)Slot * sizeof(FMeshletHeaderGPU), &Null, sizeof(FMeshletHeaderGPU));

            // The staged slab's mirror copy was queued before this write, so it has to be repeated there.
            if (GPendingSlab != 0 && Slot < GPendingCapacity)
            {
                RHI::UploadBuffer(GPendingSlab + (uint64)Slot * sizeof(FMeshletHeaderGPU), &Null, sizeof(FMeshletHeaderGPU));
            }
        }

        /** Caller holds GMutex. */
        uint32 VersionOfLocked(uint32 Slot)
        {
            return Slot < GSlotVersion.size() ? GSlotVersion[Slot] : 0u;
        }
    }

    void Reset(uint32 Slot)
    {
        if (Slot == kNullSlot)
        {
            return;
        }

        uint32 Version = 0;
        {
            FScopeLock Lock(GMutex);
            Version = VersionOfLocked(Slot);
        }

        // The geometry stays alive until the fence, so the two have to happen at the same instant.
        RHI::Core::RetireCallback([Slot, Version]
        {
            FScopeLock Lock(GMutex);
            WriteNullLocked(Slot, Version);
        });
    }

    void Release(uint32 Slot)
    {
        if (Slot == kNullSlot)
        {
            return;
        }

        uint32 Version = 0;
        {
            FScopeLock Lock(GMutex);
            Version = VersionOfLocked(Slot);
        }

        // A slot must not be reacquired while a recorded frame can still name it.
        RHI::Core::RetireCallback([Slot, Version]
        {
            FScopeLock Lock(GMutex);
            WriteNullLocked(Slot, Version);

            if (Slot < GCapacity && GSlab != 0)
            {
                GFreeList.push_back(Slot);
            }
        });
    }

    void Write(uint32 Slot, const FMeshletHeaderGPU& Header)
    {
        if (Slot == kNullSlot)
        {
            return;
        }

        // The invariant every consumer relies on, a non-zero count promises its arrays exist.
        if (Header.MeshletCount != 0
            && (Header.MeshletsAddress == 0 || Header.SpheresAddress == 0 || Header.VerticesAddress == 0
                || Header.TrianglesAddress == 0 || Header.ConesAddress == 0))
        {
            LOG_ERROR("MeshletHeaderSlab: refusing header for slot {}: count {} over null arrays "
                      "(meshlets {}, spheres {}, vertices {}, triangles {}, cones {}). Slot left describing no geometry.",
                      Slot, Header.MeshletCount, Header.MeshletsAddress, Header.SpheresAddress,
                      Header.VerticesAddress, Header.TrianglesAddress, Header.ConesAddress);

            FScopeLock RejectLock(GMutex);
            if (Slot < GCapacity && GSlab != 0)
            {
                // Through the shared writer, so the staged slab is blanked too and a publish cannot resurrect this.
                ++GSlotVersion[Slot];
                WriteNullLocked(Slot, GSlotVersion[Slot]);
            }
            return;
        }

        FScopeLock Lock(GMutex);
        if (Slot >= GCapacity || GSlab == 0)
        {
            return;
        }

        // Bumped BEFORE the upload, so a queued reset recognizes that the contents have changed.
        ++GSlotVersion[Slot];

        GMirror[Slot] = Header;
        RHI::UploadBuffer(GSlab + (uint64)Slot * sizeof(FMeshletHeaderGPU), &Header, sizeof(FMeshletHeaderGPU));

        // The staged slab's mirror copy was queued before this write, so it has to be repeated there.
        if (GPendingSlab != 0 && Slot < GPendingCapacity)
        {
            RHI::UploadBuffer(GPendingSlab + (uint64)Slot * sizeof(FMeshletHeaderGPU), &Header, sizeof(FMeshletHeaderGPU));
        }
    }

    bool IsSkinnedSlot(uint32 Slot)
    {
        if (Slot == kNullSlot)
        {
            return false;
        }

        FScopeLock Lock(GMutex);
        return Slot < (uint32)GMirror.size() && GMirror[Slot].BonePalettesAddress != 0;
    }

    RHI::GPUPtr GetAddress()
    {
        FScopeLock Lock(GMutex);
        EnsureInitializedLocked();

        // Once per frame per view, which is the natural place to notice a staged slab has landed.
        PublishPendingLocked();
        return GSlab;
    }

    void Shutdown()
    {
        RHI::GPUPtr Slab    = 0;
        RHI::GPUPtr Pending = 0;
        RHI::GPUPtr Null    = 0;
        {
            FScopeLock Lock(GMutex);

            Slab             = GSlab;
            Pending          = GPendingSlab;
            Null             = GNullGeometry;
            GSlab            = 0;
            GPendingSlab     = 0;
            GPendingCapacity = 0;
            GPendingBatch    = 0;
            GNullGeometry    = 0;
            GCapacity        = 0;
            GNextSlot     = kNullSlot + 1u;
            GMirror.clear();
            GSlotVersion.clear();
            GFreeList.clear();
            GFreeHead = 0;
        }

        // Retiring under GMutex would order the two locks opposite to how Shutdown takes them.
        RHI::Core::Retire(Slab);
        RHI::Core::Retire(Pending);
        RHI::Core::Retire(Null);
    }
}
