#include "FoliageEditMode.h"
#include "BrushModeUI.h"

#include <cmath>
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/ForestComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/TerrainSculptSystem.h"
#include "World/World.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        // In EBrushMode order, so a mode indexes its own entry.
        constexpr BrushUI::FTool kFoliageTools[] =
        {
            { LE_ICON_BRUSH,  "Paint", "Paint the species into the ground, where the forest grows it by its own rules. Alt erases.", IM_COL32(110, 215, 110, 255) },
            { LE_ICON_ERASER, "Erase", "Clear the species' painted cover and its placed plants. Alt paints.",                         IM_COL32(230, 110, 110, 255) },
            { LE_ICON_FOREST, "Place", "Drop single plants at the species' density, kept exactly where they land. Alt erases.",       IM_COL32(110, 165, 240, 255) },
        };

        constexpr float kMaxRadius = 1024.0f;

        bool RaycastGroundPlane(const FVector3& Origin, const FVector3& Dir, FVector3& OutHit)
        {
            if (std::abs(Dir.y) < 1e-6f)
            {
                return false;
            }
            const float T = -Origin.y / Dir.y;
            if (T <= 0.0f)
            {
                return false;
            }
            OutHit = Origin + Dir * T;
            return true;
        }
    }

    float FFoliageEditMode::RandFloat()
    {
        RngState ^= RngState << 13;
        RngState ^= RngState >> 17;
        RngState ^= RngState << 5;
        return float(RngState & 0xFFFFFFu) / float(0x1000000);
    }

    TVector<FFoliageEditMode::FGround> FFoliageEditMode::GatherGround(CWorld* World)
    {
        TVector<FGround> Ground;
        for (ECS::FEntity Entity : World->View<STerrainComponent, STransformComponent>())
        {
            const STerrainComponent& Terrain = World->GetComponent<STerrainComponent>(Entity);
            Ground.Add({ &Terrain, World->GetComponent<STransformComponent>(Entity).GetWorldLocation(), Terrain.TileWorldSize / (float)Math::Max(Terrain.Resolution - 1, 1) });
        }
        Algo::Sort(Ground, [](const FGround& A, const FGround& B) { return A.Spacing < B.Spacing; });
        return Ground;
    }

    bool FFoliageEditMode::ProjectToGround(const TVector<FGround>& Ground, float WorldX, float WorldZ, FVector3& OutPosition, FVector3& OutNormal)
    {
        for (const FGround& Terrain : Ground)
        {
            float Height = 0.0f;
            if (FTerrainSculptSystem::SampleHeight(*Terrain.Terrain, Terrain.Origin, WorldX, WorldZ, Height))
            {
                OutPosition = FVector3(WorldX, Height, WorldZ);
                OutNormal   = FTerrainSculptSystem::SampleNormal(*Terrain.Terrain, Terrain.Origin, WorldX, WorldZ);
                return true;
            }
        }
        if (!Ground.IsEmpty())
        {
            return false;
        }
        OutPosition = FVector3(WorldX, 0.0f, WorldZ);
        OutNormal   = FVector3(0.0f, 1.0f, 0.0f);
        return true;
    }

    bool FFoliageEditMode::RaycastGround(const TVector<FGround>& Ground, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit)
    {
        float Nearest = FLT_MAX;
        for (const FGround& Terrain : Ground)
        {
            FVector3 Hit;
            if (FTerrainSculptSystem::Raycast(*Terrain.Terrain, Terrain.Origin, RayOrigin, RayDir, Hit) && Math::Length(Hit - RayOrigin) < Nearest)
            {
                Nearest = Math::Length(Hit - RayOrigin);
                OutHit  = Hit;
            }
        }
        return Nearest < FLT_MAX || RaycastGroundPlane(RayOrigin, RayDir, OutHit);
    }

    FFoliageEditMode::FTarget FFoliageEditMode::FindForest(CWorld* World, const FVector3& Near)
    {
        FTarget First;
        for (ECS::FEntity Entity : World->View<SForestComponent, STransformComponent>())
        {
            FTarget Target{ &World->GetComponent<SForestComponent>(Entity), World->GetComponent<STransformComponent>(Entity).GetWorldLocation() };
            const float Half = Target.Forest->Extent * 0.5f;
            if (Math::Abs(Near.x - Target.Center.x) <= Half && Math::Abs(Near.z - Target.Center.z) <= Half)
            {
                return Target;
            }
            if (First.Forest == nullptr)
            {
                First = Target;
            }
        }
        return First;
    }

    FFoliageEditMode::FTarget FFoliageEditMode::CreateForest(CWorld* World, const TVector<FGround>& Ground)
    {
        // Sized to the coarsest terrain, so a backdrop around the playable one can be painted too.
        FVector3 Center(0.0f);
        float    Extent = 2048.0f;
        if (!Ground.IsEmpty())
        {
            Center = Ground.Last().Origin;
            Extent = Ground.Last().Terrain->TileWorldSize;
        }
        const ECS::FEntity Entity = World->ConstructEntity("Forest", FTransform(Center));
        SForestComponent& Forest = World->EmplaceComponent<SForestComponent>(Entity);
        Forest.Extent = Extent;
        return { &Forest, Center };
    }

    ECS::FEntity FFoliageEditMode::FindLegacyFoliage(CWorld* World)
    {
        for (ECS::FEntity Entity : World->View<SFoliageComponent>())
        {
            if (!World->HasComponent<FTransientEntityTag>(Entity))
            {
                return Entity;
            }
        }
        return ECS::NullEntity;
    }

    FFoliageEditMode::EBrushMode FFoliageEditMode::StrokeMode() const
    {
        if (!ImGui::GetIO().KeyAlt)
        {
            return Mode;
        }
        return Mode == EBrushMode::Erase ? EBrushMode::Paint : EBrushMode::Erase;
    }

    float FFoliageEditMode::BrushWeight(float Distance) const
    {
        return FTerrainSculptSystem::FalloffWeight(Distance, Radius, Falloff, FalloffShape);
    }

    void FFoliageEditMode::PaintDab(const FTarget& Target, const FVector3& Hit, float DeltaSeconds, float Sign)
    {
        // Full strength takes a cell from neutral to either limit in half a second.
        constexpr float kWeightPerSecond = 255.0f;
        constexpr int32 Cells    = SForestPaintChunk::kCells;
        constexpr float CellSize = SForestPaintChunk::kCellSize;
        SForestComponent& Forest = *Target.Forest;
        if (ActiveSpecies < 0 || ActiveSpecies >= Forest.Species.Num())
        {
            return;
        }

        const float Rate = kWeightPerSecond * Strength * DeltaSeconds * Sign;
        const FIntVector2 Lo = SForestComponent::GetChunkCoord(Target.Center, Hit.x - Radius, Hit.z - Radius);
        const FIntVector2 Hi = SForestComponent::GetChunkCoord(Target.Center, Hit.x + Radius, Hit.z + Radius);
        for (int32 CZ = Lo.y; CZ <= Hi.y; ++CZ)
        {
            for (int32 CX = Lo.x; CX <= Hi.x; ++CX)
            {
                const FVector2 ChunkMin(Target.Center.x + (float)CX * SForestPaintChunk::kSize, Target.Center.z + (float)CZ * SForestPaintChunk::kSize);
                SForestPaintChunk* Chunk = Forest.FindPaint(CX, CZ, ActiveSpecies);
                for (int32 IZ = 0; IZ < Cells; ++IZ)
                {
                    for (int32 IX = 0; IX < Cells; ++IX)
                    {
                        const float X = ChunkMin.x + ((float)IX + 0.5f) * CellSize;
                        const float Z = ChunkMin.y + ((float)IZ + 0.5f) * CellSize;
                        const float Weight = BrushWeight(Math::Length(FVector2(X - Hit.x, Z - Hit.z)));
                        if (Weight <= 0.0f)
                        {
                            continue;
                        }
                        // Rounded at random, so a slow brush still moves cells a fraction of a step each frame.
                        const float Delta = Rate * Weight;
                        const int32 Step = (int32)Math::Floor(Math::Abs(Delta) + RandFloat()) * (Delta < 0.0f ? -1 : 1);
                        if (Step == 0)
                        {
                            continue;
                        }
                        if (Chunk == nullptr)
                        {
                            Chunk = &Forest.FindOrAddPaint(CX, CZ, ActiveSpecies);
                        }
                        uint8& Cell = Chunk->Weights[IZ * Cells + IX];
                        Cell = (uint8)Math::Clamp((int32)Cell + Step, 0, 255);
                    }
                }
            }
        }
    }

    void FFoliageEditMode::PlaceDab(const FTarget& Target, const TVector<FGround>& Ground, const FVector3& Hit, float DeltaSeconds)
    {
        SForestComponent& Forest = *Target.Forest;
        if (ActiveSpecies < 0 || ActiveSpecies >= Forest.Species.Num())
        {
            return;
        }
        const SFoliageType& Type = Forest.Species[ActiveSpecies].Type;
        if (!Type.Mesh.IsValid())
        {
            return;
        }

        // The species' own density over the brush, scaled by strength, so a full-strength stroke matches what the rules would grow.
        const float Area = Math::Pi<float>() * Radius * Radius;
        PlaceAccumulator += Type.Density * Area * Strength * DeltaSeconds;
        const int32 Attempts = Math::Min((int32)PlaceAccumulator, 256);
        PlaceAccumulator -= (float)Attempts;

        const FVector3 Up(0.0f, 1.0f, 0.0f);
        for (int32 i = 0; i < Attempts; ++i)
        {
            const float Angle = RandFloat() * Math::TwoPi<float>();
            const float R     = Radius * std::sqrt(RandFloat());
            if (RandFloat() > BrushWeight(R))
            {
                continue;
            }

            FVector3 Position, Normal;
            if (!ProjectToGround(Ground, Hit.x + std::cos(Angle) * R, Hit.z + std::sin(Angle) * R, Position, Normal))
            {
                continue;
            }

            const FVector3 PlantUp = Math::Normalize(Math::Mix(Up, Normal, Math::Clamp(Type.AlignToNormal, 0.0f, 1.0f)));
            const float    Yaw     = Type.bRandomYaw ? RandFloat() * Math::TwoPi<float>() : 0.0f;

            SFoliageInstance& Plant = Forest.Placed.EmplaceGetRef();
            Plant.Position  = Position + PlantUp * Type.ZOffset;
            Plant.Scale     = FVector3(Math::Mix(Type.ScaleMin, Type.ScaleMax, RandFloat()));
            Plant.TypeIndex = ActiveSpecies;
            Plant.SetRotationQuat(Math::RotationBetween(Up, PlantUp) * Math::AngleAxis(Yaw, Up));
        }
    }

    void FFoliageEditMode::ConvertLegacyFoliage(CWorld* World, ECS::FEntity Legacy)
    {
        RunModeTransaction("Move Foliage Into Forest", [&]
        {
            const SFoliageComponent& Foliage = World->GetComponent<SFoliageComponent>(Legacy);
            FTarget Target = FindForest(World, Foliage.Instances.IsEmpty() ? FVector3(0.0f) : Foliage.Instances[0].Position);
            if (Target.Forest == nullptr)
            {
                Target = CreateForest(World, GatherGround(World));
            }

            // Painted types arrive as species the rules leave out, so only the plants that were painted show.
            const int32 First = Target.Forest->Species.Num();
            for (const SFoliageType& Type : Foliage.Types)
            {
                SForestSpecies& Species = Target.Forest->Species.EmplaceGetRef();
                Species.Type     = Type;
                Species.Coverage = 0.0f;
            }
            for (SFoliageInstance Plant : Foliage.Instances)
            {
                if (Foliage.IsValidType(Plant.TypeIndex))
                {
                    Plant.TypeIndex += First;
                    Target.Forest->Placed.Add(Plant);
                }
            }
            World->DestroyEntity(Legacy);
        });
        BoundSpeciesData = nullptr;
    }

    void FFoliageEditMode::OnExit(CWorld* World)
    {
        if (bTransactionOpen && Context)
        {
            Context->EndModeTransaction("Foliage Edit");
        }
        bTransactionOpen = false;
        bHitValid        = false;
        PlaceAccumulator = 0.0f;
    }

    void FFoliageEditMode::DrawToolbar(CWorld* World, float ButtonSize)
    {
        if (!World)
        {
            return;
        }

        ImGui::SameLine();
        if (ImGui::Button(bShowSettings ? "Hide Brush" : "Brush", ImVec2(0, ButtonSize)))
        {
            bShowSettings = !bShowSettings;
        }

        if (!bShowSettings)
        {
            return;
        }

        // The width comes from the species' asset picker, whose thumbnail eats 64 pixels before the path field.
        BrushUI::BeginPanel("##FoliageBrushSettings", 460.0f);

        int32 Tool = int32(Mode);
        if (BrushUI::ToolPalette(kFoliageTools, IM_ARRAYSIZE(kFoliageTools), Tool))
        {
            Mode = EBrushMode(Tool);
        }
        BrushUI::BrushSection(Radius, kMaxRadius, Strength, Falloff, FalloffShape, kFoliageTools[int32(Mode)].Color);

        const FTarget Target = FindForest(World, LastHit);
        DrawSpeciesPanel(World, Target);

        const ECS::FEntity Legacy = FindLegacyFoliage(World);
        if (Legacy != ECS::NullEntity)
        {
            ImGui::Separator();
            ImGui::TextWrapped("This level has painted foliage outside any forest.");
            if (ImGui::Button("Move It Into The Forest"))
            {
                ConvertLegacyFoliage(World, Legacy);
            }
        }

        ImGui::Separator();
        BrushUI::Hint("[ ] size   Shift+[ ] strength   Alt inverts");
        ImGui::End();
    }

    void FFoliageEditMode::RebindSpeciesSettings(SForestComponent* Forest)
    {
        void* Desired = (Forest != nullptr && ActiveSpecies >= 0 && ActiveSpecies < Forest->Species.Num()) ? (void*)&Forest->Species[ActiveSpecies] : nullptr;
        if (Desired == BoundSpeciesData)
        {
            return;
        }

        BoundSpeciesData = Desired;
        if (Desired == nullptr)
        {
            SpeciesSettings.SetObject(nullptr, nullptr);
            return;
        }
        SpeciesSettings.SetObject(Desired, SForestSpecies::StaticStruct());
        SpeciesSettings.SetShowSearchBar(false);
        SpeciesSettings.SetStartEditCallback([this](const FPropertyChangedEvent&)
        {
            if (Context)
            {
                Context->BeginModeTransaction();
            }
        });
        SpeciesSettings.SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            if (Context)
            {
                Context->EndModeTransaction("Edit Forest Species");
            }
        });
    }

    void FFoliageEditMode::DrawSpeciesPanel(CWorld* World, const FTarget& Target)
    {
        ImGui::SeparatorText("Species");

        SForestComponent* Forest = Target.Forest;
        const int32 SpeciesCount = Forest != nullptr ? Forest->Species.Num() : 0;

        TVector<int32> PlacedCounts;
        TVector<int32> PaintedChunks;
        PlacedCounts.SetNum(Math::Max(SpeciesCount, 1), 0);
        PaintedChunks.SetNum(Math::Max(SpeciesCount, 1), 0);
        if (Forest != nullptr)
        {
            for (const SFoliageInstance& Plant : Forest->Placed)
            {
                if (Plant.TypeIndex >= 0 && Plant.TypeIndex < SpeciesCount)
                {
                    ++PlacedCounts[Plant.TypeIndex];
                }
            }
            for (const SForestPaintChunk& Chunk : Forest->Paint)
            {
                if (Chunk.Species >= 0 && Chunk.Species < SpeciesCount)
                {
                    ++PaintedChunks[Chunk.Species];
                }
            }
        }

        const float RowHeight  = ImGui::GetTextLineHeightWithSpacing();
        const float ListHeight = Math::Clamp(RowHeight * (float)Math::Max(SpeciesCount, 1) + 8.0f, RowHeight + 8.0f, RowHeight * 8.0f);
        ImGui::BeginChild("##forestspecies", ImVec2(-FLT_MIN, ListHeight), ImGuiChildFlags_Borders);

        if (SpeciesCount == 0)
        {
            ImGui::TextDisabled(Forest == nullptr ? "No forest yet. Add a species to make one." : "No species yet.");
        }

        for (int32 i = 0; i < SpeciesCount; ++i)
        {
            const SFoliageType& Type = Forest->Species[i].Type;
            ImGui::PushID(i);

            // A species with no mesh grows nothing, so it is called out rather than looking ready.
            const bool bReady = Type.Mesh.IsValid();
            FString Label = Type.Name.IsEmpty() ? FString("Unnamed") : Type.Name;
            if (bReady)
            {
                Label += "  (";
                Label += *Type.Mesh->GetName();
                Label += ")";
            }
            else
            {
                Label += "  (no mesh)";
            }

            ImGui::PushStyleColor(ImGuiCol_Text, bReady ? IM_COL32(225, 225, 230, 255) : IM_COL32(210, 150, 110, 255));
            if (ImGui::Selectable(*Label, ActiveSpecies == i))
            {
                ActiveSpecies = i;
            }
            ImGui::PopStyleColor();

            const FString Counts = *Format("{} placed, {} painted", PlacedCounts[i], PaintedChunks[i]);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(*Counts).x);
            ImGui::TextDisabled("%s", *Counts);

            ImGui::PopID();
        }

        ImGui::EndChild();

        if (ImGui::Button("Add"))
        {
            RunModeTransaction("Add Forest Species", [&]
            {
                SForestComponent& Owner = Forest != nullptr ? *Forest : *CreateForest(World, GatherGround(World)).Forest;
                SForestSpecies& Species = Owner.Species.EmplaceGetRef();
                Species.Type.Name = Format("Species{}", Owner.Species.Num() - 1);
                // Painted in by hand, so it grows nowhere until a stroke says otherwise.
                Species.Coverage = 0.0f;
                ActiveSpecies = Owner.Species.Num() - 1;
            });
            BoundSpeciesData = nullptr;
            return;
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(Forest == nullptr || ActiveSpecies < 0 || ActiveSpecies >= SpeciesCount);
        if (ImGui::Button("Duplicate"))
        {
            RunModeTransaction("Duplicate Forest Species", [&]
            {
                // Copied by value first, since adding can reallocate and dangle the reference.
                const SForestSpecies Source = Forest->Species[ActiveSpecies];
                Forest->Species.Add(Source);
                Forest->Species.Last().Type.Name = Source.Type.Name + " Copy";
                ActiveSpecies = Forest->Species.Num() - 1;
            });
            BoundSpeciesData = nullptr;
        }
        ImGui::SameLine();

        if (ImGui::Button("Remove"))
        {
            RunModeTransaction("Remove Forest Species", [&]
            {
                Forest->RemoveSpecies(ActiveSpecies);
                ActiveSpecies = Math::Min(ActiveSpecies, Forest->Species.Num() - 1);
            });
            BoundSpeciesData = nullptr;
        }
        ImGui::EndDisabled();

        ActiveSpecies = Math::Clamp(ActiveSpecies, 0, Math::Max(SpeciesCount - 1, 0));
        RebindSpeciesSettings(Forest);

        if (Forest == nullptr || ActiveSpecies >= Forest->Species.Num())
        {
            ImGui::TextDisabled("Add a species to start painting.");
            return;
        }

        ImGui::SeparatorText("Species Settings");
        SpeciesSettings.DrawTree();
    }

    void FFoliageEditMode::Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize)
    {
        bHitValid      = false;
        ReadoutSeconds = Math::Max(ReadoutSeconds - ImGui::GetIO().DeltaTime, 0.0f);

        // Closes the stroke's transaction the moment the button is released.
        if (bTransactionOpen && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (Context)
            {
                Context->EndModeTransaction("Foliage Edit");
            }
            bTransactionOpen = false;
            PlaceAccumulator = 0.0f;
        }

        if (!World)
        {
            return;
        }

        if (BrushUI::HandleBrushKeys(Radius, kMaxRadius, Strength))
        {
            ReadoutSeconds = 1.5f;
        }

        if (!bViewportHovered)
        {
            return;
        }

        const ImVec2 MousePos = ImGui::GetMousePos();
        const ImVec2 Local    = ImVec2(MousePos.x - ViewportScreenOrigin.x, MousePos.y - ViewportScreenOrigin.y);
        if (Local.x < 0.0f || Local.y < 0.0f || Local.x > ViewportSize.x || Local.y > ViewportSize.y)
        {
            return;
        }

        FVisualizerView View;
        View.SetFromCamera(Camera, ViewportScreenOrigin, ViewportSize);
        FVector3 RayOrigin, RayDir;
        View.ScreenToRay(MousePos, RayOrigin, RayDir);

        const TVector<FGround> Ground = GatherGround(World);
        FVector3 Hit;
        if (!RaycastGround(Ground, RayOrigin, RayDir, Hit))
        {
            return;
        }

        LastHit   = Hit;
        bHitValid = true;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            return;
        }

        const FTarget Target = FindForest(World, Hit);
        if (Target.Forest == nullptr)
        {
            return;
        }

        if (!bTransactionOpen && Context)
        {
            Context->BeginModeTransaction();
            bTransactionOpen = true;
        }

        const float DeltaSeconds = Math::Max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);
        switch (StrokeMode())
        {
        case EBrushMode::Paint:
            PaintDab(Target, Hit, DeltaSeconds, 1.0f);
            break;
        case EBrushMode::Erase:
            PaintDab(Target, Hit, DeltaSeconds, -1.0f);
            Target.Forest->Placed.RemoveAll([&](const SFoliageInstance& Plant)
            {
                return Plant.TypeIndex == ActiveSpecies && Math::Length(FVector2(Plant.Position.x - Hit.x, Plant.Position.z - Hit.z)) <= Radius;
            });
            break;
        case EBrushMode::Place:
            PlaceDab(Target, Ground, Hit, DeltaSeconds);
            break;
        }
    }

    void FFoliageEditMode::DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera)
    {
        if (!bHitValid || !World)
        {
            return;
        }

        FVisualizerView View;
        View.SetFromCamera(Camera, ViewportScreenOrigin, ViewportSize);
        ImVec2 Center;
        if (!View.WorldToScreen(LastHit, Center))
        {
            return;
        }

        const EBrushMode Stroke = StrokeMode();
        const ImU32      Color  = kFoliageTools[int32(Stroke)].Color;

        // Lifted off the ground a little, so the ring is not lost in the grass it paints.
        const TVector<FGround> Ground = GatherGround(World);
        auto OnGround = [&](FVector3& P)
        {
            FVector3 Normal;
            ProjectToGround(Ground, P.x, P.z, P, Normal);
            P.y += 0.1f;
        };

        ImDrawList* Draw = ImGui::GetWindowDrawList();
        BrushUI::DrawRing(Draw, View, LastHit, Radius, BrushUI::ScaleColor(Color, 1.0f, 0.9f), 2.0f, OnGround);
        // The core, inside which the brush works at full strength.
        if (Falloff > 0.02f)
        {
            BrushUI::DrawRing(Draw, View, LastHit, Radius * (1.0f - Falloff), BrushUI::ScaleColor(Color, 1.0f, 0.45f), 1.0f, OnGround);
        }
        Draw->AddCircleFilled(Center, 3.0f, IM_COL32(255, 255, 255, 220));

        // What the brush will do and how strongly, shown when Alt flips it or a shortcut just changed it.
        char Readout[64] = {};
        if (ReadoutSeconds > 0.0f)
        {
            ImFormatString(Readout, sizeof(Readout), "%.0f m  %.0f%%", Radius, Strength * 100.0f);
        }
        else if (Stroke != Mode)
        {
            ImFormatString(Readout, sizeof(Readout), "%s", kFoliageTools[int32(Stroke)].Label);
        }
        if (Readout[0] != '\0')
        {
            BrushUI::CursorLabel(Draw, Center, Readout, Color);
        }
    }
}
