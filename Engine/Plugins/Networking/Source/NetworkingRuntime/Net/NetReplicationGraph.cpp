#include "RuntimePCH.h"
#include "NetReplicationGraph.h"
#include "World/ECS/Registry.h"

#include "Core/Profiler/Profile.h"
#include "TaskSystem/TaskSystem.h"
#include "Components/NetworkComponent.h"
#include "Components/RepTransformComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Net/NetGUID.h"
#include "Net/NetReplication.h"
#include "World/Subsystems/WorldSettings.h"

namespace Lumina::NetGraph
{
    void BuildExtract(ECS::FRegistry& Registry, FNetExtract& Out, THashMap<uint32, uint32>& OutOwnerToRecord)
    {
        LUMINA_PROFILE_SCOPE();
        Out.Reset();
        OutOwnerToRecord.Reset();

        // Every replicating entity needs a record whether or not it streams a transform.
        auto View   = Registry.View<ECS::Read<SNetworkComponent>>();
        const TSpan<const ECS::FEntity> DenseSlots = View.GetDenseEntities();
        const uint32 N = static_cast<uint32>(DenseSlots.Num());

        const uint32 NumThreads = Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 1u;
        if (static_cast<uint32>(Out.Threads.Num()) < NumThreads)
        {
            Out.Threads.SetNum(NumThreads);
        }
        for (FNetExtractThread& T : Out.Threads)
        {
            T.Reset();
        }

        auto NetStorage   = Registry.GetStorage<SNetworkComponent>();
        auto RepStorage   = Registry.GetStorage<FRepTransform>();
        // Every entity has a transform, so joining the universal pool would be a regression.
        auto TformStorage = Registry.GetStorage<STransformComponent>();
        auto ControllerStorage = Registry.GetStorage<SCharacterControllerComponent>();

        if (N > 0)
        {
            LUMINA_PROFILE_SECTION("Extract::Parallel");
            Jobs::ParallelFor(N, [&](const Jobs::FParallelRange& Range)
            {
                FNetExtractThread& L = Out.Threads[Range.Thread];
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    const ECS::FEntity E = DenseSlots.GetData()[i];
                    SNetworkComponent& Net = NetStorage.Get(E);

                    // A static-but-stateful or purely logical entity must still spawn and replicate.
                    if (!Net.bReplicates || !Net.bNetLoadOnClient)
                    {
                        continue;
                    }

                    const bool bMovement = Net.bReplicatesMovement && RepStorage.Contains(E);

                    uint8 Flags = NETREC_None;
                    if (Net.bAlwaysRelevant)                       { Flags |= NETREC_AlwaysRelevant; }
                    if (Net.NetGUID.Value >= NetGUID_DynamicStart) { Flags |= NETREC_Dynamic; }

                    // With no transform it cannot be placed in the AOI grid, so it must be always relevant.
                    if (!TformStorage.Contains(E))
                    {
                        if (!Net.bAlwaysRelevant)
                        {
                            continue;
                        }
                        L.Guid.Add(Net.NetGUID.Value);
                        L.Pos.Emplace(0.0f);
                        L.WorldPos.Emplace(0.0f);
                        L.Rot.Add(NetQuantize::FQuantizedQuat::FromQuat(FQuat::Identity()));
                        L.Scale.Add(NetQuantize::FQuantizedVector::FromVector(FVector3(1.0f), NetQuantize::ScaleQuantum));
                        L.OwnerConn.Add(Net.OwningConnectionId);
                        L.Flags.Add(Flags);
                        L.Pitch.Add(0);
                        continue;
                    }

                    STransformComponent& T = TformStorage.Get(E);

                    // Must match the parent-NetGUID gate in WriteEntityComponents.
                    const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(E);
                    const bool bNetParent = Parent != ECS::NullEntity && Net::ParentReplicates(Registry, Parent);
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
                        // A flat entity never writes WorldTransform, so the raw field would freeze at its spawn pose.
                        const FTransform& World = T.GetWorldTransformCached();
                        Pos   = World.GetLocation();
                        Rot   = World.GetRotation();
                        Scale = World.GetScale();
                    }

                    const NetQuantize::FQuantizedVector QPos   = NetQuantize::FQuantizedVector::FromVector(Pos);
                    const NetQuantize::FQuantizedQuat   QRot   = NetQuantize::FQuantizedQuat::FromQuat(Rot);
                    const NetQuantize::FQuantizedVector QScale = NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum);

                    // Other players see where a character looks, so its pitch moves the record like its pose does.
                    int8 Pitch = 0;
                    if (ControllerStorage.Contains(E))
                    {
                        Pitch = QuantizeViewPitch(ControllerStorage.Get(E).LookInput.y);
                        Flags |= NETREC_HasPitch;
                    }

                    // A non-movement entity's pose only ever rides the spawn baseline, so it skips this gate.
                    if (bMovement)
                    {
                        FRepTransform& Rep = RepStorage.Get(E);
                        if (!Rep.bSendCacheValid || QPos != Rep.LastSentPos || QRot != Rep.LastSentRot) { Flags |= NETREC_Changed; }
                        if ((Flags & NETREC_HasPitch) && Pitch != Rep.LastSentPitch)                     { Flags |= NETREC_Changed; }
                        Rep.LastSentPitch   = Pitch;
                        if (!Rep.bSendCacheValid || QScale != Rep.LastSentScale)                        { Flags |= NETREC_ScaleChanged; }
                        Rep.LastSentPos     = QPos;
                        Rep.LastSentRot     = QRot;
                        Rep.LastSentScale   = QScale;
                        Rep.bSendCacheValid = true;
                        Flags |= NETREC_Movement;
                    }

                    L.Guid.Add(Net.NetGUID.Value);
                    L.Pos.Add(Pos);                       // LOCAL -> wire
                    L.WorldPos.Add(T.GetWorldLocationCached()); // WORLD -> grid/relevancy (== local for roots)
                    L.Rot.Add(QRot);
                    L.Scale.Add(QScale);
                    L.OwnerConn.Add(Net.OwningConnectionId);
                    L.Flags.Add(Flags);
                    L.Pitch.Add(Pitch);
                }
            }, 256);
        }

        // The cost is the BuildExtract zone minus its parallel child.
        uint32 Total = 0;
        for (uint32 t = 0; t < NumThreads; ++t) { Total += static_cast<uint32>(Out.Threads[t].Guid.Num()); }
        Out.Guid.Reserve(Total); 
        Out.Pos.Reserve(Total); 
        Out.WorldPos.Reserve(Total); 
        Out.Rot.Reserve(Total);
        Out.Scale.Reserve(Total);
        Out.OwnerConn.Reserve(Total);
        Out.Flags.Reserve(Total);
        Out.Pitch.Reserve(Total);

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FNetExtractThread& L = Out.Threads[t];
            const uint32 LN = static_cast<uint32>(L.Guid.Num());
            for (uint32 i = 0; i < LN; ++i)
            {
                const uint32 Rec = static_cast<uint32>(Out.Guid.Num());
                Out.Guid.Add(L.Guid[i]);
                Out.Pos.Add(L.Pos[i]);
                Out.WorldPos.Add(L.WorldPos[i]);
                Out.Rot.Add(L.Rot[i]);
                Out.Scale.Add(L.Scale[i]);
                Out.OwnerConn.Add(L.OwnerConn[i]);
                Out.Flags.Add(L.Flags[i]);
                Out.Pitch.Add(L.Pitch[i]);
                if (L.OwnerConn[i] != 0)
                {
                    OutOwnerToRecord[L.OwnerConn[i]] = Rec; // a client owns one pawn; last wins
                }
            }
        }

        // The total replicating-entity count, the dominant driver of every per-tick replication cost.
        LUMINA_PROFILE_VALUE("Net/ExtractRecords", Out.Num());
    }

    void BuildGrid(const FNetExtract& Extract, const SDefaultWorldSettings& Settings, FNetGrid& Grid)
    {
        LUMINA_PROFILE_SCOPE();
        const float Half = Settings.WorldHalfExtent;
        float Cell = Math::Max(Settings.GridCellSize, 1.0f);
        const float Dim = Math::Ceil((2.0f * Half) / Cell);
        if (Dim > static_cast<float>(FNetGrid::MaxDim))
        {
            Cell = (2.0f * Half) / static_cast<float>(FNetGrid::MaxDim);
            LOG_WARN_ONCE("Net relevancy grid would need {} cells per axis, widening cells to {} to stay at {}", Dim, Cell, FNetGrid::MaxDim);
        }
        Grid.Origin   = FVector3(-Half, 0.0f, -Half);
        Grid.CellSize = Cell;
        Grid.DimX     = Math::Max(static_cast<int32>(Math::Min(Dim, static_cast<float>(FNetGrid::MaxDim))), 1);
        Grid.DimZ     = Grid.DimX;

        const int32  NumCells = Grid.NumCells();
        const uint32 N        = Extract.Num();

        // A counting sort, so count, prefix-sum, then scatter.
        Grid.CellStart.Init(0, NumCells + 1);
        Grid.SortedRecords.SetNum(N);
        Grid.SortedWorldPos.SetNum(N);
        Grid.SortedFlags.SetNum(N);
        Grid.SortedGuid.SetNum(N);

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
        TVector<int32>& Cursor = Grid.Cursor;
        Cursor.SetNum(NumCells);
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
