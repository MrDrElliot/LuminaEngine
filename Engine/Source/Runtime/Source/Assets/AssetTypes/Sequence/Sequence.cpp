#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Sequence.h"

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Core/Object/Cast.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Audio/AudioLibrary.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "World/World.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "World/Entity/Systems/SystemSingletons.h"

namespace Lumina
{
    ECS::FEntity FSequenceEvalContext::Resolve(int32 BindingIndex) const
    {
        if (BoundEntities == nullptr || BindingIndex < 0 || BindingIndex >= (int32)BoundEntities->size())
        {
            return ECS::NullEntity;
        }

        return (*BoundEntities)[BindingIndex];
    }

    bool FSequenceEvalContext::Crossed(float KeyTime) const
    {
        if (bStarted)
        {
            return KeyTime >= PreviousTime && KeyTime <= Time;
        }
        if (bWrapped)
        {
            return KeyTime > PreviousTime || KeyTime <= Time;
        }
        if (bJumped)
        {
            return false;
        }
        return KeyTime > PreviousTime && KeyTime <= Time;
    }

    FVector3 SSequenceVectorCurve::Evaluate(float Time) const
    {
        return FVector3(X.Evaluate(Time), Y.Evaluate(Time), Z.Evaluate(Time));
    }

    void SSequenceVectorCurve::UnwindAngles()
    {
        for (SCurve* Channel : { &X, &Y, &Z })
        {
            TVector<SCurveKey>& Keys = Channel->Curve.Keys;
            for (size_t Index = 1; Index < Keys.size(); ++Index)
            {
                const float Previous = Keys[Index - 1].Value;
                while (Keys[Index].Value - Previous > 180.0f)
                {
                    Keys[Index].Value -= 360.0f;
                }
                while (Keys[Index].Value - Previous < -180.0f)
                {
                    Keys[Index].Value += 360.0f;
                }
            }
        }
    }

    void CSequenceTrack_Transform::Evaluate(const FSequenceEvalContext& Context) const
    {
        const ECS::FEntity Entity = Context.Resolve(BindingIndex);
        if (Entity == ECS::NullEntity || Context.World == nullptr || !Context.World->IsValidEntity(Entity))
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
            Transform->SetRotation(Math::FromYawFirstEuler(Math::Radians(Rotation.Evaluate(Context.Time))));
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

        const ECS::FEntity Camera = Context.Resolve(Cuts[CutIndex].BindingIndex);
        if (Camera == ECS::NullEntity || !Context.World->IsValidEntity(Camera))
        {
            return;
        }

        if (Context.Instance != nullptr && !Context.Instance->bDriveCamera)
        {
            return;
        }

        // SetActiveCamera is a switch, so re-issuing it each frame would stomp anything that took the camera.
        if (Context.bJumped || FindCutAt(Context.PreviousTime) != CutIndex)
        {
            // A jump lands on the shot at once, since easing in from wherever the scrub was would show a blend never authored.
            const float Blend = Context.bJumped ? 0.0f : Cuts[CutIndex].BlendTime;
            Context.World->SetActiveCamera(Camera, Blend);
        }
    }

    void CSequenceTrack_Event::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.Instance == nullptr)
        {
            return;
        }

        const ECS::FEntity Target = Context.Resolve(BindingIndex);
        for (const SSequenceEventKey& Key : Keys)
        {
            if (Context.Crossed(Key.Time))
            {
                SSequenceFiredEvent& Fired = Context.Instance->FiredEvents.emplace_back();
                Fired.Name    = Key.Name;
                Fired.Payload = Key.Payload;
                Fired.Time    = Key.Time;
                Fired.Target  = Target;
            }
        }
    }

    namespace
    {
        // Walks a dotted path of reflected fields from a component down to a leaf, stepping into nested structs.
        FProperty* ResolvePropertyPath(CStruct* Struct, void* Container, FStringView Path, void*& OutContainer)
        {
            while (Struct != nullptr && Container != nullptr && !Path.empty())
            {
                const size_t Dot = Path.find('.');
                const FStringView Segment = Dot == FStringView::npos ? Path : Path.substr(0, Dot);
                FProperty* Property = Struct->GetProperty(FName(Segment));
                if (Property == nullptr)
                {
                    return nullptr;
                }

                if (Dot == FStringView::npos)
                {
                    OutContainer = Container;
                    return Property;
                }

                if (!Property->IsA(EPropertyTypeFlags::Struct))
                {
                    return nullptr;
                }

                Container = Property->GetValuePtr<void>(Container);
                Struct = static_cast<FStructProperty*>(Property)->GetStruct();
                Path = Path.substr(Dot + 1);
            }
            return nullptr;
        }
    }

    void CSequenceTrack_Property::Evaluate(const FSequenceEvalContext& Context) const
    {
        const ECS::FEntity Entity = Context.Resolve(BindingIndex);
        if (Entity == ECS::NullEntity || Context.World == nullptr || !Context.World->IsValidEntity(Entity) || PropertyPath.empty())
        {
            return;
        }

        const FComponentOps* Ops = FindComponentOps(ComponentType.ToString());
        if (Ops == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*Context.World);
        void* Component = Ops->Get(Registry, Entity);
        if (Component == nullptr)
        {
            return;
        }

        void* Container = nullptr;
        FProperty* Property = ResolvePropertyPath(Ops->StaticStruct(), Component, FStringView(PropertyPath.c_str(), PropertyPath.size()), Container);
        if (Property == nullptr)
        {
            return;
        }

        const float Value = Curve.Evaluate(Context.Time);
        if (Property->IsA(EPropertyTypeFlags::Float))
        {
            *Property->GetValuePtr<float>(Container) = Value;
        }
        else if (Property->IsA(EPropertyTypeFlags::Double))
        {
            *Property->GetValuePtr<double>(Container) = (double)Value;
        }
        else if (Property->IsA(EPropertyTypeFlags::Int32))
        {
            *Property->GetValuePtr<int32>(Container) = (int32)Math::Round(Value);
        }
        else if (Property->IsA(EPropertyTypeFlags::Bool))
        {
            *Property->GetValuePtr<bool>(Container) = Value >= 0.5f;
        }
        else
        {
            return;
        }

        // Systems that only react to changed components would otherwise never see a sequence move the value.
        if (Ops->Patch != nullptr)
        {
            Ops->Patch(Registry, Entity);
        }
    }

    bool CSequenceTrack_Property::SampleValue(CWorld* World, ECS::FEntity Entity, float& OutValue) const
    {
        if (World == nullptr || Entity == ECS::NullEntity || !World->IsValidEntity(Entity) || PropertyPath.empty())
        {
            return false;
        }

        const FComponentOps* Ops = FindComponentOps(ComponentType.ToString());
        void* Component = Ops != nullptr ? Ops->Get(ECS::GetWorldRegistry(*World), Entity) : nullptr;
        if (Component == nullptr)
        {
            return false;
        }

        void* Container = nullptr;
        FProperty* Property = ResolvePropertyPath(Ops->StaticStruct(), Component, FStringView(PropertyPath.c_str(), PropertyPath.size()), Container);
        if (Property == nullptr)
        {
            return false;
        }

        if (Property->IsA(EPropertyTypeFlags::Float))       { OutValue = *Property->GetValuePtr<float>(Container); }
        else if (Property->IsA(EPropertyTypeFlags::Double)) { OutValue = (float)*Property->GetValuePtr<double>(Container); }
        else if (Property->IsA(EPropertyTypeFlags::Int32))  { OutValue = (float)*Property->GetValuePtr<int32>(Container); }
        else if (Property->IsA(EPropertyTypeFlags::Bool))   { OutValue = *Property->GetValuePtr<bool>(Container) ? 1.0f : 0.0f; }
        else
        {
            return false;
        }
        return true;
    }

    void CSequenceTrack_Audio::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.Instance == nullptr)
        {
            return;
        }

        // A scrub or a loop restarts the clips from wherever the playhead now is.
        if ((Context.bJumped && !Context.bStarted) || Context.bWrapped)
        {
            OnRelease(*Context.Instance, Context.World);
        }

        const ECS::FEntity Entity = Context.Resolve(BindingIndex);
        const STransformComponent* Transform = (Entity != ECS::NullEntity && Context.World != nullptr && Context.World->IsValidEntity(Entity))
            ? Context.World->TryGetComponent<STransformComponent>(Entity) : nullptr;

        for (int32 ClipIndex = 0; ClipIndex < (int32)Clips.size(); ++ClipIndex)
        {
            const SSequenceAudioClip& Clip = Clips[ClipIndex];
            if (Clip.Sound == nullptr)
            {
                continue;
            }

            // A clip already started keeps playing, or waits with the sequence, and never starts twice.
            FSequenceInstance::FPlayingSound* Playing = nullptr;
            for (FSequenceInstance::FPlayingSound& Sound : Context.Instance->Sounds)
            {
                if (Sound.Track == this && Sound.ClipIndex == ClipIndex)
                {
                    Playing = &Sound;
                    break;
                }
            }
            if (Playing != nullptr)
            {
                if (Playing->bPaused != Context.bPaused)
                {
                    CAudioLibrary::SetPaused(Playing->Handle, Context.bPaused);
                    Playing->bPaused = Context.bPaused;
                }
                continue;
            }
            if (Context.bPaused)
            {
                continue;
            }

            // Without a known length only the moment the playhead crosses the start counts, since a finished clip must not replay.
            const float Offset = Context.Time - Clip.StartTime;
            const float Length = Clip.Sound->GetDuration() / Math::Max(Clip.Pitch, 0.01f);
            const bool bInside = Length > 0.0f ? (Offset >= 0.0f && Offset < Length) : Context.Crossed(Clip.StartTime);
            if (!bInside)
            {
                continue;
            }

            FAudioPlayParams Params = CAudioLibrary::DefaultPlayParams();
            Params.Volume = Clip.Volume;
            Params.Pitch = Clip.Pitch;
            Params.Bus = Clip.Bus;
            if (Transform != nullptr)
            {
                Params.bSpatialized = true;
                Params.Position = Transform->GetWorldLocation();
            }
            if (const CAudioStream* Stream = Cast<CAudioStream>(Clip.Sound.Get()); Stream != nullptr && Offset > 0.0f)
            {
                Params.StartFrame = (uint64)(Offset * Clip.Pitch * (float)Stream->SampleRate);
            }

            Context.Instance->Sounds.push_back({ this, CAudioLibrary::PlaySoundEx(Clip.Sound.Get(), Params), ClipIndex, false });
        }
    }

    void CSequenceTrack_Audio::OnRelease(FSequenceInstance& Instance, CWorld* World) const
    {
        for (size_t Index = 0; Index < Instance.Sounds.size();)
        {
            if (Instance.Sounds[Index].Track == this)
            {
                CAudioLibrary::Stop(Instance.Sounds[Index].Handle, true, 0.3f);
                Instance.Sounds.erase(Instance.Sounds.begin() + Index);
                continue;
            }
            ++Index;
        }
    }

    void CSequenceTrack_CameraShake::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.Instance == nullptr || Context.World == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*Context.World);
        FSequenceInstance::FPlayingShake* Playing = nullptr;
        for (FSequenceInstance::FPlayingShake& Shake : Context.Instance->Shakes)
        {
            if (Shake.Track == this)
            {
                Playing = &Shake;
                break;
            }
        }

        if (Playing == nullptr)
        {
            FCameraShakeParams Params;
            Params.LocationAmplitude = LocationAmplitude;
            Params.RotationAmplitude = RotationAmplitude;
            Params.Frequency = Frequency;
            Params.Duration = 0.0f;
            Params.BlendInTime = 0.0f;
            Params.BlendOutTime = 0.3f;
            Context.Instance->Shakes.push_back({ this, SCameraSystem::PlayCameraShake(Registry, Params) });
            Playing = &Context.Instance->Shakes.back();
        }

        const float Scale = Intensity.Curve.Keys.empty() && !Intensity.bUseAsset ? 1.0f : Math::Max(Intensity.Evaluate(Context.Time), 0.0f);
        FCameraGlobalState* State = Registry.Ctx().Find<FCameraGlobalState>();
        if (State == nullptr)
        {
            return;
        }
        for (FCameraShakeInstance& Shake : State->Shakes)
        {
            if (Shake.Handle == Playing->Handle)
            {
                Shake.LocationAmplitude = LocationAmplitude * Scale;
                Shake.RotationAmplitude = RotationAmplitude * Scale;
                Shake.Frequency = Frequency;
                break;
            }
        }
    }

    void CSequenceTrack_CameraShake::OnRelease(FSequenceInstance& Instance, CWorld* World) const
    {
        for (size_t Index = 0; Index < Instance.Shakes.size();)
        {
            if (Instance.Shakes[Index].Track == this)
            {
                if (World != nullptr)
                {
                    SCameraSystem::StopCameraShake(ECS::GetWorldRegistry(*World), Instance.Shakes[Index].Handle);
                }
                Instance.Shakes.erase(Instance.Shakes.begin() + Index);
                continue;
            }
            ++Index;
        }
    }

    void CSequenceTrack_Fade::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.Instance == nullptr)
        {
            return;
        }

        const float Value = Math::Clamp(Amount.Evaluate(Context.Time), 0.0f, 1.0f);
        if (Value > Context.Instance->OverlayFade)
        {
            Context.Instance->OverlayFade      = Value;
            Context.Instance->OverlayFadeColor = Color;
        }
    }

    void CSequenceTrack_TimeDilation::Evaluate(const FSequenceEvalContext& Context) const
    {
        if (Context.World != nullptr)
        {
            Context.World->SetTimeDilation(Math::Max(Scale.Evaluate(Context.Time), 0.0f));
        }
    }

    void CSequenceTrack_TimeDilation::OnRelease(FSequenceInstance& Instance, CWorld* World) const
    {
        if (World != nullptr)
        {
            World->SetTimeDilation(Instance.PreviousTimeDilation);
        }
    }

    void CSequenceTrack_LookAt::Evaluate(const FSequenceEvalContext& Context) const
    {
        const ECS::FEntity Entity = Context.Resolve(BindingIndex);
        if (Context.World == nullptr || Entity == ECS::NullEntity || !Context.World->IsValidEntity(Entity))
        {
            return;
        }

        STransformComponent* Transform = Context.World->TryGetComponent<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        FVector3 To = TargetOffset;
        if (TargetBindingIndex != INDEX_NONE)
        {
            const ECS::FEntity Target = Context.Resolve(TargetBindingIndex);
            const STransformComponent* TargetTransform = Target != ECS::NullEntity && Context.World->IsValidEntity(Target)
                ? Context.World->TryGetComponent<STransformComponent>(Target) : nullptr;
            if (TargetTransform == nullptr)
            {
                return;
            }
            To += TargetTransform->GetWorldLocation();
        }

        // An unkeyed weight means a plain aim, so the common case needs no keys at all.
        const float Amount = Weight.Curve.Keys.empty() && !Weight.bUseAsset ? 1.0f : Math::Clamp(Weight.Evaluate(Context.Time), 0.0f, 1.0f);
        const FVector3 From = Transform->GetWorldLocation();
        if (bAutoFocus)
        {
            if (SCameraComponent* Camera = Context.World->TryGetComponent<SCameraComponent>(Entity))
            {
                Camera->PostProcess.DepthOfFieldFocusDistance = Math::Max(Math::Length(To - From), 0.1f);
            }
        }
        if (Amount <= 0.0f || Math::LengthSquared(To - From) < 1e-6f)
        {
            return;
        }

        const FQuat Aim = Math::FindLookAtRotation(To, From);
        Transform->SetRotation(Amount >= 1.0f ? Aim : Math::Slerp(Transform->GetRotation(), Aim, Amount));
    }

    void FSequenceInstance::Bind(const CSequence* Sequence, CWorld* World)
    {
        Release(World, true);

        if (Sequence == nullptr || World == nullptr)
        {
            return;
        }

        BoundEntities.assign(Sequence->Bindings.size(), ECS::NullEntity);
        PreviousCamera = World->GetActiveCameraEntity();
        PreviousTimeDilation = World->GetTimeDilation();

        for (int32 i = 0; i < (int32)Sequence->Bindings.size(); ++i)
        {
            const SSequenceBinding& Binding = Sequence->Bindings[i];

            if (Binding.Kind == ESequenceBindingKind::Camera)
            {
                const ECS::FEntity Camera = World->ConstructEntity(Binding.Name);
                World->GetOrEmplaceComponent<SCameraComponent>(Camera);
                BoundEntities[i] = Camera;
                SpawnedEntities.push_back(Camera);
                continue;
            }

            if (Binding.Kind == ESequenceBindingKind::Spawn)
            {
                if (!Binding.SpawnPrefab.IsValid())
                {
                    continue;
                }

                const ECS::FEntity Spawned = Binding.SpawnPrefab->Instantiate(World);
                if (Spawned != ECS::NullEntity)
                {
                    BoundEntities[i] = Spawned;
                    SpawnedEntities.push_back(Spawned);
                }
                continue;
            }

            auto View = World->View<SNameComponent>();
            for (ECS::FEntity Entity : View)
            {
                if (View.Get<SNameComponent>(Entity).Name == Binding.Name)
                {
                    BoundEntities[i] = Entity;
                    break;
                }
            }
        }

        // Spawned entities are destroyed outright, so there is nothing to put back for them.
        for (ECS::FEntity Entity : BoundEntities)
        {
            if (Entity == ECS::NullEntity || !World->IsValidEntity(Entity))
            {
                continue;
            }

            bool bSpawned = false;
            for (ECS::FEntity Other : SpawnedEntities)
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
                Entry.Rotation = Transform->GetRotation();
                Entry.Scale = Transform->GetScale();
                RestoreState.push_back(Entry);
            }
        }

        BoundSequence = Sequence;
        bBound = true;
    }

    void FSequenceInstance::Release(CWorld* World, bool bRestore)
    {
        if (World != nullptr && bBound)
        {
            if (BoundSequence != nullptr)
            {
                for (const TObjectPtr<CSequenceTrack>& Track : BoundSequence->Tracks)
                {
                    if (Track.IsValid())
                    {
                        Track->OnRelease(*this, World);
                    }
                }
            }

            SCameraSystem::SetCinematicOverlay(ECS::GetWorldRegistry(*World), 0.0f, FVector3(0.0f), 0.0f);

            // Hands the view back when the sequence still owns it, so gameplay resumes on its own camera.
            const ECS::FEntity Active = World->GetActiveCameraEntity();
            bool bOwnsView = false;
            for (ECS::FEntity Entity : BoundEntities)
            {
                bOwnsView |= Entity != ECS::NullEntity && Entity == Active;
            }
            if (bOwnsView && PreviousCamera != ECS::NullEntity && World->IsValidEntity(PreviousCamera))
            {
                World->SetActiveCamera(PreviousCamera);
            }

            if (bRestore)
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
                        Transform->SetRotation(Entry.Rotation);
                        Transform->SetScale(Entry.Scale);
                    }
                }
            }

            for (ECS::FEntity Spawned : SpawnedEntities)
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
        Sounds.clear();
        Shakes.clear();
        PreviousCamera = ECS::NullEntity;
        BoundSequence = nullptr;
        bBound = false;
    }

    void FSequenceInstance::Evaluate(const CSequence* Sequence, CWorld* World, float Time, float PreviousTime, bool bJumped, bool bStarted, bool bWrapped,
                                     bool bPaused)
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
        Context.bJumped = bJumped || bStarted || bWrapped;
        Context.bStarted = bStarted;
        Context.bWrapped = bWrapped;
        Context.bPaused = bPaused;
        Context.Instance = this;

        OverlayFade = 0.0f;
        Sequence->Evaluate(Context);
        SCameraSystem::SetCinematicOverlay(ECS::GetWorldRegistry(*World), OverlayFade, OverlayFadeColor, Sequence->LetterboxAspect);
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
        // Tracks run in their declared order, so a look-at aims from where the transform put the camera and cuts read both.
        TFixedVector<const CSequenceTrack*, 64> Ordered;
        for (const TObjectPtr<CSequenceTrack>& Track : Tracks)
        {
            if (Track.IsValid() && Track->bEnabled)
            {
                Ordered.push_back(Track.Get());
            }
        }

        Algo::StableSort(Ordered.begin(), Ordered.end(), [](const CSequenceTrack* A, const CSequenceTrack* B)
        {
            return A->GetEvaluationOrder() < B->GetEvaluationOrder();
        });

        for (const CSequenceTrack* Track : Ordered)
        {
            Track->Evaluate(Context);
        }
    }
}
