#include "RuntimePCH.h"
#include "Sequence.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "World/World.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    entt::entity FSequenceEvalContext::Resolve(int32 BindingIndex) const
    {
        if (BoundEntities == nullptr || BindingIndex < 0 || BindingIndex >= (int32)BoundEntities->size())
        {
            return entt::null;
        }

        return (*BoundEntities)[BindingIndex];
    }

    FVector3 SSequenceVectorCurve::Evaluate(float Time) const
    {
        return FVector3(X.Evaluate(Time), Y.Evaluate(Time), Z.Evaluate(Time));
    }

    void CSequenceTrack_Transform::Evaluate(const FSequenceEvalContext& Context) const
    {
        const entt::entity Entity = Context.Resolve(BindingIndex);
        if (Entity == entt::null || Context.World == nullptr || !Context.World->IsValidEntity(Entity))
        {
            return;
        }

        STransformComponent* Transform = Context.World->TryGetComponent<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        // Each channel group is opt-in, so a track can drive rotation while gameplay keeps owning position.
        if (Location.bEnabled)
        {
            Transform->SetLocation(Location.Evaluate(Context.Time));
        }

        if (Rotation.bEnabled)
        {
            Transform->SetRotationFromEuler(Rotation.Evaluate(Context.Time));
        }

        if (Scale.bEnabled)
        {
            Transform->SetScale(Scale.Evaluate(Context.Time));
        }
    }

    int32 CSequenceTrack_CameraCut::FindCutAt(float Time) const
    {
        for (int32 i = 0; i < (int32)Cuts.size(); ++i)
        {
            const SSequenceCameraCut& Cut = Cuts[i];
            if (Time >= Cut.StartTime && Time < Cut.EndTime)
            {
                return i;
            }
        }

        return INDEX_NONE;
    }

    void CSequenceTrack_CameraCut::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.World == nullptr)
        {
            return;
        }

        const int32 CutIndex = FindCutAt(Context.Time);
        if (CutIndex == INDEX_NONE)
        {
            return;
        }

        const entt::entity Camera = Context.Resolve(Cuts[CutIndex].BindingIndex);
        if (Camera == entt::null || !Context.World->IsValidEntity(Camera))
        {
            return;
        }

        // SetActiveCamera is a switch, so re-issuing it each frame would stomp anything that took the camera.
        if (Context.bJumped || FindCutAt(Context.PreviousTime) != CutIndex)
        {
            Context.World->SetActiveCamera(Camera);
        }
    }

    void FSequenceInstance::Bind(const CSequence* Sequence, CWorld* World)
    {
        Release(World, true);

        if (Sequence == nullptr || World == nullptr)
        {
            return;
        }

        BoundEntities.assign(Sequence->Bindings.size(), entt::null);

        for (int32 i = 0; i < (int32)Sequence->Bindings.size(); ++i)
        {
            const SSequenceBinding& Binding = Sequence->Bindings[i];

            if (Binding.Kind == ESequenceBindingKind::Spawn)
            {
                if (!Binding.SpawnPrefab.IsValid())
                {
                    continue;
                }

                const entt::entity Spawned = Binding.SpawnPrefab->Instantiate(World);
                if (Spawned != entt::null)
                {
                    BoundEntities[i] = Spawned;
                    SpawnedEntities.push_back(Spawned);
                }
                continue;
            }

            auto View = World->View<SNameComponent>();
            for (entt::entity Entity : View)
            {
                if (View.get<SNameComponent>(Entity).Name == Binding.Name)
                {
                    BoundEntities[i] = Entity;
                    break;
                }
            }
        }

        // Spawned entities are destroyed outright, so there is nothing to put back for them.
        for (entt::entity Entity : BoundEntities)
        {
            if (Entity == entt::null || !World->IsValidEntity(Entity))
            {
                continue;
            }

            bool bSpawned = false;
            for (entt::entity Other : SpawnedEntities)
            {
                if (Other == Entity)
                {
                    bSpawned = true;
                    break;
                }
            }

            if (bSpawned)
            {
                continue;
            }

            if (STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entity))
            {
                FRestoreEntry Entry;
                Entry.Entity = Entity;
                Entry.Location = Transform->GetLocation();
                Entry.Rotation = Math::Degrees(Math::EulerAngles(Transform->GetRotation()));
                Entry.Scale = Transform->GetScale();
                RestoreState.push_back(Entry);
            }
        }

        bBound = true;
    }

    void FSequenceInstance::Release(CWorld* World, bool bRestore)
    {
        if (World != nullptr)
        {
            if (bRestore && bBound)
            {
                for (const FRestoreEntry& Entry : RestoreState)
                {
                    if (!World->IsValidEntity(Entry.Entity))
                    {
                        continue;
                    }

                    if (STransformComponent* Transform = World->TryGetComponent<STransformComponent>(Entry.Entity))
                    {
                        Transform->SetLocation(Entry.Location);
                        Transform->SetRotationFromEuler(Entry.Rotation);
                        Transform->SetScale(Entry.Scale);
                    }
                }
            }

            for (entt::entity Spawned : SpawnedEntities)
            {
                if (World->IsValidEntity(Spawned))
                {
                    World->DestroyEntity(Spawned);
                }
            }
        }

        SpawnedEntities.clear();
        BoundEntities.clear();
        RestoreState.clear();
        bBound = false;
    }

    void FSequenceInstance::Evaluate(const CSequence* Sequence, CWorld* World, float Time, float PreviousTime, bool bJumped)
    {
        if (Sequence == nullptr || World == nullptr || !bBound)
        {
            return;
        }

        FSequenceEvalContext Context;
        Context.World = World;
        Context.Sequence = Sequence;
        Context.Time = Time;
        Context.PreviousTime = PreviousTime;
        Context.BoundEntities = &BoundEntities;
        Context.bJumped = bJumped;

        Sequence->Evaluate(Context);
    }

    int32 CSequence::GetFrameCount() const
    {
        return Math::Max(1, (int32)Math::Floor(Duration * (float)FrameRate + 0.5f));
    }

    float CSequence::FrameToTime(int32 Frame) const
    {
        return FrameRate > 0 ? (float)Frame / (float)FrameRate : 0.0f;
    }

    int32 CSequence::TimeToFrame(float Time) const
    {
        return (int32)Math::Floor(Time * (float)FrameRate + 0.5f);
    }

    float CSequence::SnapToFrame(float Time) const
    {
        return FrameToTime(TimeToFrame(Time));
    }

    void CSequence::Evaluate(const FSequenceEvalContext& Context) const
    {
        // A cut reads the camera transform, so a track on that camera has to write it first.
        for (const TObjectPtr<CSequenceTrack>& Track : Tracks)
        {
            if (Track.IsValid() && Track->bEnabled && !Track->IsA<CSequenceTrack_CameraCut>())
            {
                Track->Evaluate(Context);
            }
        }

        for (const TObjectPtr<CSequenceTrack>& Track : Tracks)
        {
            if (Track.IsValid() && Track->bEnabled && Track->IsA<CSequenceTrack_CameraCut>())
            {
                Track->Evaluate(Context);
            }
        }
    }
}
