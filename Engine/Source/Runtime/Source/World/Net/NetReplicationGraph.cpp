#include "RuntimePCH.h"
#include "NetReplicationGraph.h"

#include "Core/Profiler/Profile.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/NetworkComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/RepTransformComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Net/NetGUID.h"
#include "World/Net/NetReplication.h"
#include "World/Subsystems/WorldSettings.h"
#include <algorithm>
#include <cmath>

namespace Lumina::NetGraph
{
    void BuildExtract(entt::registry& Registry, FNetExtract& Out, THashMap<uint32, uint32>& OutOwnerToRecord)
    {
        LUMINA_PROFILE_SCOPE();
        Out.Reset();
        OutOwnerToRecord.clear();

        // Lead on SNetworkComponent (not FRepTransform): every replicating entity needs a relevancy/spawn
        // record, whether or not it streams a transform. Movement is a per-record axis (NETREC_Movement).
        auto View   = Registry.view<SNetworkComponent>();
        auto Handle = View.handle();
        const uint32 N = static_cast<uint32>(Handle->size());

        const uint32 NumThreads = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 1u;
        if (static_cast<uint32>(Out.Threads.size()) < NumThreads)
        {
            Out.Threads.resize(NumThreads);
        }
        for (FNetExtractThread& T : Out.Threads)
        {
            T.Reset();
        }

        auto&& NetStorage   = Registry.storage<SNetworkComponent>();
        auto&& RepStorage   = Registry.storage<FRepTransform>();
        // Every entity has a transform, so it's NOT in the lead view (joining the universal pool is a
        // regression); fetch it from its storage by entity in the hot loop.
        auto&& TformStorage = Registry.storage<STransformComponent>();

        if (N > 0)
        {
            LUMINA_PROFILE_SECTION("Extract::Parallel");
            Task::ParallelFor(N, [&](const Task::FParallelRange& Range)
            {
                FNetExtractThread& L = Out.Threads[Range.Thread];
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    const entt::entity E = (*Handle)[i];
                    SNetworkComponent& Net = NetStorage.get(E);

                    // Existence gate: anything that replicates to clients gets a record. Movement is a separate
                    // axis below -- a static-but-stateful or purely-logical entity must still spawn + replicate.
                    if (!Net.bReplicates || !Net.bNetLoadOnClient)
                    {
                        continue;
                    }

                    const bool bMovement = Net.bReplicatesMovement && RepStorage.contains(E);

                    uint8 Flags = NETREC_None;
                    if (Net.bAlwaysRelevant)                       { Flags |= NETREC_AlwaysRelevant; }
                    if (Net.NetGUID.Value >= NetGUID_DynamicStart) { Flags |= NETREC_Dynamic; }

                    // No transform -> can't be placed in the AOI grid, so it must be AlwaysRelevant to replicate
                    // at all (a purely logical entity: game/match/score state). Carries no pose.
                    if (!TformStorage.contains(E))
                    {
                        if (!Net.bAlwaysRelevant)
                        {
                            continue;
                        }
                        L.Guid.push_back(Net.NetGUID.Value);
                        L.Pos.push_back(FVector3(0.0f));
                        L.WorldPos.push_back(FVector3(0.0f));
                        L.Rot.push_back(NetQuantize::FQuantizedQuat::FromQuat(FQuat::Identity()));
                        L.Scale.push_back(NetQuantize::FQuantizedVector::FromVector(FVector3(1.0f), NetQuantize::ScaleQuantum));
                        L.OwnerConn.push_back(Net.OwningConnectionId);
                        L.Flags.push_back(Flags);
                        continue;
                    }

                    STransformComponent& T = TformStorage.get(E);

                    // Send LOCAL (relative) only when the parent replicates to clients -- then the client
                    // reparents and composes against the parent's world (rigid). For no parent, or a parent the
                    // client won't have, send WORLD so the (unparented-on-client) entity lands at the right pose.
                    // Must match WriteEntityComponents' parent-NetGUID gate (Net::ParentReplicates).
                    const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(E);
                    const bool bNetParent = (Rel != nullptr && Rel->Parent != entt::null && Net::ParentReplicates(Registry, Rel->Parent));
                    FVector3 Pos;
                    FQuat    Rot;
                    FVector3 Scale;
                    if (bNetParent)
                    {
                        Pos   = T.GetLocalLocation();
                        Rot   = T.GetLocalRotation();
                        Scale = T.GetLocalScale();
                    }
                    else
                    {
                        Pos   = T.WorldTransform.GetLocation();
                        Rot   = T.WorldTransform.GetRotation();
                        Scale = T.WorldTransform.GetScale();
                    }

                    const NetQuantize::FQuantizedVector QPos   = NetQuantize::FQuantizedVector::FromVector(Pos);
                    const NetQuantize::FQuantizedQuat   QRot   = NetQuantize::FQuantizedQuat::FromQuat(Rot);
                    const NetQuantize::FQuantizedVector QScale = NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum);

                    // Movement entities stream their transform: run the global "did it move at all" change gate
                    // vs the server-wide FRepTransform baseline (per-entity write -> parallel-safe). Non-movement
                    // entities skip this -- their pose only ever rides the spawn baseline.
                    if (bMovement)
                    {
                        FRepTransform& Rep = RepStorage.get(E);
                        if (!Rep.bSendCacheValid || QPos != Rep.LastSentPos || QRot != Rep.LastSentRot) { Flags |= NETREC_Changed; }
                        if (!Rep.bSendCacheValid || QScale != Rep.LastSentScale)                        { Flags |= NETREC_ScaleChanged; }
                        Rep.LastSentPos     = QPos;
                        Rep.LastSentRot     = QRot;
                        Rep.LastSentScale   = QScale;
                        Rep.bSendCacheValid = true;
                        Flags |= NETREC_Movement;
                    }

                    L.Guid.push_back(Net.NetGUID.Value);
                    L.Pos.push_back(Pos);                       // LOCAL -> wire
                    L.WorldPos.push_back(T.GetWorldLocationCached()); // WORLD -> grid/relevancy (== local for roots)
                    L.Rot.push_back(QRot);
                    L.Scale.push_back(QScale);
                    L.OwnerConn.push_back(Net.OwningConnectionId);
                    L.Flags.push_back(Flags);
                }
            }, 256);
        }

        // Serial merge into the dense SoA + build the owner->record map (cost = the BuildExtract zone minus
        // the Extract::Parallel child).
        uint32 Total = 0;
        for (uint32 t = 0; t < NumThreads; ++t) { Total += static_cast<uint32>(Out.Threads[t].Guid.size()); }
        Out.Guid.reserve(Total); 
        Out.Pos.reserve(Total); 
        Out.WorldPos.reserve(Total); 
        Out.Rot.reserve(Total);
        Out.Scale.reserve(Total);
        Out.OwnerConn.reserve(Total);
        Out.Flags.reserve(Total);

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FNetExtractThread& L = Out.Threads[t];
            const uint32 LN = static_cast<uint32>(L.Guid.size());
            for (uint32 i = 0; i < LN; ++i)
            {
                const uint32 Rec = static_cast<uint32>(Out.Guid.size());
                Out.Guid.push_back(L.Guid[i]);
                Out.Pos.push_back(L.Pos[i]);
                Out.WorldPos.push_back(L.WorldPos[i]);
                Out.Rot.push_back(L.Rot[i]);
                Out.Scale.push_back(L.Scale[i]);
                Out.OwnerConn.push_back(L.OwnerConn[i]);
                Out.Flags.push_back(L.Flags[i]);
                if (L.OwnerConn[i] != 0)
                {
                    OutOwnerToRecord[L.OwnerConn[i]] = Rec; // a client owns one pawn; last wins
                }
            }
        }

        // Total replicating-entity count -- the dominant driver of every per-tick replication cost.
        LUMINA_PROFILE_VALUE("Net/ExtractRecords", static_cast<int64>(Out.Num()));
    }

    void BuildGrid(const FNetExtract& Extract, const SDefaultWorldSettings& Settings, FNetGrid& Grid)
    {
        LUMINA_PROFILE_SCOPE();
        const float Half = Settings.WorldHalfExtent;
        const float Cell = (Settings.GridCellSize > 1.0f) ? Settings.GridCellSize : 1.0f;
        Grid.Origin   = FVector3(-Half, 0.0f, -Half);
        Grid.CellSize = Cell;
        Grid.DimX     = static_cast<int32>(std::ceil((2.0f * Half) / Cell));
        Grid.DimX = std::max(Grid.DimX, 1);
        Grid.DimZ     = Grid.DimX;

        const int32  NumCells = Grid.NumCells();
        const uint32 N        = Extract.Num();

        // Counting sort: count -> prefix-sum -> scatter.
        Grid.CellStart.assign(NumCells + 1, 0);
        Grid.SortedRecords.resize(N);
        Grid.SortedWorldPos.resize(N);
        Grid.SortedFlags.resize(N);
        Grid.SortedGuid.resize(N);

        for (uint32 i = 0; i < N; ++i)
        {
            int32 cx, cz;
            Grid.CellCoords(Extract.WorldPos[i], cx, cz);
            ++Grid.CellStart[Grid.CellIndex(cx, cz) + 1];
        }
        for (int32 c = 0; c < NumCells; ++c)
        {
            Grid.CellStart[c + 1] += Grid.CellStart[c];
        }

        // Scatter with a per-cell cursor.
        TVector<int32> Cursor;
        Cursor.resize(NumCells);
        for (int32 c = 0; c < NumCells; ++c) { Cursor[c] = Grid.CellStart[c]; }
        for (uint32 i = 0; i < N; ++i)
        {
            int32 cx, cz;
            Grid.CellCoords(Extract.WorldPos[i], cx, cz);
            const int32 c   = Grid.CellIndex(cx, cz);
            const int32 Dst = Cursor[c]++;
            Grid.SortedRecords[Dst]  = i;
            Grid.SortedWorldPos[Dst] = Extract.WorldPos[i]; // duplicate in cell order for the hot AOI scan
            Grid.SortedFlags[Dst]    = Extract.Flags[i];
            Grid.SortedGuid[Dst]     = Extract.Guid[i];
        }
    }

    ENetLODTier TierForDistanceSq(float DistSq, const SDefaultWorldSettings& Settings)
    {
        const float Leave = Settings.AOILeaveRadius;
        if (DistSq > Leave * Leave) { return ENetLODTier::Cull; }
        const float Near = Settings.TierNearDistance;
        if (DistSq <= Near * Near) { return ENetLODTier::Near; }
        const float Mid = Settings.TierMidDistance;
        if (DistSq <= Mid * Mid) { return ENetLODTier::Mid; }
        return ENetLODTier::Far;
    }
}
