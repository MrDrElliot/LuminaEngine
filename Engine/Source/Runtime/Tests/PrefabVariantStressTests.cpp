#include <gtest/gtest.h>
#include <random>
#include "World/ECS/Registry.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Assets/AssetTypes/Prefabs/PrefabOverrideTestTypes.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "GUID/GUID.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    CPrefab* NewPrefab()
    {
        ProcessNewlyLoadedCObjects();
        return NewObject<CPrefab>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    FName StableIDOf(ECS::FRegistry& Registry, ECS::FEntity E)
    {
        const SPrefabComponent* Comp = Registry.IsValid(E) ? Registry.TryGet<SPrefabComponent>(E) : nullptr;
        return Comp != nullptr ? Comp->StableID : FName();
    }

    FName ParentIDOf(ECS::FRegistry& Registry, ECS::FEntity E)
    {
        const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(E);
        return Parent != ECS::NullEntity ? StableIDOf(Registry, Parent) : FName();
    }

    THashMap<FName, ECS::FEntity> Index(ECS::FRegistry& Registry)
    {
        THashMap<FName, ECS::FEntity> Out;
        Registry.View<SPrefabComponent>().ForEach([&](ECS::FEntity E, const SPrefabComponent& Comp)
        {
            Out[Comp.StableID] = E;
        });
        return Out;
    }

    ECS::FEntity Find(ECS::FRegistry& Registry, FName ID)
    {
        THashMap<FName, ECS::FEntity> All = Index(Registry);
        auto It = All.find(ID);
        return It != All.end() ? It->second : ECS::NullEntity;
    }

    ECS::FEntity AddNode(ECS::FRegistry& Registry, FName ID, FVector3 Location, ECS::FEntity Parent = ECS::NullEntity)
    {
        const ECS::FEntity E = Registry.Create();
        Registry.Emplace<SPrefabComponent>(E).StableID = ID;
        Registry.Emplace<STransformComponent>(E).SetLocalLocation(Location);
        if (Parent != ECS::NullEntity)
        {
            ECS::Utils::ReparentEntity(Registry, E, Parent, false);
        }
        return E;
    }

    bool Near(FVector3 A, FVector3 B) { return Math::Length(A - B) < 1.0e-4f; }
    bool Near(FQuat A, FQuat B) { return Math::Abs(Math::Dot(A, B)) > 1.0f - 1.0e-5f; }

    // Everything a variant authors about a node, keyed by StableID so two registries with different ids compare.
    struct FNodeState
    {
        FName Parent;
        FVector3 Location;
        FQuat Rotation;
        FVector3 Scale;
        bool bTransform = false;
        bool bLight = false;
        float Intensity = 0.0f;
        FVector3 LightColor;
        bool bLink = false;
        int32 LinkValue = 0;
        FName LinkTarget;
    };

    THashMap<FName, FNodeState> Snapshot(ECS::FRegistry& Registry)
    {
        THashMap<FName, FNodeState> Out;
        for (auto& [ID, E] : Index(Registry))
        {
            FNodeState& S = Out[ID];
            S.Parent = ParentIDOf(Registry, E);
            if (const STransformComponent* T = Registry.TryGet<STransformComponent>(E))
            {
                S.bTransform = true;
                S.Location = T->GetLocalLocation();
                S.Rotation = T->GetLocalRotation();
                S.Scale = T->GetLocalScale();
            }
            if (const SPointLightComponent* L = Registry.TryGet<SPointLightComponent>(E))
            {
                S.bLight = true;
                S.Intensity = L->Intensity;
                S.LightColor = L->LightColor;
            }
            if (const SPrefabLinkTestComponent* K = Registry.TryGet<SPrefabLinkTestComponent>(E))
            {
                S.bLink = true;
                S.LinkValue = K->Value;
                S.LinkTarget = StableIDOf(Registry, K->Target);
            }
        }
        return Out;
    }

    // Returns a description of the first difference, or an empty string when the two match.
    std::string Diff(const THashMap<FName, FNodeState>& Want, const THashMap<FName, FNodeState>& Got)
    {
        for (const auto& [ID, W] : Want)
        {
            auto It = Got.find(ID);
            if (It == Got.end())
            {
                return std::string("missing node ") + ID.c_str();
            }
            const FNodeState& G = It->second;
            const std::string N = ID.c_str();
            if (!G.bTransform) return N + " has no transform";
            if (W.Parent != G.Parent) return N + " parent " + W.Parent.c_str() + " became " + G.Parent.c_str();
            if (!Near(W.Location, G.Location))
            {
                return N + " location (" + std::to_string(W.Location.x) + "," + std::to_string(W.Location.y) + "," + std::to_string(W.Location.z)
                    + ") became (" + std::to_string(G.Location.x) + "," + std::to_string(G.Location.y) + "," + std::to_string(G.Location.z) + ")";
            }
            if (!Near(W.Rotation, G.Rotation))
            {
                auto Q = [](FQuat R) { return "(" + std::to_string(R.x) + "," + std::to_string(R.y) + "," + std::to_string(R.z) + "," + std::to_string(R.w) + ")"; };
                return N + " rotation " + Q(W.Rotation) + " became " + Q(G.Rotation);
            }
            if (!Near(W.Scale, G.Scale)) return N + " scale changed";
            if (W.bLight != G.bLight) return N + (W.bLight ? " lost its light" : " gained a light");
            if (W.bLight && (W.Intensity != G.Intensity || !Near(W.LightColor, G.LightColor))) return N + " light values changed";
            if (W.bLink != G.bLink) return N + (W.bLink ? " lost its link" : " gained a link");
            if (W.bLink && W.LinkValue != G.LinkValue) return N + " link value changed";
            if (W.bLink && W.LinkTarget != G.LinkTarget) return N + " link target " + W.LinkTarget.c_str() + " became " + G.LinkTarget.c_str();
        }
        for (const auto& [ID, G] : Got)
        {
            if (Want.find(ID) == Want.end())
            {
                return std::string("unexpected node ") + ID.c_str();
            }
        }
        return {};
    }

    // A zero quaternion or a missing transform only comes from decomposing a world matrix nobody computed.
    std::string Degenerate(ECS::FRegistry& Registry)
    {
        for (auto& [ID, E] : Index(Registry))
        {
            const STransformComponent* T = Registry.TryGet<STransformComponent>(E);
            if (T == nullptr)
            {
                return std::string(ID.c_str()) + " has no transform";
            }
            const FQuat R = T->GetLocalRotation();
            if (Math::Abs(Math::Dot(R, R) - 1.0f) > 1.0e-3f)
            {
                return std::string(ID.c_str()) + " has a degenerate rotation";
            }
        }
        return {};
    }

    // The instance root is placed by the level, so only its children are held to the prefab's values.
    std::string DiffInstance(const THashMap<FName, FNodeState>& Want, ECS::FRegistry& World, ECS::FEntity InstanceRoot)
    {
        THashMap<FName, ECS::FEntity> ByID;
        ByID[World.Get<SPrefabInstanceComponent>(InstanceRoot).StableID] = InstanceRoot;
        World.GetHierarchy().ForEachDescendant(InstanceRoot, [&](ECS::FEntity E)
        {
            if (const SPrefabInstanceComponent* Inst = World.TryGet<SPrefabInstanceComponent>(E))
            {
                ByID[Inst->StableID] = E;
            }
        });
        auto IDOf = [&](ECS::FEntity E)
        {
            const SPrefabInstanceComponent* Inst = World.IsValid(E) ? World.TryGet<SPrefabInstanceComponent>(E) : nullptr;
            return Inst != nullptr ? Inst->StableID : FName();
        };

        // A prefab with several roots nests the extras under the placed root, so a root-level node reads as the root's child.
        const FName RootID = IDOf(InstanceRoot);
        for (const auto& [ID, W] : Want)
        {
            auto It = ByID.find(ID);
            if (It == ByID.end())
            {
                return std::string("instance is missing ") + ID.c_str();
            }
            const ECS::FEntity E = It->second;
            const std::string N = ID.c_str();
            if (E == InstanceRoot)
            {
                continue;
            }
            const FName Parent = IDOf(World.GetHierarchy().GetParent(E));
            const FName WantParent = W.Parent.IsNone() ? RootID : W.Parent;
            if (Parent != WantParent) return N + " instance parent " + WantParent.c_str() + " became " + Parent.c_str();
            const STransformComponent* T = World.TryGet<STransformComponent>(E);
            if (T == nullptr) return N + " instance has no transform";
            if (!Near(T->GetLocalLocation(), W.Location))
            {
                const FVector3 G = T->GetLocalLocation();
                return N + " instance location (" + std::to_string(W.Location.x) + "," + std::to_string(W.Location.y) + "," + std::to_string(W.Location.z)
                    + ") is (" + std::to_string(G.x) + "," + std::to_string(G.y) + "," + std::to_string(G.z) + ")";
            }
            if (!Near(T->GetLocalRotation(), W.Rotation)) return N + " instance rotation differs";
            const SPointLightComponent* L = World.TryGet<SPointLightComponent>(E);
            if ((L != nullptr) != W.bLight) return N + " instance light presence differs";
            if (L != nullptr && L->Intensity != W.Intensity) return N + " instance light intensity differs";
            const SPrefabLinkTestComponent* K = World.TryGet<SPrefabLinkTestComponent>(E);
            if ((K != nullptr) != W.bLink) return N + " instance link presence differs";
            if (K != nullptr && K->Value != W.LinkValue) return N + " instance link value differs";
            if (K != nullptr && IDOf(K->Target) != W.LinkTarget) return N + " instance link target " + W.LinkTarget.c_str() + " became " + IDOf(K->Target).c_str();
        }
        for (const auto& [ID, E] : ByID)
        {
            if (E != InstanceRoot && Want.find(ID) == Want.end())
            {
                return std::string("instance kept ") + ID.c_str();
            }
        }
        return {};
    }

    void BindTransformLikeAWorld(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
    }

    // A bare world skips InitializeWorld, so its transforms are bound the way the world's construct hook binds them.
    CWorld* MakeWorld()
    {
        ProcessNewlyLoadedCObjects();
        CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        ECS::GetWorldRegistry(*World).GetSignals<STransformComponent>().OnConstruct.Connect<&BindTransformLikeAWorld>();
        return World;
    }

    // What the end of a frame does, so a later refresh reads current world matrices.
    void EndFrame(CWorld* World)
    {
        ECS::Utils::ResolveAllDirtyTransforms(ECS::GetWorldRegistry(*World));
    }

    CPrefab* MakeParent()
    {
        CPrefab* Parent = NewPrefab();
        ECS::FRegistry& R = Parent->Registry;
        const ECS::FEntity Root = AddNode(R, "root", FVector3(0.0f));
        const ECS::FEntity Arm = AddNode(R, "arm", FVector3(1.0f, 2.0f, 3.0f), Root);
        const ECS::FEntity Hand = AddNode(R, "hand", FVector3(0.0f, 0.5f, 0.0f), Arm);
        AddNode(R, "leg", FVector3(-1.0f, -2.0f, 0.5f), Root);
        R.Emplace<SPointLightComponent>(Hand).Intensity = 3.0f;
        R.Emplace<SPrefabLinkTestComponent>(Arm).Target = Hand;
        return Parent;
    }

    CPrefab* MakeVariantOf(CPrefab* Parent)
    {
        CPrefab* Variant = NewPrefab();
        Variant->ParentPrefab = Parent;
        Variant->ResolveVariant();
        return Variant;
    }

    // Capture then resolve must hand back exactly what was edited.
    void ExpectRoundTrip(CPrefab* Variant, const char* Context)
    {
        const auto Edited = Snapshot(Variant->Registry);
        Variant->CaptureVariantDelta();
        Variant->ResolveVariant();
        ASSERT_FALSE(Variant->IsUnresolvedVariant()) << Context;
        const std::string Problem = Diff(Edited, Snapshot(Variant->Registry));
        EXPECT_TRUE(Problem.empty()) << Context << ": " << Problem;
    }
}

// A plain resolve hands every inherited child its authored local transform, not one rebuilt from stale world matrices.
TEST(PrefabVariantStress, ResolveKeepsInheritedChildTransforms)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ASSERT_FALSE(Variant->IsUnresolvedVariant());
    EXPECT_EQ(Diff(Snapshot(Parent->Registry), Snapshot(Variant->Registry)), "");
}

TEST(PrefabVariantStress, OverriddenAndAddedChildrenKeepTheirTransforms)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    R.Get<STransformComponent>(Find(R, "hand")).SetLocalLocation(FVector3(4.0f, 5.0f, 6.0f));
    AddNode(R, "added", FVector3(7.0f, 8.0f, 9.0f), Find(R, "arm"));
    AddNode(R, "added-root", FVector3(-3.0f, 0.0f, 1.0f));
    ExpectRoundTrip(Variant, "override and add");
}

// The variant moves an inherited node to a different parent and keeps its local offset there.
TEST(PrefabVariantStress, ReparentedInheritedNodeRoundTrips)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    ECS::Utils::ReparentEntity(R, Find(R, "hand"), Find(R, "leg"), false);
    ExpectRoundTrip(Variant, "reparent under a sibling");

    ECS::Utils::ReparentEntity(R, Find(R, "leg"), ECS::NullEntity, false);
    ExpectRoundTrip(Variant, "unparent to the root");
}

// Deleting an inherited node whose child the variant keeps must not move the kept child.
TEST(PrefabVariantStress, DeletedParentRescuesChildInPlace)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    const ECS::FEntity Hand = Find(R, "hand");
    ECS::Utils::ReparentEntity(R, Hand, ECS::NullEntity, false);
    ECS::Utils::DestroyEntityHierarchy(R, Find(R, "arm"));
    ExpectRoundTrip(Variant, "delete the arm and keep the hand");
}

// A parent edit reaches every leaf the variant left alone and none it overrode.
TEST(PrefabVariantStress, ParentEditsFlowThroughUnlessOverridden)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    R.Get<STransformComponent>(Find(R, "arm")).SetLocalLocation(FVector3(9.0f, 9.0f, 9.0f));
    Variant->CaptureVariantDelta();

    ECS::FRegistry& P = Parent->Registry;
    P.Get<STransformComponent>(Find(P, "arm")).SetLocalLocation(FVector3(-5.0f, 0.0f, 0.0f));
    P.Get<STransformComponent>(Find(P, "leg")).SetLocalLocation(FVector3(0.0f, -7.0f, 0.0f));
    P.Get<SPointLightComponent>(Find(P, "hand")).Intensity = 11.0f;
    AddNode(P, "tail", FVector3(0.0f, 0.0f, -2.0f), Find(P, "root"));

    Variant->ResolveVariant();
    auto Got = Snapshot(Variant->Registry);
    EXPECT_TRUE(Near(Got[FName("arm")].Location, FVector3(9.0f))) << "an overridden leaf stays pinned";
    EXPECT_TRUE(Near(Got[FName("leg")].Location, FVector3(0.0f, -7.0f, 0.0f))) << "an untouched leaf follows the parent";
    EXPECT_EQ(Got[FName("hand")].Intensity, 11.0f);
    ASSERT_TRUE(Got.find(FName("tail")) != Got.end()) << "a node the parent adds later appears in the variant";
    EXPECT_EQ(Got[FName("tail")].Parent, FName("root"));
    EXPECT_TRUE(Near(Got[FName("tail")].Location, FVector3(0.0f, 0.0f, -2.0f)));
}

// The parent deleting a node the variant overrode, reparented under, or linked to must resolve without crashing.
TEST(PrefabVariantStress, ParentDeletingAnOverriddenNodeResolves)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    R.Get<SPointLightComponent>(Find(R, "hand")).Intensity = 42.0f;
    AddNode(R, "glove", FVector3(0.1f, 0.2f, 0.3f), Find(R, "hand"));
    R.Get<SPrefabLinkTestComponent>(Find(R, "arm")).Value = 5;
    Variant->CaptureVariantDelta();

    ECS::FRegistry& P = Parent->Registry;
    ECS::Utils::DestroyEntityHierarchy(P, Find(P, "arm"));
    Variant->ResolveVariant();
    ASSERT_FALSE(Variant->IsUnresolvedVariant());
    auto Got = Snapshot(Variant->Registry);
    EXPECT_TRUE(Got.find(FName("root")) != Got.end());
    EXPECT_TRUE(Got.find(FName("leg")) != Got.end());
    ASSERT_TRUE(Got.find(FName("glove")) != Got.end()) << "a node the variant added survives its anchor going away";
    EXPECT_TRUE(Near(Got[FName("glove")].Location, FVector3(0.1f, 0.2f, 0.3f)));
}

TEST(PrefabVariantStress, CyclesAreRefusedWithoutHanging)
{
    CPrefab* A = MakeParent();
    CPrefab* B = MakeVariantOf(A);
    A->ParentPrefab = B;
    B->ResolveVariant();
    EXPECT_TRUE(B->IsUnresolvedVariant());
    A->ResolveVariant();
    EXPECT_TRUE(A->IsUnresolvedVariant());

    CPrefab* Self = NewPrefab();
    Self->ParentPrefab = Self;
    Self->ResolveVariant();
    EXPECT_TRUE(Self->IsUnresolvedVariant());
    A->ParentPrefab = nullptr;
    Self->ParentPrefab = nullptr;
}

// The delta survives a trip through the registry serializer, handles included.
TEST(PrefabVariantStress, DeltaSurvivesSerialization)
{
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    R.Get<STransformComponent>(Find(R, "hand")).SetLocalLocation(FVector3(2.0f, 2.0f, 2.0f));
    const ECS::FEntity Added = AddNode(R, "added", FVector3(1.0f, 1.0f, 1.0f), Find(R, "leg"));
    R.Get<SPrefabLinkTestComponent>(Find(R, "arm")).Target = Added;
    const auto Edited = Snapshot(R);
    Variant->CaptureVariantDelta();

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        ASSERT_TRUE(ECS::Utils::SerializeRegistry(Writer, Variant->VariantDelta));
    }
    CPrefab* Loaded = NewPrefab();
    Loaded->ParentPrefab = Parent;
    {
        FMemoryReader Reader(Bytes);
        ASSERT_TRUE(ECS::Utils::SerializeRegistry(Reader, Loaded->VariantDelta));
    }
    Loaded->VariantOverriddenProperties = Variant->VariantOverriddenProperties;
    Loaded->VariantAddedComponents = Variant->VariantAddedComponents;
    Loaded->VariantRemovedComponents = Variant->VariantRemovedComponents;
    Loaded->VariantStructuralNodes = Variant->VariantStructuralNodes;
    Loaded->VariantRemovedEntities = Variant->VariantRemovedEntities;
    Loaded->ResolveVariant();
    ASSERT_FALSE(Loaded->IsUnresolvedVariant());
    EXPECT_EQ(Diff(Edited, Snapshot(Loaded->Registry)), "");
}

// Instances of a variant spawn with the variant's transforms and follow it through a refresh.
TEST(PrefabVariantStress, InstancesMatchTheVariantThroughRefresh)
{
    CWorld* World = MakeWorld();
    ASSERT_NE(World, nullptr);
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    ECS::FRegistry& R = Variant->Registry;
    R.Get<STransformComponent>(Find(R, "hand")).SetLocalLocation(FVector3(0.0f, 3.0f, 0.0f));
    Variant->CaptureVariantDelta();
    Variant->ResolveVariant();

    const ECS::FEntity Root = Variant->Instantiate(World, FTransform());
    ASSERT_NE(Root, ECS::NullEntity);
    EndFrame(World);
    ECS::FRegistry& W = ECS::GetWorldRegistry(*World);

    auto InstanceSnapshot = [&]()
    {
        THashMap<FName, FVector3> Out;
        W.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
        {
            if (const STransformComponent* T = W.TryGet<STransformComponent>(E))
            {
                Out[Inst.StableID] = T->GetLocalLocation();
            }
        });
        return Out;
    };

    auto Spawned = InstanceSnapshot();
    EXPECT_TRUE(Near(Spawned[FName("hand")], FVector3(0.0f, 3.0f, 0.0f)));
    EXPECT_TRUE(Near(Spawned[FName("arm")], FVector3(1.0f, 2.0f, 3.0f)));

    // The editor's commit order, which records the old values the refresh compares against.
    Variant->CapturePreCommitState();
    AddNode(R, "added", FVector3(0.0f, 0.0f, 4.0f), Find(R, "arm"));
    R.Get<STransformComponent>(Find(R, "leg")).SetLocalLocation(FVector3(5.0f, 0.0f, 0.0f));
    Variant->CaptureVariantDelta();
    Variant->ResolveVariant();
    Variant->RefreshInstance(World, Root);
    Variant->ClearPreCommitState();

    auto Refreshed = InstanceSnapshot();
    ASSERT_TRUE(Refreshed.find(FName("added")) != Refreshed.end()) << "a node added to the variant reaches its instances";
    EXPECT_TRUE(Near(Refreshed[FName("added")], FVector3(0.0f, 0.0f, 4.0f))) << "and spawns at its authored offset, got " << Refreshed[FName("added")].x << "," << Refreshed[FName("added")].y << "," << Refreshed[FName("added")].z;
    EXPECT_TRUE(Near(Refreshed[FName("leg")], FVector3(5.0f, 0.0f, 0.0f))) << "got " << Refreshed[FName("leg")].x << "," << Refreshed[FName("leg")].y << "," << Refreshed[FName("leg")].z;
    EXPECT_TRUE(Near(Refreshed[FName("hand")], FVector3(0.0f, 3.0f, 0.0f)));
}

// A parent edit reaches instances of its variants, not only the variant asset.
TEST(PrefabVariantStress, ParentEditReachesVariantInstances)
{
    CWorld* World = MakeWorld();
    CPrefab* Parent = MakeParent();
    CPrefab* Variant = MakeVariantOf(Parent);
    const ECS::FEntity Root = Variant->Instantiate(World, FTransform());
    ASSERT_NE(Root, ECS::NullEntity);
    EndFrame(World);
    ECS::FRegistry& W = ECS::GetWorldRegistry(*World);

    ECS::FRegistry& P = Parent->Registry;
    P.Get<STransformComponent>(Find(P, "leg")).SetLocalLocation(FVector3(0.0f, -6.0f, 0.0f));
    P.Get<SPointLightComponent>(Find(P, "hand")).Intensity = 8.0f;
    // What PropagateToVariants does for each variant, aimed at this test's world rather than the loaded ones.
    Variant->CapturePreCommitState();
    Variant->ResolveVariant();
    Variant->RefreshInstance(World, Root);
    Variant->ClearPreCommitState();

    bool bSawLeg = false;
    W.View<SPrefabInstanceComponent>().ForEach([&](ECS::FEntity E, const SPrefabInstanceComponent& Inst)
    {
        if (Inst.StableID == FName("leg"))
        {
            bSawLeg = true;
            EXPECT_TRUE(Near(W.Get<STransformComponent>(E).GetLocalLocation(), FVector3(0.0f, -6.0f, 0.0f))) << "the moved leg follows into the instance";
        }
        if (Inst.StableID == FName("hand"))
        {
            EXPECT_EQ(W.Get<SPointLightComponent>(E).Intensity, 8.0f);
        }
    });
    EXPECT_TRUE(bSawLeg);
}

namespace
{
    // Random editor-like edits against one prefab's registry. Every op keeps the registry a valid prefab.
    struct FFuzzer
    {
        std::mt19937 Rng;
        int32 NextID = 0;
        const char* Prefix;

        FFuzzer(uint32 Seed, const char* InPrefix) : Rng(Seed), Prefix(InPrefix) {}

        float Unit() { return std::uniform_real_distribution<float>(-4.0f, 4.0f)(Rng); }
        int32 Pick(int32 Count) { return std::uniform_int_distribution<int32>(0, Count - 1)(Rng); }

        ECS::FEntity Random(ECS::FRegistry& R)
        {
            TVector<ECS::FEntity> All;
            for (auto& [ID, E] : Index(R))
            {
                All.push_back(E);
            }
            return All.empty() ? ECS::NullEntity : All[Pick((int32)All.size())];
        }

        int32 LastOp = -1;
        std::string* Log = nullptr;

        void Note(ECS::FRegistry& R, const char* What, ECS::FEntity E, ECS::FEntity Other = ECS::NullEntity)
        {
            if (Log != nullptr)
            {
                *Log += std::string(Prefix) + " " + What + " " + StableIDOf(R, E).c_str() + (Other != ECS::NullEntity ? std::string(" -> ") + StableIDOf(R, Other).c_str() : std::string()) + "\n";
            }
        }

        void Step(ECS::FRegistry& R)
        {
            ECS::FEntity E = Random(R);
            const int32 Op = Pick(9);
            LastOp = Op;
            if (E == ECS::NullEntity || Op == 0)
            {
                const FName ID((std::string(Prefix) + std::to_string(NextID++)).c_str());
                ECS::FEntity Parent = Pick(4) == 0 ? ECS::NullEntity : E;
                const ECS::FEntity Made = AddNode(R, ID, FVector3(Unit(), Unit(), Unit()), Parent);
                Note(R, "add", Made, Parent);
                return;
            }
            switch (Op)
            {
            case 1:
            case 2:
                Note(R, "move", E);
                R.Get<STransformComponent>(E).SetLocalLocation(FVector3(Unit(), Unit(), Unit()));
                break;
            case 3:
                Note(R, "rotate", E);
                R.Get<STransformComponent>(E).SetLocalRotation(Math::Normalize(FQuat(Unit(), Unit(), Unit(), Unit() + 5.0f)));
                R.Get<STransformComponent>(E).SetLocalScale(FVector3(1.0f + 0.1f * Unit()));
                break;
            case 4:
                if (Index(R).size() > 2)
                {
                    Note(R, "delete", E);
                    ECS::Utils::DestroyEntityHierarchy(R, E);
                }
                break;
            case 5:
            {
                ECS::FEntity NewParent = Pick(3) == 0 ? ECS::NullEntity : Random(R);
                if (NewParent != E && (NewParent == ECS::NullEntity || !R.GetHierarchy().IsDescendantOf(NewParent, E)))
                {
                    Note(R, "reparent", E, NewParent);
                    ECS::Utils::ReparentEntity(R, E, NewParent, false);
                }
                break;
            }
            case 6:
                Note(R, "light", E);
                if (R.HasAll<SPointLightComponent>(E) && Pick(2) == 0)
                {
                    R.Remove<SPointLightComponent>(E);
                }
                else
                {
                    SPointLightComponent& Light = R.GetOrEmplace<SPointLightComponent>(E);
                    Light.Intensity = 1.0f + (float)Pick(50);
                    Light.LightColor = FVector3(0.5f, (float)Pick(3) * 0.25f, 1.0f);
                }
                break;
            case 7:
            {
                Note(R, "link", E);
                SPrefabLinkTestComponent& Link = R.GetOrEmplace<SPrefabLinkTestComponent>(E);
                Link.Value = Pick(1000);
                Link.Target = Pick(4) == 0 ? ECS::NullEntity : Random(R);
                break;
            }
            default:
                Note(R, "unlink", E);
                if (R.HasAll<SPrefabLinkTestComponent>(E))
                {
                    R.Remove<SPrefabLinkTestComponent>(E);
                }
                break;
            }
        }
    };
}

// Random edits at every level of a three deep variant chain, with each edited level checked after its capture.
TEST(PrefabVariantStress, RandomEditsRoundTripThroughAChain)
{
    constexpr int32 Seeds = 300;
    constexpr int32 Rounds = 16;
    int32 Failures = 0;
    for (uint32 Seed = 1; Seed <= Seeds && Failures < 5; ++Seed)
    {
        CPrefab* Base = MakeParent();
        CPrefab* Mid = MakeVariantOf(Base);
        CPrefab* Leaf = MakeVariantOf(Mid);
        CWorld* World = MakeWorld();
        const ECS::FEntity InstanceRoot = Leaf->Instantiate(World, FTransform());
        ECS::FRegistry& WorldRegistry = ECS::GetWorldRegistry(*World);
        EndFrame(World);
        FFuzzer BaseEdits(Seed * 7919u, "b");
        FFuzzer MidEdits(Seed * 104729u, "m");
        FFuzzer LeafEdits(Seed * 1299709u, "l");
        std::string Ops;
        BaseEdits.Log = &Ops;
        MidEdits.Log = &Ops;
        LeafEdits.Log = &Ops;

        for (int32 Round = 0; Round < Rounds && Failures < 5; ++Round)
        {
            // The values the instance last synced to, which is what the editor records before every commit.
            Leaf->CapturePreCommitState();
            const std::string Context = "seed " + std::to_string(Seed) + " round " + std::to_string(Round);

            auto Check = [&](ECS::FRegistry& R, const std::string& Where)
            {
                const std::string Bad = Degenerate(R);
                if (!Bad.empty())
                {
                    ADD_FAILURE() << Context << " " << Where << ": " << Bad << "\n" << Ops;
                    ++Failures;
                }
                return Bad.empty();
            };

            bool bOk = true;
            for (int32 I = 0; I < 3 && bOk; ++I)
            {
                BaseEdits.Step(Base->Registry);
                bOk = Check(Base->Registry, "base after op " + std::to_string(BaseEdits.LastOp));
            }
            Mid->ResolveVariant();
            bOk = bOk && Check(Mid->Registry, "mid after resolving onto a base edit");
            Leaf->ResolveVariant();
            bOk = bOk && Check(Leaf->Registry, "leaf after resolving onto a base edit");
            if (!bOk)
            {
                break;
            }

            for (int32 I = 0; I < 3 && bOk; ++I)
            {
                MidEdits.Step(Mid->Registry);
                bOk = Check(Mid->Registry, "mid after op " + std::to_string(MidEdits.LastOp));
            }
            if (!bOk)
            {
                break;
            }
            const auto MidEdited = Snapshot(Mid->Registry);
            Mid->CaptureVariantDelta();
            Mid->ResolveVariant();
            std::string Problem = Diff(MidEdited, Snapshot(Mid->Registry));
            if (!Problem.empty())
            {
                ADD_FAILURE() << Context << " mid: " << Problem << "\n" << Ops;
                ++Failures;
                break;
            }

            Leaf->ResolveVariant();
            bOk = Check(Leaf->Registry, "leaf after resolving onto a mid edit");
            for (int32 I = 0; I < 3 && bOk; ++I)
            {
                LeafEdits.Step(Leaf->Registry);
                bOk = Check(Leaf->Registry, "leaf after op " + std::to_string(LeafEdits.LastOp));
            }
            if (!bOk)
            {
                break;
            }
            const auto LeafEdited = Snapshot(Leaf->Registry);
            Leaf->CaptureVariantDelta();
            Leaf->ResolveVariant();
            Problem = Diff(LeafEdited, Snapshot(Leaf->Registry));
            if (!Problem.empty())
            {
                ADD_FAILURE() << Context << " leaf: " << Problem << "\n" << Ops;
                ++Failures;
                break;
            }

            // The placed instance is refreshed the way a save propagates, and must then match the variant node for node.
            // An empty prefab is refused on purpose, since it reads the same as one that failed to load.
            if (WorldRegistry.IsValid(InstanceRoot) && Leaf->Registry.NumEntities() > 0)
            {
                Leaf->ResolveVariant();
                Leaf->RefreshInstance(World, InstanceRoot);
                Leaf->ClearPreCommitState();
                EndFrame(World);
                Problem = DiffInstance(Snapshot(Leaf->Registry), WorldRegistry, InstanceRoot);
                if (!Problem.empty())
                {
                    ADD_FAILURE() << Context << " instance: " << Problem << "\n" << Ops;
                    ++Failures;
                    break;
                }
            }

            // Resolving again with nothing new must be a fixed point at both levels.
            Mid->ResolveVariant();
            Leaf->ResolveVariant();
            Problem = Diff(LeafEdited, Snapshot(Leaf->Registry));
            if (!Problem.empty())
            {
                ADD_FAILURE() << Context << " leaf re-resolve: " << Problem;
                ++Failures;
                break;
            }
        }
    }
}
