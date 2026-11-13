#pragma once

#include "World/ECS/Registry.h"

#include "Core/Math/Math.h"
#include "Core/Serialization/NetQuantize.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Components/RepTransformComponent.h"

namespace Lumina
{
    struct SDefaultWorldSettings;

    // Distance LOD tier for a replicated entity relative to a client's viewpoint (XZ ground distance).
    enum class ENetLODTier : uint8
    {
        Near = 0, // full rate + precision
        Mid  = 1,
        Far  = 2,
        Cull = 3, // not relevant
    };

    // Per-record flags in FNetExtract.
    enum ENetRecordFlags : uint8
    {
        NETREC_None           = 0,
        NETREC_AlwaysRelevant = 1 << 0,
        NETREC_Dynamic        = 1 << 1, // dynamic (runtime-spawned) NetGUID -> needs per-client spawn/despawn
        NETREC_Movement       = 1 << 4, // replicates a streamed transform (has FRepTransform); else spawn/relevancy only
        NETREC_HasPitch       = 1 << 5, // a character whose view pitch rides with its pose
    };

    inline float ClampViewPitch(float Degrees)
    {
        return Math::Clamp(Degrees, -90.0f, 90.0f);
    }

    // A view pitch in degrees to and from the signed byte the wire carries.
    inline int8 QuantizeViewPitch(float Degrees)
    {
        const float Clamped = ClampViewPitch(Degrees);
        return static_cast<int8>(Clamped * (127.0f / 90.0f) + (Clamped >= 0.0f ? 0.5f : -0.5f));
    }

    inline float ViewPitchDegrees(int8 Quantized)
    {
        return static_cast<float>(Quantized) * (90.0f / 127.0f);
    }

    // Per-thread scratch for the parallel extract (append-local, merged serially). Reused across ticks.
    struct FNetExtractThread
    {
        TVector<uint32>                      Guid;
        TVector<FVector3>                    Pos;      // LOCAL pose -> the wire
        TVector<FVector3>                    WorldPos; // WORLD pose -> grid + relevancy (correct for attached)
        TVector<NetQuantize::FQuantizedQuat> Rot;
        TVector<NetQuantize::FQuantizedVector> Scale;
        TVector<uint32>                      OwnerConn;
        TVector<uint8>                       Flags;
        TVector<int8>                        Pitch;
        TVector<FNetChangeSeq>               Seq;

        void Reset()
        {
            Guid.Reset(); Pos.Reset(); WorldPos.Reset(); Rot.Reset(); Scale.Reset(); OwnerConn.Reset(); Flags.Reset(); Pitch.Reset(); Seq.Reset();
        }
    };

    // Per-tick flat SoA snapshot of all movement-replicating entities. Built once on the server and reused for
    // every client's relevancy gather. The array index is the dense "record index".
    struct FNetExtract
    {
        TVector<uint32>                        Guid;
        TVector<FVector3>                      Pos;      // LOCAL pose -> the wire (relative for attached children)
        TVector<FVector3>                      WorldPos; // WORLD pose -> grid cell + relevancy distance/viewpoint
        TVector<NetQuantize::FQuantizedQuat>   Rot;
        TVector<NetQuantize::FQuantizedVector> Scale;
        TVector<uint32>                        OwnerConn; // 0 = server/unowned
        TVector<uint8>                         Flags;     // ENetRecordFlags
        TVector<int8>                          Pitch;     // QuantizeViewPitch, meaningful with NETREC_HasPitch
        TVector<FNetChangeSeq>                 Seq;       // meaningful with NETREC_Movement

        TVector<FNetExtractThread>             Threads;   // reused per-thread scratch

        void Reset()
        {
            Guid.Reset(); Pos.Reset(); WorldPos.Reset(); Rot.Reset(); Scale.Reset(); OwnerConn.Reset(); Flags.Reset(); Pitch.Reset(); Seq.Reset();
        }
        uint32 Num() const { return static_cast<uint32>(Guid.Num()); }
    };

    // Uniform 2D grid over the XZ plane, CSR (counting-sort) layout. Cell c owns SortedRecords in
    // [CellStart[c], CellStart[c+1]). Reused across ticks.
    struct FNetGrid
    {
        static constexpr int32 MaxDim = 512;

        FVector3        Origin;            // world min corner; X and Z used
        float           CellSize = 64.0f;
        int32           DimX     = 0;
        int32           DimZ     = 0;
        TVector<int32>    CellStart;       // size NumCells()+1
        TVector<uint32>   SortedRecords;   // size = record count (extract record index, in cell order)
        // Parallel to SortedRecords, duplicated in cell order so the per-client AOI distance/cull check reads
        // contiguously instead of gathering scattered Extract.WorldPos[Rec]/Flags[Rec] (the hot cache miss).
        TVector<FVector3> SortedWorldPos;
        TVector<uint8>    SortedFlags;
        TVector<uint32>   SortedGuid;
        TVector<int32>    Cursor;          // scatter scratch, size NumCells()

        int32 NumCells() const { return DimX * DimZ; }

        // Clamped cell coords for a world position.
        void CellCoords(const FVector3& P, int32& OutX, int32& OutZ) const
        {
            int32 cx = static_cast<int32>((P.x - Origin.x) / CellSize);
            int32 cz = static_cast<int32>((P.z - Origin.z) / CellSize);
            OutX = cx < 0 ? 0 : (cx >= DimX ? DimX - 1 : cx);
            OutZ = cz < 0 ? 0 : (cz >= DimZ ? DimZ - 1 : cz);
        }
        int32 CellIndex(int32 cx, int32 cz) const { return cz * DimX + cx; }

        // World min corner of a cell (for cell-relative position encoding, Stage 2).
        FVector3 CellOrigin(int32 cx, int32 cz) const
        {
            return FVector3(Origin.x + cx * CellSize, Origin.y, Origin.z + cz * CellSize);
        }
    };

    // Per-(client, entity) relevancy + send-schedule state.
    struct FRelevantEntry
    {
        ENetLODTier Tier             = ENetLODTier::Near;
        float       TimeSinceSent    = 1.0e9f; // large -> first eligible send (Stage 2 rate LOD)
        float       TimeOutOfAOI     = 0.0f;   // grace accumulator once outside the leave radius
        float       TimeHeld         = 0.0f;   // how long the client has held it, so a crowd cap never drops a fresh arrival
        uint64      RelevantTick     = 0;      // == FNetWorldState::RelevancyTick when seen relevant this tick
        bool        bDynamic         = false;  // runtime-spawned -> per-client spawn/despawn applies
        bool        bBaselinePending = false;  // dynamic entity spawned this tick; hold its transform one tick (spawn carried the pose)
        bool        bNeedsBaseline   = false;  // stable entity newly relevant -> send its current pose once even if unchanged
        FNetChangeSeq SentSeq;                 // the entity's change sequence as of the last transform sent to this client
    };

    // Per-connection relevancy view. Keyed by NetGUID. Never touches the global GuidTable.
    struct FNetClientView
    {
        THashMap<uint32 /*guid*/, FRelevantEntry> Relevant;
        bool bForceBaseline = true; // re-send every relevant pose (set on join / map travel)

        // Clients are serviced at the send rate on staggered ticks, so each tick carries only a share of them.
        double NextServiceTime = -1.0;
        double LastServiceTime = -1.0;

        // Property and ownership messages held until this client's next service.
        TVector<uint8> PendingReliable;
    };

    namespace NetGraph
    {
        // Build the SoA extract (parallel) from movement-replicating entities, plus a connId->record map for
        // O(1) viewpoint lookup. Server-owned entities use their STransformComponent local pose; client-owned
        // relay the newest received ring sample. Also bumps each entity's FRepTransform change sequence when it moved.
        // A connection owning several entities gets the NETREC_HasPitch one as its viewpoint anchor, or else the lowest NetGUID, never extract order.
        NETWORKINGRUNTIME_API void BuildExtract(ECS::FRegistry& Registry, FNetExtract& OutExtract, THashMap<uint32, uint32>& OutOwnerToRecord);

        // Whether a serviced client is owed a transform record for Entry, and whether scale must ride along.
        NETWORKINGRUNTIME_API bool IsTransformDue(const FRelevantEntry& Entry, const FNetChangeSeq& Current, bool bForceBaseline,
            const SDefaultWorldSettings& Settings, bool& bOutScale);

        // Build the CSR grid from the extract (counting sort) using the settings' world extent + cell size.
        NETWORKINGRUNTIME_API void BuildGrid(const FNetExtract& Extract, const SDefaultWorldSettings& Settings, FNetGrid& OutGrid);

        // Assign a tier from XZ distance to the viewpoint, using the settings' tier distances + leave radius.
        NETWORKINGRUNTIME_API ENetLODTier TierForDistanceSq(float DistSq, const SDefaultWorldSettings& Settings);
    }
}
