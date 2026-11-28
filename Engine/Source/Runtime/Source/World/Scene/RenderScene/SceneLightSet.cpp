#include "RuntimePCH.h"
#include "SceneLightSet.h"

#include <bit>

#include "Memory/Memcpy.h"
#include "Renderer/Vertex.h"
#include "Renderer/ViewVolume.h"
#include "World/ECS/Registry.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace SceneLightSetInternal
    {
        // Sixty-four lights a block, so a growth step never reallocates for a handful of new lights.
        constexpr uint32 kMinCapacity = 64u;

        uint64 MakeKey(ECS::FEntity Entity, ESceneLightKind Kind)
        {
            return ((uint64)Entity.Value << 2u) | (uint64)Kind;
        }

        template<typename TFunc>
        void ForEachKind(TFunc&& Func)
        {
            for (uint32 Kind = 0; Kind < (uint32)ESceneLightKind::Num; ++Kind)
            {
                Func((ESceneLightKind)Kind);
            }
        }

        template<typename TFunc>
        decltype(auto) WithComponentType(ESceneLightKind Kind, TFunc&& Func)
        {
            switch (Kind)
            {
            case ESceneLightKind::Spot: return Func((SSpotLightComponent*)nullptr);
            case ESceneLightKind::Area: return Func((SAreaLightComponent*)nullptr);
            default:                    return Func((SPointLightComponent*)nullptr);
            }
        }

        FLight MakeRecord(const SPointLightComponent& Light, const STransformComponent& Transform) { return FSceneLightSet::MakePointLight(Light, Transform); }
        FLight MakeRecord(const SSpotLightComponent& Light, const STransformComponent& Transform)  { return FSceneLightSet::MakeSpotLight(Light, Transform); }
        FLight MakeRecord(const SAreaLightComponent& Light, const STransformComponent& Transform)  { return FSceneLightSet::MakeAreaLight(Light, Transform); }
    }

    using namespace SceneLightSetInternal;

    FLight FSceneLightSet::MakePointLight(const SPointLightComponent& Light, const STransformComponent& Transform)
    {
        FLight Record          = {};
        Record.Flags           = ELightFlags::Point;
        Record.Position        = Transform.GetWorldLocation();
        Record.Radius          = Light.Intensity > 0.0f ? Light.Attenuation : 0.0f;
        Record.Falloff         = Light.Falloff;
        Record.Color           = PackColor(FVector4(Light.LightColor, 1.0));
        Record.Intensity       = Light.Intensity;
        Record.ShadowDataIndex = Constants::kIndexNone;
        if (Light.bVolumetric)
        {
            Record.Flags                      |= ELightFlags::Volumetric;
            Record.VolumetricIntensity         = Light.VolumetricIntensity;
            Record.VolumetricScatteringRadius  = Light.VolumetricScatteringRadius;
        }
        Record.Flags = PackLightContactShadow(Record.Flags, Light.bContactShadows, Light.ContactShadowLength);
        Record.Flags = PackLightMinRoughness(Record.Flags, Light.MinRoughness);
        return Record;
    }

    FLight FSceneLightSet::MakeSpotLight(const SSpotLightComponent& Light, const STransformComponent& Transform)
    {
        const FVector3 Forward = Transform.ComputeWorldTransform().GetRotation() * FViewVolume::ForwardAxis;

        FLight Record          = {};
        Record.Flags           = ELightFlags::Spot;
        Record.Position        = Transform.GetWorldLocation();
        Record.Direction       = Math::Normalize(-Forward);
        Record.Radius          = Light.Intensity > 0.0f ? Light.Attenuation : 0.0f;
        Record.Falloff         = Light.Falloff;
        Record.Color           = PackColor(FVector4(Light.LightColor, 1.0));
        Record.Intensity       = Light.Intensity;
        Record.Angles          = FVector2(Math::Cos(Math::Radians(Light.InnerConeAngle)), Math::Cos(Math::Radians(Light.OuterConeAngle)));
        Record.ShadowDataIndex = Constants::kIndexNone;
        if (Light.bVolumetric)
        {
            Record.Flags                      |= ELightFlags::Volumetric;
            Record.VolumetricIntensity         = Light.VolumetricIntensity;
            Record.VolumetricScatteringRadius  = Light.VolumetricScatteringRadius;
        }
        Record.Flags = PackLightContactShadow(Record.Flags, Light.bContactShadows, Light.ContactShadowLength);
        Record.Flags = PackLightMinRoughness(Record.Flags, Light.MinRoughness);
        return Record;
    }

    FLight FSceneLightSet::MakeAreaLight(const SAreaLightComponent& Light, const STransformComponent& Transform)
    {
        const FQuat    Rotation = Transform.ComputeWorldTransform().GetRotation();
        const FVector2 HalfSize = FVector2(Math::Max(Light.Width, 0.01f), Math::Max(Light.Height, 0.01f)) * 0.5f;

        FLight Record          = {};
        Record.Flags           = ELightFlags::Area;
        Record.Position        = Transform.GetWorldLocation();
        Record.Direction       = -Math::Normalize(Rotation * FViewVolume::ForwardAxis);
        // Padded by the half diagonal, so the cull sphere still covers the reach past the rectangle's corners.
        Record.Radius          = Light.Intensity > 0.0f ? Math::Max(Light.Attenuation, 0.01f) + Math::Length(HalfSize) : 0.0f;
        Record.Falloff         = Light.Falloff;
        Record.Color           = PackColor(FVector4(Light.LightColor, 1.0));
        Record.Intensity       = Light.Intensity;
        Record.Angles          = HalfSize;
        Record.ShadowDataIndex = Constants::kIndexNone;
        Record.VolumetricScatteringRadius = std::bit_cast<float>(PackNormal(Math::Normalize(Rotation * FViewVolume::RightAxis)));
        if (Light.bVolumetric)
        {
            Record.Flags               |= ELightFlags::Volumetric;
            Record.VolumetricIntensity  = Light.VolumetricIntensity;
        }
        Record.Flags = PackLightContactShadow(Record.Flags, Light.bContactShadows, Light.ContactShadowLength);
        Record.Flags = PackLightMinRoughness(Record.Flags, Light.MinRoughness);
        return Record;
    }

    FSceneLightSet::~FSceneLightSet()
    {
        Detach();
    }

    void FSceneLightSet::Attach(ECS::FRegistry& InRegistry)
    {
        Detach();
        Registry = &InRegistry;

        Registry->GetSignals<SPointLightComponent>().OnConstruct.Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SPointLightComponent>().OnDestroy  .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SSpotLightComponent>().OnConstruct .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SSpotLightComponent>().OnDestroy   .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SAreaLightComponent>().OnConstruct .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SAreaLightComponent>().OnDestroy   .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SDisabledTag>().OnConstruct        .Connect<&FSceneLightSet::OnMembershipChanged>(this);
        Registry->GetSignals<SDisabledTag>().OnDestroy          .Connect<&FSceneLightSet::OnMembershipChanged>(this);

        MovedConsumer = ECS::Utils::AddMovedTransformConsumer(*Registry, true);
        bRescan = true;
    }

    void FSceneLightSet::Detach()
    {
        if (Registry != nullptr)
        {
            Registry->GetSignals<SPointLightComponent>().OnConstruct.DisconnectPayload(this);
            Registry->GetSignals<SPointLightComponent>().OnDestroy  .DisconnectPayload(this);
            Registry->GetSignals<SSpotLightComponent>().OnConstruct .DisconnectPayload(this);
            Registry->GetSignals<SSpotLightComponent>().OnDestroy   .DisconnectPayload(this);
            Registry->GetSignals<SAreaLightComponent>().OnConstruct .DisconnectPayload(this);
            Registry->GetSignals<SAreaLightComponent>().OnDestroy   .DisconnectPayload(this);
            Registry->GetSignals<SDisabledTag>().OnConstruct        .DisconnectPayload(this);
            Registry->GetSignals<SDisabledTag>().OnDestroy          .DisconnectPayload(this);
            if (MovedConsumer != Constants::kIndexNoneU32)
            {
                ECS::Utils::RemoveMovedTransformConsumer(*Registry, MovedConsumer);
            }
        }
        Registry      = nullptr;
        MovedConsumer = Constants::kIndexNoneU32;
        Records.Reset();
        Layers.Reset();
        Sent.Reset();
        Slots.Reset();
        FreeSlots.Reset();
        SlotOf.Reset();
        Dirty.Reset();
        CPULights.Reset();
        Pending.Lock()->Reset();
        Capacity      = 0;
        NumLive       = 0;
        NumVolumetric = 0;
        NumContact    = 0;
        bRescan       = true;
    }

    void FSceneLightSet::OnMembershipChanged(ECS::FRegistry& InRegistry, ECS::FEntity Entity)
    {
        Pending.Lock()->Add(Entity);
    }

    void FSceneLightSet::Sync()
    {
        LUMINA_PROFILE_SCOPE();
        if (Registry == nullptr)
        {
            return;
        }

        // Writes after this tick stamp newer, so the next sync sees them however they landed.
        const uint32 Since = LastTick;
        LastTick = ECS::FSparseSet::AdvanceChangeTick();

        {
            auto Guard = Pending.Lock();
            PendingScratch = Move(*Guard);
            Guard->Reset();
        }

        if (bRescan)
        {
            RescanAll();
            bRescan = false;
        }
        else
        {
            for (ECS::FEntity Entity : PendingScratch)
            {
                ForEachKind([&](ESceneLightKind Kind) { Refresh(Entity, Kind); });
            }

            ForEachKind([&](ESceneLightKind Kind)
            {
                const uint64 Epoch = WithComponentType(Kind, [&]<typename T>(T*) { return Registry->GetWriteEpoch<T>(); });
                if (Epoch != Epochs[(uint32)Kind])
                {
                    Epochs[(uint32)Kind] = Epoch;
                    ScanEdits(Kind, Since);
                }
            });

            MovedScratch.Reset();
            ECS::Utils::DrainMovedTransforms(*Registry, MovedConsumer, MovedScratch);
            for (ECS::FEntity Entity : MovedScratch)
            {
                ForEachKind([&](ESceneLightKind Kind)
                {
                    if (const uint32* Slot = SlotOf.Find(MakeKey(Entity, Kind)))
                    {
                        Rebuild(*Slot);
                    }
                });
            }
        }

        // This frame's shadow and light function patch comes after, so a caster that lost its slot falls back to its plain record.
        for (const FCPULight& Light : CPULights)
        {
            Rebuild(Light.Slot);
        }

        if ((uint32)Slots.Num() > Capacity)
        {
            Capacity = std::bit_ceil(Math::Max((uint32)Slots.Num(), kMinCapacity));
            Records.Reserve(Capacity);
            // A grown GPU buffer starts empty, so every slot goes up again.
            for (uint32 Slot = 0; Slot < (uint32)Slots.Num(); ++Slot)
            {
                MarkDirty(Slot);
            }
            bResendAll = true;
        }
    }

    void FSceneLightSet::RescanAll()
    {
        for (uint32 Slot = 0; Slot < (uint32)Slots.Num(); ++Slot)
        {
            if (Slots[Slot].bLive)
            {
                Release(Slot);
            }
        }

        ForEachKind([&](ESceneLightKind Kind)
        {
            WithComponentType(Kind, [&]<typename T>(T*)
            {
                Epochs[(uint32)Kind] = Registry->GetWriteEpoch<T>();
                const ECS::FSparseSet* Set = Registry->GetStorage<T>().GetSet();
                const ECS::FEntity* Dense  = Set != nullptr ? Set->GetDenseData() : nullptr;
                const int32 Count          = Set != nullptr ? Set->GetDenseSize() : 0;
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    if (!Dense[Index].IsNull() && Set->FindLiveDense(Dense[Index]) == (uint32)Index)
                    {
                        Refresh(Dense[Index], Kind);
                    }
                }
            });
        });
    }

    void FSceneLightSet::ScanEdits(ESceneLightKind Kind, uint32 Since)
    {
        LUMINA_PROFILE_SCOPE();
        WithComponentType(Kind, [&]<typename T>(T*)
        {
            const ECS::FSparseSet* Set = Registry->GetStorage<T>().GetSet();
            if (Set == nullptr)
            {
                return;
            }

            const ECS::FEntity* Dense = Set->GetDenseData();
            const uint32*       Ticks = Set->GetChangeTickData();
            const int32         Count = Set->GetDenseSize();
            for (int32 Index = 0; Index < Count; ++Index)
            {
                const bool bChanged = Ticks == nullptr || ECS::FSparseSet::IsNewerTick(Ticks[Index], Since);
                if (bChanged && !Dense[Index].IsNull() && Set->FindLiveDense(Dense[Index]) == (uint32)Index)
                {
                    Refresh(Dense[Index], Kind);
                }
            }
        });
    }

    void FSceneLightSet::Refresh(ECS::FEntity Entity, ESceneLightKind Kind)
    {
        const uint64  Key   = MakeKey(Entity, Kind);
        const uint32* Found = SlotOf.Find(Key);

        const bool bPresent = Registry->IsValid(Entity) && !Registry->GetStorage<SDisabledTag>().Contains(Entity)
            && WithComponentType(Kind, [&]<typename T>(T*) { return Registry->GetStorage<T>().Contains(Entity); })
            && Registry->GetStorage<STransformComponent>().Contains(Entity);
        if (!bPresent)
        {
            if (Found != nullptr)
            {
                Release(*Found);
            }
            return;
        }

        const uint32 Slot = Found != nullptr ? *Found : Allocate(Entity, Kind);
        Rebuild(Slot);
    }

    void FSceneLightSet::Rebuild(uint32 Slot)
    {
        FSlot& Info = Slots[Slot];
        const STransformComponent* Transform = Registry->GetStorage<STransformComponent>().TryGetUntracked(Info.Entity);
        if (Transform == nullptr)
        {
            return;
        }

        CountFlags(Records[Slot], -1);
        WithComponentType(Info.Kind, [&]<typename T>(T*)
        {
            const T* Light = Registry->GetStorage<T>().TryGetUntracked(Info.Entity);
            if (Light == nullptr)
            {
                return;
            }
            Records[Slot] = MakeRecord(*Light, *Transform);
            Layers[Slot]  = Light->RenderLayers.Bits;
            bool bLightFunction = false;
            if constexpr (requires { Light->LightFunctionMaterial; })
            {
                bLightFunction = Light->LightFunctionMaterial.Get() != nullptr;
            }
            SetCPU(Slot, Light->bCastShadows || bLightFunction);
        });
        CountFlags(Records[Slot], 1);
        MarkDirty(Slot);
    }

    uint32 FSceneLightSet::Allocate(ECS::FEntity Entity, ESceneLightKind Kind)
    {
        uint32 Slot;
        if (!FreeSlots.IsEmpty())
        {
            Slot = FreeSlots.Last();
            FreeSlots.Pop();
        }
        else
        {
            Slot = (uint32)Slots.Num();
            Slots.Emplace();
            Records.Emplace();
            Layers.Add(0u);
            Sent.Emplace();
        }

        FSlot& Info  = Slots[Slot];
        Info         = FSlot{};
        Info.Entity  = Entity;
        Info.Kind    = Kind;
        Info.bLive   = true;
        Records[Slot] = FLight{};
        SlotOf.TryAdd(MakeKey(Entity, Kind), Slot);
        ECS::Utils::SetMovedTransformInterest(*Registry, MovedConsumer, Entity, true);
        ++NumLive;
        return Slot;
    }

    void FSceneLightSet::Release(uint32 Slot)
    {
        FSlot& Info = Slots[Slot];
        CountFlags(Records[Slot], -1);
        SetCPU(Slot, false);
        SlotOf.Remove(MakeKey(Info.Entity, Info.Kind));

        bool bOtherKind = false;
        ForEachKind([&](ESceneLightKind Kind) { bOtherKind |= SlotOf.Find(MakeKey(Info.Entity, Kind)) != nullptr; });
        if (!bOtherKind)
        {
            ECS::Utils::SetMovedTransformInterest(*Registry, MovedConsumer, Info.Entity, false);
        }

        // A zero radius is what every view's cull reads as an empty slot.
        Records[Slot] = FLight{};
        Layers[Slot]  = 0u;
        Info.bLive    = false;
        Info.Entity   = ECS::FEntity();
        MarkDirty(Slot);
        FreeSlots.Add(Slot);
        --NumLive;
    }

    void FSceneLightSet::SetCPU(uint32 Slot, bool bCPU)
    {
        FSlot& Info = Slots[Slot];
        const bool bListed = Info.CPUIndex != Constants::kIndexNoneU32;
        if (bCPU == bListed)
        {
            return;
        }

        if (bCPU)
        {
            Info.CPUIndex = (uint32)CPULights.Num();
            CPULights.Add(FCPULight{ Info.Entity, Info.Kind, Slot });
            return;
        }

        const uint32 Index = Info.CPUIndex;
        CPULights.RemoveAtSwap((int32)Index);
        if (Index < (uint32)CPULights.Num())
        {
            Slots[CPULights[Index].Slot].CPUIndex = Index;
        }
        Info.CPUIndex = Constants::kIndexNoneU32;
    }

    void FSceneLightSet::Patch(uint32 Slot, const FLight& Light)
    {
        if (Slot < (uint32)Records.Num() && Slots[Slot].bLive)
        {
            Records[Slot] = Light;
            MarkDirty(Slot);
        }
    }

    void FSceneLightSet::MarkDirty(uint32 Slot)
    {
        if (!Slots[Slot].bDirty)
        {
            Slots[Slot].bDirty = true;
            Dirty.Add(Slot);
        }
    }

    void FSceneLightSet::CountFlags(const FLight& Light, int32 Sign)
    {
        if (Light.Radius <= 0.0f)
        {
            return;
        }
        if (EnumHasAnyFlags(Light.Flags, ELightFlags::Volumetric))
        {
            NumVolumetric = (uint32)((int32)NumVolumetric + Sign);
        }
        if (EnumHasAnyFlags(Light.Flags, ELightFlags::ContactShadow))
        {
            NumContact = (uint32)((int32)NumContact + Sign);
        }
    }

    void FSceneLightSet::TakeUploads(TVector<FUpload>& Out)
    {
        Out.Reserve(Out.Num() + Dirty.Num());
        for (uint32 Slot : Dirty)
        {
            Slots[Slot].bDirty = false;
            FUpload& Last = Sent[Slot];
            if (!bResendAll && Last.Layers == Layers[Slot] && Memory::Memcmp(&Last.Light, &Records[Slot], sizeof(FLight)) == 0)
            {
                continue;
            }
            Last.Light  = Records[Slot];
            Last.Slot   = Slot;
            Last.Layers = Layers[Slot];
            Out.Add(Last);
        }
        Dirty.Reset();
        bResendAll = false;
    }
}
