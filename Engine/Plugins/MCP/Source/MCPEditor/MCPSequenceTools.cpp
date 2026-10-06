#include "MCPSequenceTools.h"
#include "World/ECS/Registry.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentEntityToken.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Agent/AgentToolSchema.h"
#include "Asset/AssetOps.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Audio/SoundBase.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Physics/PhysicsLibrary.h"
#include "Session/SessionOps.h"
#include "Tools/Screenshot/MovieCapture.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/SequenceLibrary.h"
#include "World/World.h"
#include "World/WorldManager.h"

namespace Lumina::MCP
{
    // Named because the other tool files share these Register names and can land in one unity blob.
    namespace SequenceTools
    {
        struct FTrackType
        {
            const char* Name;
            CClass*     (*Class)();
            bool        bNeedsBinding;
            const char* Description;
        };

        const FTrackType TrackTypes[] =
        {
            { "Transform",    &CSequenceTrack_Transform::StaticClass,    true,  "Keys the binding's Location, Rotation (pitch, yaw, roll degrees) and Scale." },
            { "CameraCut",    &CSequenceTrack_CameraCut::StaticClass,    false, "Which camera binding is live over each time range, with optional blends." },
            { "Event",        &CSequenceTrack_Event::StaticClass,        false, "Named moments gameplay reacts to through CSequenceLibrary.ConsumeEvents." },
            { "Property",     &CSequenceTrack_Property::StaticClass,     true,  "A curve driving one reflected number on one component, such as SCameraComponent FOV." },
            { "Audio",        &CSequenceTrack_Audio::StaticClass,        false, "Sounds started as the playhead passes them, at the binding when there is one." },
            { "Fade",         &CSequenceTrack_Fade::StaticClass,         false, "Fades the whole picture toward Color, holding across cuts." },
            { "TimeDilation", &CSequenceTrack_TimeDilation::StaticClass, false, "World time scale for slow motion while the edit keeps real time." },
            { "LookAt",       &CSequenceTrack_LookAt::StaticClass,       true,  "Aims the binding at another binding or a fixed point, optionally pulling focus." },
            { "CameraShake",  &CSequenceTrack_CameraShake::StaticClass,  false, "A looping drift over the live view for a handheld feel, scaled by Intensity." },
        };

        const FTrackType* FindTrackType(FStringView Name)
        {
            for (const FTrackType& Type : TrackTypes)
            {
                if (Name == Type.Name)
                {
                    return &Type;
                }
            }
            return nullptr;
        }

        FString TrackTypeName(const CSequenceTrack* Track)
        {
            for (const FTrackType& Type : TrackTypes)
            {
                if (Track->GetClass() == Type.Class())
                {
                    return Type.Name;
                }
            }
            return FString(Track->GetClass()->GetName().ToString().c_str());
        }

        const char* BindingKindName(ESequenceBindingKind Kind)
        {
            switch (Kind)
            {
                case ESequenceBindingKind::Spawn:  return "Spawn";
                case ESequenceBindingKind::Camera: return "Camera";
                default:                           return "Possess";
            }
        }

        // Held for the session, since a loaded asset nothing references is freed the moment a player lets go of it, edits and all.
        TVector<TStrongObjectPtr<CSequence>>& TouchedSequences()
        {
            static TVector<TStrongObjectPtr<CSequence>> Touched;
            return Touched;
        }

        bool ResolveSequence(const FString& Reference, CSequence*& Out, FString& OutError)
        {
            CObject* Asset = nullptr;
            if (!Agent::ResolveAssetObject(FStringView(Reference), Asset, OutError))
            {
                return false;
            }

            Out = Cast<CSequence>(Asset);
            if (Out == nullptr)
            {
                OutError = Lumina::Format("'{}' is a {}, not a sequence.", Reference, Asset->GetClass()->GetName());
                return false;
            }

            TVector<TStrongObjectPtr<CSequence>>& Touched = TouchedSequences();
            if (std::find_if(Touched.begin(), Touched.end(), [Out](const TStrongObjectPtr<CSequence>& Held) { return Held.Get() == Out; }) == Touched.end())
            {
                Touched.push_back(Out);
            }
            return true;
        }

        void MarkDirty(CSequence* Sequence)
        {
            if (CPackage* Package = Sequence->GetPackage())
            {
                Package->MarkDirty();
            }
        }

        int32 FindBinding(const CSequence* Sequence, FStringView Name)
        {
            for (int32 Index = 0; Index < (int32)Sequence->Bindings.size(); ++Index)
            {
                if (Sequence->Bindings[Index].Name.ToString() == Name)
                {
                    return Index;
                }
            }
            return INDEX_NONE;
        }

        // A binding by name or by index, where empty means none.
        bool ParseBinding(const CSequence* Sequence, const FString& Text, int32& Out, FString& OutError)
        {
            Out = INDEX_NONE;
            if (Text.empty())
            {
                return true;
            }

            Out = FindBinding(Sequence, FStringView(Text));
            if (Out != INDEX_NONE)
            {
                return true;
            }

            char* End = nullptr;
            const long Parsed = std::strtol(Text.c_str(), &End, 10);
            if (End != nullptr && *End == '\0' && Parsed >= 0 && Parsed < (long)Sequence->Bindings.size())
            {
                Out = (int32)Parsed;
                return true;
            }

            OutError = Lumina::Format("The sequence has no binding '{}'. sequence.describe lists them.", Text);
            return false;
        }

        FString BindingName(const CSequence* Sequence, int32 Index)
        {
            return Index >= 0 && Index < (int32)Sequence->Bindings.size() ? FString(Sequence->Bindings[Index].Name.ToString().c_str()) : FString();
        }

        bool CheckTrack(const CSequence* Sequence, int32 Track, FString& OutError)
        {
            if (Track < 0 || Track >= (int32)Sequence->Tracks.size() || Sequence->Tracks[Track] == nullptr)
            {
                OutError = Lumina::Format("Track {} does not exist. This sequence has {} track(s).", Track, Sequence->Tracks.size());
                return false;
            }
            return true;
        }

        CSequenceTrack* AddTrack(CSequence* Sequence, CClass* Class, int32 Binding)
        {
            CSequenceTrack* Track = NewObject<CSequenceTrack>(Class, Sequence->GetPackage());
            Track->BindingIndex = Binding;
            Sequence->Tracks.push_back(Track);
            return Track;
        }

        template<typename T>
        T* FindTrack(CSequence* Sequence, int32 Binding, int32* OutIndex = nullptr)
        {
            for (int32 Index = 0; Index < (int32)Sequence->Tracks.size(); ++Index)
            {
                T* Track = Cast<T>(Sequence->Tracks[Index].Get());
                if (Track != nullptr && Track->BindingIndex == Binding)
                {
                    if (OutIndex != nullptr)
                    {
                        *OutIndex = Index;
                    }
                    return Track;
                }
            }
            return nullptr;
        }

        template<typename T>
        T* FindOrAddTrack(CSequence* Sequence, int32 Binding, int32& OutIndex)
        {
            if (T* Existing = FindTrack<T>(Sequence, Binding, &OutIndex))
            {
                return Existing;
            }
            OutIndex = (int32)Sequence->Tracks.size();
            return static_cast<T*>(AddTrack(Sequence, T::StaticClass(), Binding));
        }

        int32 IndexOfTrack(const CSequence* Sequence, const CSequenceTrack* Track)
        {
            for (int32 Index = 0; Index < (int32)Sequence->Tracks.size(); ++Index)
            {
                if (Sequence->Tracks[Index].Get() == Track)
                {
                    return Index;
                }
            }
            return INDEX_NONE;
        }

        bool ParseJson(const FString& Text, nlohmann::json& Out, FString& OutError)
        {
            Out = nlohmann::json::parse(Text.c_str(), nullptr, false);
            if (Out.is_discarded())
            {
                OutError = Lumina::Format("'{}' is not valid JSON.", Text);
                return false;
            }
            return true;
        }

        bool ReadVector(const nlohmann::json& Value, FVector3& Out)
        {
            if (Value.is_array() && Value.size() == 3 && Value[0].is_number() && Value[1].is_number() && Value[2].is_number())
            {
                Out = FVector3(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
                return true;
            }
            if (Value.is_object() && Value.contains("X") && Value.contains("Y") && Value.contains("Z"))
            {
                Out = FVector3(Value["X"].get<float>(), Value["Y"].get<float>(), Value["Z"].get<float>());
                return true;
            }
            return false;
        }

        bool ParseInterp(FStringView Text, ECurveInterpMode& Out)
        {
            if (Text.empty() || Text == "Cubic")  { Out = ECurveInterpMode::Cubic; return true; }
            if (Text == "Linear")                 { Out = ECurveInterpMode::Linear; return true; }
            if (Text == "Constant")               { Out = ECurveInterpMode::Constant; return true; }
            return false;
        }

        void Key(SKeyedCurve& Curve, float Time, float Value, ECurveInterpMode Mode)
        {
            const int32 Index = Curve.UpdateOrAddKey(Time, Value);
            Curve.Keys[Index].InterpMode = Mode;
        }

        void KeyVector(SSequenceVectorCurve& Curve, float Time, const FVector3& Value, ECurveInterpMode Mode)
        {
            Curve.bEnabled = true;
            Key(Curve.X.Curve, Time, Value.x, Mode);
            Key(Curve.Y.Curve, Time, Value.y, Mode);
            Key(Curve.Z.Curve, Time, Value.z, Mode);
        }

        void FinishVector(SSequenceVectorCurve& Curve, bool bAngles)
        {
            if (bAngles)
            {
                Curve.UnwindAngles();
            }
            Curve.X.Curve.ComputeAutoTangents();
            Curve.Y.Curve.ComputeAutoTangents();
            Curve.Z.Curve.ComputeAutoTangents();
        }

        FVector3 LookAtEuler(const FVector3& From, const FVector3& To)
        {
            return Math::Degrees(Math::YawFirstEulerAngles(Math::FindLookAtRotation(To, From)));
        }

        CWorld* FindWorld(bool bGame)
        {
            if (GWorldManager == nullptr)
            {
                return nullptr;
            }

            for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
            {
                if (!Context || !Context->World.IsValid())
                {
                    continue;
                }
                const bool bIsGame = Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation;
                if (bIsGame == bGame)
                {
                    return Context->World.Get();
                }
            }
            return nullptr;
        }

        CWorld* RequireGameWorld(FString& OutError)
        {
            CWorld* World = FindWorld(true);
            if (World == nullptr)
            {
                OutError = "No game is playing. Start one with editor.play first, since sequences run in the game world.";
            }
            return World;
        }

        template<typename TFunc>
        void ForEachPlayer(CWorld* World, const CSequence* Only, TFunc&& Visit)
        {
            TVector<ECS::FEntity> Players;
            auto View = World->View<SSequencePlayerComponent>();
            for (ECS::FEntity Entity : View)
            {
                const SSequencePlayerComponent& Player = View.Get<SSequencePlayerComponent>(Entity);
                if (Only == nullptr || Player.Sequence.Get() == Only)
                {
                    Players.push_back(Entity);
                }
            }
            for (ECS::FEntity Entity : Players)
            {
                Visit(Entity);
            }
        }

        FString DescribeValues(CSequenceTrack* Track, bool bSummary)
        {
            nlohmann::json Values;
            if (!Agent::WriteStruct(Track->GetClass(), Track, Values).IsValid() || !Values.is_object())
            {
                return FString();
            }

            if (bSummary)
            {
                // A curve reads as its key count and range, which is what matters when surveying a long edit.
                std::function<void(nlohmann::json&)> Collapse = [&Collapse](nlohmann::json& Node)
                {
                    if (!Node.is_object())
                    {
                        return;
                    }
                    if (Node.contains("Keys") && Node["Keys"].is_array())
                    {
                        const nlohmann::json& Keys = Node["Keys"];
                        nlohmann::json Summary = { { "KeyCount", Keys.size() } };
                        if (!Keys.empty())
                        {
                            Summary["From"] = Keys.front().value("Time", 0.0f);
                            Summary["To"] = Keys.back().value("Time", 0.0f);
                        }
                        Node = Summary;
                        return;
                    }
                    for (auto& Entry : Node.items())
                    {
                        Collapse(Entry.value());
                    }
                };
                Collapse(Values);
            }
            return FString(Values.dump().c_str());
        }

        constexpr float NoGround = -100000.0f;

        // The first surface under a point, found in the playing world when there is one and the editor's otherwise.
        float GroundHeight(float X, float Z)
        {
            FString Error;
            CWorld* World = FindWorld(true);
            if (World == nullptr)
            {
                World = SessionOps::GetSceneWorld(Error);
            }
            if (World == nullptr)
            {
                return NoGround;
            }

            const SRayResult Hit = CPhysicsLibrary::Raycast(World, FVector3(X, 5000.0f, Z), FVector3(X, -5000.0f, Z), ECS::NullEntity);
            return Hit.bHit ? Hit.Location.y : NoGround;
        }

        // With Ground set a key's heights are above the terrain, so a shot can be placed without knowing the island's elevation.
        void Ground(FVector3& Point, bool bGround)
        {
            if (bGround)
            {
                const float Height = GroundHeight(Point.x, Point.z);
                Point.y += Height > NoGround ? Height : 0.0f;
            }
        }

        void FillStatus(const MovieCapture::FStatus& Status, SMovieStatusResult& Out)
        {
            Out.bActive       = Status.bActive;
            Out.bFinished     = Status.bFinished;
            Out.FramesWritten = (int32)Status.FramesWritten;
            Out.FrameCount    = (int32)Status.FrameCount;
            Out.OutputPath    = Status.OutputPath;
            Out.PngDirectory  = Status.PngDirectory;
            Out.SourceWidth   = (int32)Status.SourceWidth;
            Out.SourceHeight  = (int32)Status.SourceHeight;
            Out.Error         = Status.Error;
            Out.bAudio        = Status.bAudio;
        }

        void RegisterCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateSequenceParams, SCreateSequenceResult>(
                Owner, "sequence.create",
                "Create an empty cinematic sequence asset. Build it with sequence.shot, add_event, add_track and set_keys, then play it with sequence.play or render it with movie.render.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateSequenceParams& In, SCreateSequenceResult& Out)
                {
                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("A sequence needs a name.");
                    }
                    if (!AssetOps::IsAssetLocation(FStringView(In.Folder)))
                    {
                        return Agent::FToolResult::Error("Sequences belong under /Game/Content, since nothing scans for assets elsewhere.");
                    }
                    if (!VFS::IsDirectory(In.Folder))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} is not a folder. Use assets.create_folder first.", In.Folder));
                    }

                    FFixedString Path = Paths::Combine(FStringView(In.Folder), FStringView(In.Name));
                    CPackage::AddPackageExt(Path);
                    if (VFS::Exists(Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} already exists.", Path));
                    }

                    CSequence* Sequence = CFactory::CreateNewOf<CSequence>(FStringView(Path.c_str(), Path.size()));
                    if (Sequence == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not create a sequence at {}.", Path));
                    }

                    Sequence->Duration = Math::Max(In.Duration, 0.1f);
                    Sequence->FrameRate = Math::Clamp(In.FrameRate, 1, 240);
                    Sequence->LetterboxAspect = Math::Clamp(In.LetterboxAspect, 0.0f, 4.0f);

                    if (!CPackage::SavePackage(Sequence->GetPackage(), Path))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Could not save {}.", Path));
                    }

                    FAssetRegistry::Get().AssetCreated(Sequence);
                    Out.Path = FString(Path.c_str());
                    Out.Guid = FString(Sequence->GetGUID().ToString().c_str());
                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterSave(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceRefParams, SCreateSequenceResult>(
                Owner, "sequence.save",
                "Write a sequence's edits to disk.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceRefParams& In, SCreateSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CPackage* Package = Sequence->GetPackage();
                    const FFixedString Path = Package != nullptr ? Package->GetPackagePath() : FFixedString();
                    if (Package == nullptr || !CPackage::SavePackage(Package, FStringView(Path.c_str(), Path.size())))
                    {
                        return Agent::FToolResult::Error("Could not save the sequence.");
                    }

                    Out.Path = FString(Path.c_str());
                    Out.Guid = FString(Sequence->GetGUID().ToString().c_str());
                    return Agent::FToolResult::Ok(Lumina::Format("Saved {}.", Out.Path));
                });
        }

        void RegisterSettings(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceSettingsParams, SDescribeSequenceResult>(
                Owner, "sequence.set_settings",
                "Change a sequence's length, frame rate or letterbox.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceSettingsParams& In, SDescribeSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Duration >= 0.0f)
                    {
                        Sequence->Duration = Math::Max(In.Duration, 0.1f);
                    }
                    if (In.FrameRate > 0)
                    {
                        Sequence->FrameRate = Math::Clamp(In.FrameRate, 1, 240);
                    }
                    if (In.LetterboxAspect >= 0.0f)
                    {
                        Sequence->LetterboxAspect = Math::Min(In.LetterboxAspect, 4.0f);
                    }
                    MarkDirty(Sequence);

                    Out.Name = FString(Sequence->GetName().ToString().c_str());
                    Out.Duration = Sequence->Duration;
                    Out.FrameRate = Sequence->FrameRate;
                    Out.LetterboxAspect = Sequence->LetterboxAspect;
                    return Agent::FToolResult::Ok("Updated.");
                });
        }

        void RegisterDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeSequenceParams, SDescribeSequenceResult>(
                Owner, "sequence.describe",
                "Report a sequence's settings, bindings and every track with its values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeSequenceParams& In, SDescribeSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(Sequence->GetName().ToString().c_str());
                    Out.Duration = Sequence->Duration;
                    Out.FrameRate = Sequence->FrameRate;
                    Out.LetterboxAspect = Sequence->LetterboxAspect;

                    for (int32 Index = 0; Index < (int32)Sequence->Bindings.size(); ++Index)
                    {
                        const SSequenceBinding& Binding = Sequence->Bindings[Index];
                        SSequenceBindingInfo Info;
                        Info.Index = Index;
                        Info.Name = FString(Binding.Name.ToString().c_str());
                        Info.Kind = BindingKindName(Binding.Kind);
                        if (Binding.SpawnPrefab.IsValid())
                        {
                            Info.Prefab = FString(Binding.SpawnPrefab->GetGUID().ToString().c_str());
                        }
                        Out.Bindings.push_back(Move(Info));
                    }

                    for (int32 Index = 0; Index < (int32)Sequence->Tracks.size(); ++Index)
                    {
                        CSequenceTrack* Track = Sequence->Tracks[Index].Get();
                        if (Track == nullptr)
                        {
                            continue;
                        }
                        SSequenceTrackInfo Info;
                        Info.Index = Index;
                        Info.Type = TrackTypeName(Track);
                        Info.Binding = BindingName(Sequence, Track->BindingIndex);
                        Info.bEnabled = Track->bEnabled;
                        Info.Values = DescribeValues(Track, In.bSummary);
                        Out.Tracks.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} binding(s), {} track(s), {:.2f}s.", Out.Bindings.size(), Out.Tracks.size(), Out.Duration));
                });
        }

        void RegisterTrackTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceTrackTypesParams, SSequenceTrackTypesResult>(
                Owner, "sequence.track_types",
                "List the kinds of track a sequence can hold.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
                [](const SSequenceTrackTypesParams& In, SSequenceTrackTypesResult& Out)
                {
                    for (const FTrackType& Type : TrackTypes)
                    {
                        SSequenceTrackTypeInfo Info;
                        Info.Name = Type.Name;
                        Info.Description = Type.Description;
                        Info.bNeedsBinding = Type.bNeedsBinding;
                        if (In.bIncludeSchema)
                        {
                            const Agent::FSchemaResult Schema = Agent::GenerateSchema(Type.Class());
                            if (Schema.IsValid())
                            {
                                Info.Schema = FString(Schema.Schema.dump().c_str());
                            }
                        }
                        Out.Types.push_back(Move(Info));
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} track type(s).", Out.Types.size()));
                });
        }

        void RegisterAddBinding(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceBindingParams, SSequenceIndexResult>(
                Owner, "sequence.add_binding",
                "Add something for the sequence to drive, an existing entity by name, a prefab spawned for its length, or a cinematic camera.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceBindingParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("A binding needs a name.");
                    }
                    if (FindBinding(Sequence, FStringView(In.Name)) != INDEX_NONE)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("The sequence already has a binding named '{}'.", In.Name));
                    }

                    SSequenceBinding Binding;
                    Binding.Name = FName(In.Name.c_str());
                    if (In.Kind == "Spawn")
                    {
                        Binding.Kind = ESequenceBindingKind::Spawn;
                        CObject* Asset = nullptr;
                        if (!Agent::ResolveAssetObject(FStringView(In.Prefab), Asset, Error) || Cast<CPrefab>(Asset) == nullptr)
                        {
                            return Agent::FToolResult::Error(Error.empty() ? FString("Prefab has to name a prefab asset.") : Error);
                        }
                        Binding.SpawnPrefab = Cast<CPrefab>(Asset);
                    }
                    else if (In.Kind == "Camera")
                    {
                        Binding.Kind = ESequenceBindingKind::Camera;
                    }
                    else if (!In.Kind.empty() && In.Kind != "Possess")
                    {
                        return Agent::FToolResult::Error("Kind has to be Possess, Spawn or Camera.");
                    }

                    Out.Index = (int32)Sequence->Bindings.size();
                    Sequence->Bindings.push_back(Binding);
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("Binding {} is {}.", Out.Index, In.Name));
                });
        }

        void RegisterRemoveBinding(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveSequenceBindingParams, SSequenceIndexResult>(
                Owner, "sequence.remove_binding",
                "Remove a binding along with its tracks and camera cuts, renumbering the ones after it.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveSequenceBindingParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Removed = INDEX_NONE;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !ParseBinding(Sequence, In.Binding, Removed, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (Removed == INDEX_NONE)
                    {
                        return Agent::FToolResult::Error("Name the binding to remove.");
                    }

                    const auto Renumber = [Removed](int32& Index)
                    {
                        if (Index > Removed)
                        {
                            --Index;
                        }
                    };

                    for (int32 Index = (int32)Sequence->Tracks.size() - 1; Index >= 0; --Index)
                    {
                        CSequenceTrack* Track = Sequence->Tracks[Index].Get();
                        if (Track == nullptr || Track->BindingIndex == Removed)
                        {
                            Sequence->Tracks.erase(Sequence->Tracks.begin() + Index);
                            continue;
                        }
                        Renumber(Track->BindingIndex);

                        if (CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track))
                        {
                            for (int32 Cut = (int32)Cuts->Cuts.size() - 1; Cut >= 0; --Cut)
                            {
                                if (Cuts->Cuts[Cut].BindingIndex == Removed)
                                {
                                    Cuts->Cuts.erase(Cuts->Cuts.begin() + Cut);
                                    continue;
                                }
                                Renumber(Cuts->Cuts[Cut].BindingIndex);
                            }
                        }
                        else if (CSequenceTrack_LookAt* LookAt = Cast<CSequenceTrack_LookAt>(Track))
                        {
                            if (LookAt->TargetBindingIndex == Removed)
                            {
                                LookAt->TargetBindingIndex = INDEX_NONE;
                            }
                            Renumber(LookAt->TargetBindingIndex);
                        }
                    }

                    Sequence->Bindings.erase(Sequence->Bindings.begin() + Removed);
                    MarkDirty(Sequence);
                    Out.Index = Removed;
                    return Agent::FToolResult::Ok(Lumina::Format("Removed binding {}.", Removed));
                });
        }

        void RegisterAddTrack(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceTrackParams, SSequenceIndexResult>(
                Owner, "sequence.add_track",
                "Add a track of any type from sequence.track_types. Property tracks take Settings such as {\"ComponentType\":\"SCameraComponent\",\"PropertyPath\":\"FOV\"}.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceTrackParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Binding = INDEX_NONE;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !ParseBinding(Sequence, In.Binding, Binding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FTrackType* Type = FindTrackType(FStringView(In.Type));
                    if (Type == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a track type. sequence.track_types lists them.", In.Type));
                    }
                    if (Type->bNeedsBinding && Binding == INDEX_NONE)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("A {} track drives a binding, so name one.", Type->Name));
                    }

                    CSequenceTrack* Track = AddTrack(Sequence, Type->Class(), Binding);
                    Out.Index = (int32)Sequence->Tracks.size() - 1;

                    if (!In.Settings.empty())
                    {
                        nlohmann::json Settings;
                        if (!ParseJson(In.Settings, Settings, Error))
                        {
                            Sequence->Tracks.pop_back();
                            return Agent::FToolResult::Error(Error);
                        }
                        for (auto& Entry : Settings.items())
                        {
                            Agent::FResolvedProperty Resolved;
                            if (!Agent::ResolvePropertyPath(Track->GetClass(), Track, FStringView(Entry.key().c_str()), Resolved, Error))
                            {
                                Sequence->Tracks.pop_back();
                                return Agent::FToolResult::Error(Error);
                            }
                            const Agent::FMarshalResult Read = Agent::ReadProperty(Entry.value(), Resolved.Property, Resolved.ValuePtr, FStringView(Entry.key().c_str()));
                            if (!Read.IsValid())
                            {
                                Sequence->Tracks.pop_back();
                                return Agent::FToolResult::Error(Read.Error);
                            }
                        }
                    }

                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("Track {} is a {} track.", Out.Index, Type->Name));
                });
        }

        void RegisterRemoveTrack(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveSequenceTrackParams, SSequenceIndexResult>(
                Owner, "sequence.remove_track",
                "Remove a track by index, or every track with a negative index.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveSequenceTrackParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Track < 0)
                    {
                        Out.Index = (int32)Sequence->Tracks.size();
                        Sequence->Tracks.clear();
                        MarkDirty(Sequence);
                        return Agent::FToolResult::Ok(Lumina::Format("Removed {} track(s).", Out.Index));
                    }

                    if (!CheckTrack(Sequence, In.Track, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    Sequence->Tracks.erase(Sequence->Tracks.begin() + In.Track);
                    MarkDirty(Sequence);
                    Out.Index = In.Track;
                    return Agent::FToolResult::Ok(Lumina::Format("Removed track {}. Later tracks moved down one.", In.Track));
                });
        }

        void RegisterSetTrackProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetSequenceTrackPropertyParams, SSequencePropertyResult>(
                Owner, "sequence.set_track_property",
                "Set any field on a track by path, such as bEnabled, TargetOffset, bAutoFocus, Color, LocationAmplitude or Clips[0].Volume.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetSequenceTrackPropertyParams& In, SSequencePropertyResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !CheckTrack(Sequence, In.Track, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CSequenceTrack* Track = Sequence->Tracks[In.Track].Get();
                    Agent::FResolvedProperty Resolved;
                    if (!Agent::ResolvePropertyPath(Track->GetClass(), Track, FStringView(In.Path), Resolved, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Value;
                    if (!ParseJson(In.Value, Value, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Previous;
                    Agent::WriteProperty(Resolved.Property, Resolved.ValuePtr, Previous);
                    const Agent::FMarshalResult Read = Agent::ReadProperty(Value, Resolved.Property, Resolved.ValuePtr, FStringView(In.Path));
                    if (!Read.IsValid())
                    {
                        return Agent::FToolResult::Error(Read.Error);
                    }

                    nlohmann::json Current;
                    Agent::WriteProperty(Resolved.Property, Resolved.ValuePtr, Current);
                    Out.Previous = FString(Previous.dump().c_str());
                    Out.Current = FString(Current.dump().c_str());
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} is now {}.", In.Path, Out.Current));
                });
        }

        void RegisterSetKeys(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetSequenceKeysParams, SSetSequenceKeysResult>(
                Owner, "sequence.set_keys",
                "Key one curve on a track. Keys is an array such as [{\"Time\":0,\"Value\":[0,5,0]},{\"Time\":2,\"Value\":[10,5,0]}] for a vector channel or [{\"Time\":0,\"Value\":1}] for a single curve. Rotation keys are unwound so they turn the short way.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetSequenceKeysParams& In, SSetSequenceKeysResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !CheckTrack(Sequence, In.Track, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ECurveInterpMode DefaultMode;
                    if (!ParseInterp(FStringView(In.Interp), DefaultMode))
                    {
                        return Agent::FToolResult::Error("Interp has to be Cubic, Linear or Constant.");
                    }

                    CSequenceTrack* Track = Sequence->Tracks[In.Track].Get();
                    Agent::FResolvedProperty Resolved;
                    if (!Agent::ResolvePropertyPath(Track->GetClass(), Track, FStringView(In.Channel), Resolved, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CStruct* ChannelType = Resolved.Property->IsA(EPropertyTypeFlags::Struct)
                        ? static_cast<FStructProperty*>(Resolved.Property)->GetStruct() : nullptr;
                    const bool bVector = ChannelType == SSequenceVectorCurve::StaticStruct();
                    if (!bVector && ChannelType != SCurve::StaticStruct())
                    {
                        return Agent::FToolResult::Error(Lumina::Format("{} is not a curve. Key a channel such as Location, Rotation, Curve, Amount, Weight or Intensity.", In.Channel));
                    }

                    nlohmann::json Keys;
                    if (!ParseJson(In.Keys, Keys, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (!Keys.is_array())
                    {
                        return Agent::FToolResult::Error("Keys has to be an array.");
                    }

                    SSequenceVectorCurve* Vector = bVector ? static_cast<SSequenceVectorCurve*>(Resolved.ValuePtr) : nullptr;
                    SCurve* Scalar = bVector ? nullptr : static_cast<SCurve*>(Resolved.ValuePtr);

                    if (In.bReplace)
                    {
                        if (Vector != nullptr)
                        {
                            Vector->X.Curve.Keys.clear();
                            Vector->Y.Curve.Keys.clear();
                            Vector->Z.Curve.Keys.clear();
                        }
                        else
                        {
                            Scalar->Curve.Keys.clear();
                        }
                    }

                    for (const nlohmann::json& Entry : Keys)
                    {
                        if (!Entry.is_object() || !Entry.contains("Time") || !Entry["Time"].is_number() || !Entry.contains("Value"))
                        {
                            return Agent::FToolResult::Error("Every key needs a numeric Time and a Value.");
                        }

                        ECurveInterpMode Mode = DefaultMode;
                        if (Entry.contains("Interp") && Entry["Interp"].is_string()
                            && !ParseInterp(FStringView(Entry["Interp"].get<std::string>().c_str()), Mode))
                        {
                            return Agent::FToolResult::Error("A key's Interp has to be Cubic, Linear or Constant.");
                        }

                        const float Time = Entry["Time"].get<float>();
                        if (Vector != nullptr)
                        {
                            FVector3 Value;
                            if (!ReadVector(Entry["Value"], Value))
                            {
                                return Agent::FToolResult::Error("A vector channel's Value is [x,y,z].");
                            }
                            KeyVector(*Vector, Time, Value, Mode);
                        }
                        else
                        {
                            if (!Entry["Value"].is_number())
                            {
                                return Agent::FToolResult::Error("A single curve's Value is a number.");
                            }
                            Key(Scalar->Curve, Time, Entry["Value"].get<float>(), Mode);
                        }
                    }

                    if (Vector != nullptr)
                    {
                        const bool bAngles = In.Channel.find("Rotation") != FString::npos;
                        FinishVector(*Vector, bAngles);
                        Out.KeyCount = Vector->X.Curve.NumKeys();
                    }
                    else
                    {
                        Scalar->bUseAsset = false;
                        Scalar->Curve.ComputeAutoTangents();
                        Out.KeyCount = Scalar->Curve.NumKeys();
                    }

                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} now has {} key(s).", In.Channel, Out.KeyCount));
                });
        }

        int32 AddCut(CSequence* Sequence, int32 Camera, float Start, float End, float BlendTime)
        {
            int32 TrackIndex = INDEX_NONE;
            CSequenceTrack_CameraCut* Track = FindOrAddTrack<CSequenceTrack_CameraCut>(Sequence, INDEX_NONE, TrackIndex);

            // A new shot takes over its range, trimming or dropping the cuts it lands on.
            TVector<SSequenceCameraCut> Kept;
            for (const SSequenceCameraCut& Cut : Track->Cuts)
            {
                if (Cut.EndTime <= Start || Cut.StartTime >= End)
                {
                    Kept.push_back(Cut);
                    continue;
                }
                if (Cut.StartTime < Start)
                {
                    SSequenceCameraCut Before = Cut;
                    Before.EndTime = Start;
                    Kept.push_back(Before);
                }
                if (Cut.EndTime > End)
                {
                    SSequenceCameraCut After = Cut;
                    After.StartTime = End;
                    After.BlendTime = 0.0f;
                    Kept.push_back(After);
                }
            }

            SSequenceCameraCut Cut;
            Cut.BindingIndex = Camera;
            Cut.StartTime = Start;
            Cut.EndTime = End;
            Cut.BlendTime = Math::Max(BlendTime, 0.0f);
            Kept.push_back(Cut);

            std::sort(Kept.begin(), Kept.end(), [](const SSequenceCameraCut& A, const SSequenceCameraCut& B) { return A.StartTime < B.StartTime; });
            Track->Cuts = Move(Kept);

            Sequence->Duration = Math::Max(Sequence->Duration, End);
            return TrackIndex;
        }

        void RegisterAddCameraCut(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceCameraCutParams, SSequenceIndexResult>(
                Owner, "sequence.add_camera_cut",
                "Make a camera binding the live view from Start to End, trimming any cut it overlaps.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceCameraCutParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Camera = INDEX_NONE;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !ParseBinding(Sequence, In.Camera, Camera, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (Camera == INDEX_NONE || In.End <= In.Start)
                    {
                        return Agent::FToolResult::Error("A cut needs a camera binding and an End after its Start.");
                    }

                    Out.Index = AddCut(Sequence, Camera, In.Start, In.End, In.BlendTime);
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} is live from {:.2f}s to {:.2f}s.", BindingName(Sequence, Camera), In.Start, In.End));
                });
        }

        void RegisterAddEvent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceEventParams, SSequenceIndexResult>(
                Owner, "sequence.add_event",
                "Fire a named event at a time, for gameplay to react to through CSequenceLibrary.ConsumeEvents, such as an explosion on a beat.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceEventParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Binding = INDEX_NONE;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !ParseBinding(Sequence, In.Binding, Binding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (In.Name.empty())
                    {
                        return Agent::FToolResult::Error("An event needs a name.");
                    }

                    int32 TrackIndex = INDEX_NONE;
                    CSequenceTrack_Event* Track = FindOrAddTrack<CSequenceTrack_Event>(Sequence, Binding, TrackIndex);

                    SSequenceEventKey Key;
                    Key.Time = Math::Max(In.Time, 0.0f);
                    Key.Name = FName(In.Name.c_str());
                    Key.Payload = In.Payload;
                    auto Where = std::upper_bound(Track->Keys.begin(), Track->Keys.end(), Key.Time,
                        [](float Time, const SSequenceEventKey& Other) { return Time < Other.Time; });
                    Track->Keys.insert(Where, Key);

                    Out.Index = TrackIndex;
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} fires at {:.2f}s.", In.Name, Key.Time));
                });
        }

        void RegisterAddAudio(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceAudioParams, SSequenceIndexResult>(
                Owner, "sequence.add_audio",
                "Start a sound at a time, as music in stereo or at a binding in the world.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceAudioParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Binding = INDEX_NONE;
                    if (!ResolveSequence(In.Sequence, Sequence, Error) || !ParseBinding(Sequence, In.Binding, Binding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CObject* Asset = nullptr;
                    if (!Agent::ResolveAssetObject(FStringView(In.Sound), Asset, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    CSoundBase* Sound = Cast<CSoundBase>(Asset);
                    if (Sound == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("'{}' is not a sound.", In.Sound));
                    }

                    int32 TrackIndex = INDEX_NONE;
                    CSequenceTrack_Audio* Track = FindOrAddTrack<CSequenceTrack_Audio>(Sequence, Binding, TrackIndex);
                    SSequenceAudioClip Clip;
                    Clip.StartTime = Math::Max(In.Start, 0.0f);
                    Clip.Sound = Sound;
                    Clip.Volume = Math::Max(In.Volume, 0.0f);
                    Clip.Pitch = Math::Max(In.Pitch, 0.01f);
                    // A clip bound to nothing is score, so it follows the music volume rather than the effects.
                    Clip.Bus = Binding == INDEX_NONE ? EAudioBus::Music : EAudioBus::SFX;
                    Track->Clips.push_back(Clip);

                    Out.Index = TrackIndex;
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} starts at {:.2f}s.", Sound->GetName(), Clip.StartTime));
                });
        }

        void KeyProperty(CSequence* Sequence, int32 Camera, const char* Path, float Time, float Value, ECurveInterpMode Mode, TVector<int32>& OutTracks)
        {
            CSequenceTrack_Property* Found = nullptr;
            int32 FoundIndex = INDEX_NONE;
            for (int32 Index = 0; Index < (int32)Sequence->Tracks.size(); ++Index)
            {
                CSequenceTrack_Property* Track = Cast<CSequenceTrack_Property>(Sequence->Tracks[Index].Get());
                if (Track != nullptr && Track->BindingIndex == Camera && Track->ComponentType == FName("SCameraComponent") && Track->PropertyPath == Path)
                {
                    Found = Track;
                    FoundIndex = Index;
                    break;
                }
            }

            if (Found == nullptr)
            {
                FoundIndex = (int32)Sequence->Tracks.size();
                Found = static_cast<CSequenceTrack_Property*>(AddTrack(Sequence, CSequenceTrack_Property::StaticClass(), Camera));
                Found->ComponentType = FName("SCameraComponent");
                Found->PropertyPath = Path;
            }

            Key(Found->Curve.Curve, Time, Value, Mode);
            Found->Curve.Curve.ComputeAutoTangents();
            if (std::find(OutTracks.begin(), OutTracks.end(), FoundIndex) == OutTracks.end())
            {
                OutTracks.push_back(FoundIndex);
            }
        }

        void RegisterShot(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceShotParams, SSequenceShotResult>(
                Owner, "sequence.shot",
                "Author a whole camera shot in one call, a camera binding with its move, aim, lens and cut. Keys are relative to Start, such as "
                "[{\"Time\":0,\"Location\":[0,3,-10],\"Target\":[0,1,0]},{\"Time\":4,\"Location\":[6,4,-8],\"FOV\":40}]. A key aims with Rotation [pitch,yaw,roll] or a Target point; "
                "LookAt or LookAtPoint aims every frame instead. Use one camera per shot so moves on either side of a cut cannot bend each other.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceShotParams& In, SSequenceShotResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (In.End <= In.Start)
                    {
                        return Agent::FToolResult::Error("A shot needs an End after its Start.");
                    }

                    ECurveInterpMode Mode;
                    if (!ParseInterp(FStringView(In.Interp), Mode))
                    {
                        return Agent::FToolResult::Error("Interp has to be Cubic, Linear or Constant.");
                    }

                    nlohmann::json Keys;
                    if (!ParseJson(In.Keys.empty() ? FString("[]") : In.Keys, Keys, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (!Keys.is_array() || Keys.empty())
                    {
                        return Agent::FToolResult::Error("A shot needs at least one key with a Location.");
                    }

                    int32 LookAtBinding = INDEX_NONE;
                    if (!ParseBinding(Sequence, In.LookAt, LookAtBinding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FVector3 LookAtPoint(0.0f);
                    bool bHasLookAtPoint = false;
                    if (!In.LookAtPoint.empty())
                    {
                        nlohmann::json Point;
                        if (!ParseJson(In.LookAtPoint, Point, Error) || !ReadVector(Point, LookAtPoint))
                        {
                            return Agent::FToolResult::Error("LookAtPoint is [x,y,z].");
                        }
                        Ground(LookAtPoint, In.bGround);
                        bHasLookAtPoint = true;
                    }

                    FVector3 LookAtOffset(0.0f);
                    if (!In.LookAtOffset.empty())
                    {
                        nlohmann::json Offset;
                        if (!ParseJson(In.LookAtOffset, Offset, Error) || !ReadVector(Offset, LookAtOffset))
                        {
                            return Agent::FToolResult::Error("LookAtOffset is [x,y,z].");
                        }
                    }

                    FString CameraName = In.Camera;
                    if (CameraName.empty())
                    {
                        int32 Cameras = 0;
                        for (const SSequenceBinding& Binding : Sequence->Bindings)
                        {
                            Cameras += Binding.Kind == ESequenceBindingKind::Camera ? 1 : 0;
                        }
                        do
                        {
                            CameraName = Lumina::Format("ShotCam{}", ++Cameras);
                        }
                        while (FindBinding(Sequence, FStringView(CameraName)) != INDEX_NONE);
                    }

                    int32 Camera = FindBinding(Sequence, FStringView(CameraName));
                    if (Camera == INDEX_NONE)
                    {
                        SSequenceBinding Binding;
                        Binding.Name = FName(CameraName.c_str());
                        Binding.Kind = ESequenceBindingKind::Camera;
                        Camera = (int32)Sequence->Bindings.size();
                        Sequence->Bindings.push_back(Binding);
                    }

                    int32 TransformIndex = INDEX_NONE;
                    CSequenceTrack_Transform* Transform = FindOrAddTrack<CSequenceTrack_Transform>(Sequence, Camera, TransformIndex);
                    Out.Tracks.push_back(TransformIndex);

                    const bool bFollows = LookAtBinding != INDEX_NONE || bHasLookAtPoint;
                    for (const nlohmann::json& Entry : Keys)
                    {
                        FVector3 Location;
                        if (!Entry.is_object() || !Entry.contains("Location") || !ReadVector(Entry["Location"], Location))
                        {
                            return Agent::FToolResult::Error("Every shot key needs a Location as [x,y,z].");
                        }

                        const bool bGround = Entry.value("Ground", In.bGround);
                        Ground(Location, bGround);
                        const float Time = In.Start + Math::Clamp(Entry.value("Time", 0.0f), 0.0f, In.End - In.Start);
                        KeyVector(Transform->Location, Time, Location, Mode);

                        FVector3 Rotation;
                        FVector3 Target;
                        if (Entry.contains("Rotation") && ReadVector(Entry["Rotation"], Rotation))
                        {
                            KeyVector(Transform->Rotation, Time, Rotation, Mode);
                        }
                        else if (Entry.contains("Target") && ReadVector(Entry["Target"], Target))
                        {
                            Ground(Target, bGround);
                            KeyVector(Transform->Rotation, Time, LookAtEuler(Location, Target), Mode);
                        }
                        else if (bHasLookAtPoint)
                        {
                            // Keyed too, so the rotation is right even before the look-at track takes over.
                            KeyVector(Transform->Rotation, Time, LookAtEuler(Location, LookAtPoint), Mode);
                        }

                        if (Entry.contains("FOV") && Entry["FOV"].is_number())
                        {
                            KeyProperty(Sequence, Camera, "FOV", Time, Entry["FOV"].get<float>(), Mode, Out.Tracks);
                        }
                        if (Entry.contains("Focus") && Entry["Focus"].is_number())
                        {
                            KeyProperty(Sequence, Camera, "PostProcess.DepthOfFieldFocusDistance", Time, Entry["Focus"].get<float>(), Mode, Out.Tracks);
                        }
                        if (Entry.contains("FStop") && Entry["FStop"].is_number())
                        {
                            KeyProperty(Sequence, Camera, "PostProcess.DepthOfFieldFStop", Time, Entry["FStop"].get<float>(), Mode, Out.Tracks);
                        }
                    }

                    FinishVector(Transform->Location, false);
                    if (Transform->Rotation.bEnabled)
                    {
                        FinishVector(Transform->Rotation, true);
                    }

                    if (In.FOV > 0.0f)
                    {
                        KeyProperty(Sequence, Camera, "FOV", In.Start, In.FOV, ECurveInterpMode::Constant, Out.Tracks);
                    }
                    if (In.FStop > 0.0f)
                    {
                        KeyProperty(Sequence, Camera, "PostProcess.DepthOfFieldFStop", In.Start, In.FStop, ECurveInterpMode::Constant, Out.Tracks);
                    }

                    if (bFollows)
                    {
                        int32 LookAtIndex = INDEX_NONE;
                        CSequenceTrack_LookAt* LookAt = FindOrAddTrack<CSequenceTrack_LookAt>(Sequence, Camera, LookAtIndex);
                        LookAt->TargetBindingIndex = LookAtBinding;
                        LookAt->TargetOffset = LookAtBinding != INDEX_NONE ? LookAtOffset : LookAtPoint + LookAtOffset;
                        LookAt->bAutoFocus = In.bAutoFocus && In.FStop > 0.0f;
                        Out.Tracks.push_back(LookAtIndex);
                    }

                    if (In.Handheld > 0.0f)
                    {
                        // The shake runs over whatever view is live, so its strength is keyed to this shot's range alone.
                        int32 ShakeIndex = INDEX_NONE;
                        CSequenceTrack_CameraShake* Shake = FindOrAddTrack<CSequenceTrack_CameraShake>(Sequence, INDEX_NONE, ShakeIndex);
                        Key(Shake->Intensity.Curve, In.Start - 0.001f, 0.0f, ECurveInterpMode::Constant);
                        Key(Shake->Intensity.Curve, In.Start, In.Handheld, ECurveInterpMode::Constant);
                        Key(Shake->Intensity.Curve, In.End, 0.0f, ECurveInterpMode::Constant);
                        if (std::find(Out.Tracks.begin(), Out.Tracks.end(), ShakeIndex) == Out.Tracks.end())
                        {
                            Out.Tracks.push_back(ShakeIndex);
                        }
                    }

                    if (In.bCut)
                    {
                        const int32 CutIndex = AddCut(Sequence, Camera, In.Start, In.End, In.BlendTime);
                        Out.Tracks.push_back(CutIndex);
                    }
                    Sequence->Duration = Math::Max(Sequence->Duration, In.End);

                    Out.Camera = CameraName;
                    Out.CameraBinding = Camera;
                    MarkDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} covers {:.2f}s to {:.2f}s with {} key(s).", CameraName, In.Start, In.End, Keys.size()));
                });
        }

        void RegisterPlay(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequencePlayParams, SSequencePlayerInfo>(
                Owner, "sequence.play",
                "Play a sequence in the running game, replacing any player of the same sequence. Paused shows StartTime for framing.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequencePlayParams& In, SSequencePlayerInfo& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    CWorld* World = RequireGameWorld(Error);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ForEachPlayer(World, Sequence, [World](ECS::FEntity Player) { CSequenceLibrary::StopSequence(World, Player); });

                    const ECS::FEntity Player = CSequenceLibrary::PlaySequence(World, Sequence, In.bLoop, In.PlayRate);
                    SSequencePlayerComponent& Component = World->GetComponent<SSequencePlayerComponent>(Player);
                    if (In.StartTime > 0.0f)
                    {
                        Component.Seek(In.StartTime);
                    }
                    if (In.bPaused)
                    {
                        Component.Pause();
                    }

                    Out.Player = Agent::FEntityTokens::Mint(ECS::GetWorldRegistry(*World), Player);
                    Out.Sequence = FString(Sequence->GetName().ToString().c_str());
                    Out.Time = Component.Time;
                    Out.Duration = Sequence->Duration;
                    Out.bPlaying = true;
                    Out.bPaused = In.bPaused;
                    return Agent::FToolResult::Ok(Lumina::Format("Playing {} from {:.2f}s{}.", Out.Sequence, Out.Time, In.bPaused ? ", paused" : ""));
                });
        }

        void CollectState(CWorld* World, SSequenceStateResult& Out)
        {
            ForEachPlayer(World, nullptr, [World, &Out](ECS::FEntity Entity)
            {
                const SSequencePlayerComponent& Player = World->GetComponent<SSequencePlayerComponent>(Entity);
                SSequencePlayerInfo Info;
                Info.Player = Agent::FEntityTokens::Mint(ECS::GetWorldRegistry(*World), Entity);
                Info.Time = Player.Time;
                Info.bPlaying = Player.bPlaying;
                Info.bPaused = Player.bPaused;
                if (const CSequence* Sequence = Player.Sequence.Get())
                {
                    Info.Sequence = FString(Sequence->GetName().ToString().c_str());
                    Info.Duration = Sequence->Duration;
                    for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
                    {
                        const CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track.Get());
                        const int32 Cut = Cuts != nullptr ? Cuts->FindCutAt(Player.Time) : INDEX_NONE;
                        if (Cut != INDEX_NONE)
                        {
                            Info.LiveCamera = BindingName(Sequence, Cuts->Cuts[Cut].BindingIndex);
                            break;
                        }
                    }
                }
                Out.Players.push_back(Move(Info));
            });
        }

        void RegisterControl(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceControlParams, SSequenceStateResult>(
                Owner, "sequence.control",
                "Pause, Resume, Stop, Seek or Restart the sequences playing in the game.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceControlParams& In, SSequenceStateResult& Out)
                {
                    FString Error;
                    CWorld* World = RequireGameWorld(Error);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CSequence* Only = nullptr;
                    if (!In.Sequence.empty() && !ResolveSequence(In.Sequence, Only, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FString& Action = In.Action;
                    if (Action != "Pause" && Action != "Resume" && Action != "Stop" && Action != "Seek" && Action != "Restart")
                    {
                        return Agent::FToolResult::Error("Action has to be Pause, Resume, Stop, Seek or Restart.");
                    }

                    int32 Touched = 0;
                    ForEachPlayer(World, Only, [&](ECS::FEntity Entity)
                    {
                        ++Touched;
                        if (Action == "Stop")
                        {
                            CSequenceLibrary::StopSequence(World, Entity);
                            return;
                        }

                        SSequencePlayerComponent& Player = World->GetComponent<SSequencePlayerComponent>(Entity);
                        if (Action == "Pause")        { Player.Pause(); }
                        else if (Action == "Resume")  { Player.Resume(); }
                        else if (Action == "Seek")    { Player.Seek(In.Time); }
                        else
                        {
                            Player.Seek(0.0f);
                            Player.Play(true);
                        }
                    });

                    CollectState(World, Out);
                    return Touched > 0
                        ? Agent::FToolResult::Ok(Lumina::Format("{} applied to {} player(s).", Action, Touched))
                        : Agent::FToolResult::Error("No sequence is playing. Start one with sequence.play.");
                });
        }

        void RegisterState(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceRefParams, SSequenceStateResult>(
                Owner, "sequence.state",
                "Report every sequence player in the game with its playhead and live camera.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SSequenceRefParams&, SSequenceStateResult& Out)
                {
                    FString Error;
                    CWorld* World = RequireGameWorld(Error);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    CollectState(World, Out);
                    return Agent::FToolResult::Ok(Lumina::Format("{} player(s).", Out.Players.size()));
                });
        }

        void RegisterMovieRender(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMovieRenderParams, SMovieStatusResult>(
                Owner, "movie.render",
                "Render the running game to an H.264 MP4 at a locked frame rate, however slowly each frame draws. With a Sequence it plays that sequence from the start "
                "and stops at its end; without one it records Seconds of gameplay. Returns at once, so poll movie.status until bFinished.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMovieRenderParams& In, SMovieStatusResult& Out)
                {
                    FString Error;
                    CWorld* World = RequireGameWorld(Error);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CSequence* Sequence = nullptr;
                    if (!In.Sequence.empty() && !ResolveSequence(In.Sequence, Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MovieCapture::FSettings Settings;
                    Settings.OutputPath = In.OutputPath;
                    Settings.PngDirectory = In.PngDirectory;
                    Settings.PngEvery = (uint32)Math::Max(In.PngEvery, 1);
                    if (Settings.OutputPath.empty() && Settings.PngDirectory.empty())
                    {
                        const FString Stem = Sequence != nullptr ? FString(Sequence->GetName().ToString().c_str()) : FString("Gameplay");
                        Settings.OutputPath = Lumina::Format("{}/Saved/Movies/{}.mp4", Paths::GetEngineDirectory(), Stem);
                    }
                    Settings.Width = (uint32)Math::Max(In.Width, 16);
                    Settings.Height = (uint32)Math::Max(In.Height, 16);
                    Settings.FrameRate = (uint32)Math::Clamp(In.FrameRate, 1, 240);
                    Settings.BitRate = (uint32)Math::Max(In.BitRate, 1000000);
                    Settings.WarmupFrames = (uint32)Math::Max(In.WarmupFrames, 1);
                    Settings.bRecordAudio = In.bAudio;
                    Settings.WavPath = In.WavPath;

                    const float Seconds = Sequence != nullptr ? Sequence->Duration : In.Seconds;
                    Settings.FrameCount = Seconds > 0.0f ? (uint32)std::ceil(Seconds * (float)Settings.FrameRate) : 0u;

                    if (Sequence != nullptr)
                    {
                        // Held on its first frame through the warmup, so streaming and exposure settle on the opening shot.
                        ForEachPlayer(World, nullptr, [World](ECS::FEntity Player) { CSequenceLibrary::StopSequence(World, Player); });
                        const ECS::FEntity Player = CSequenceLibrary::PlaySequence(World, Sequence, false, 1.0f);
                        World->GetComponent<SSequencePlayerComponent>(Player).Pause();

                        Settings.OnRecordingStarted = [Sequence]()
                        {
                            if (CWorld* Game = FindWorld(true))
                            {
                                ForEachPlayer(Game, Sequence, [Game](ECS::FEntity Entity) { Game->GetComponent<SSequencePlayerComponent>(Entity).Resume(); });
                            }
                        };
                    }

                    if (!MovieCapture::Start(Move(Settings), Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FillStatus(MovieCapture::GetStatus(), Out);
                    return Agent::FToolResult::Ok(Out.FrameCount > 0
                        ? Lumina::Format("Recording {} frame(s) to {}.", Out.FrameCount, Out.OutputPath.empty() ? Out.PngDirectory : Out.OutputPath)
                        : Lumina::Format("Recording to {} until movie.stop.", Out.OutputPath.empty() ? Out.PngDirectory : Out.OutputPath));
                });
        }

        void RegisterMovieStatus(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceRefParams, SMovieStatusResult>(
                Owner, "movie.status",
                "Report how far a movie render has got, and where it wrote once bFinished.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SSequenceRefParams&, SMovieStatusResult& Out)
                {
                    FillStatus(MovieCapture::GetStatus(), Out);
                    if (!Out.Error.empty())
                    {
                        return Agent::FToolResult::Error(Out.Error);
                    }
                    return Agent::FToolResult::Ok(Out.bActive
                        ? Lumina::Format("{} of {} frame(s).", Out.FramesWritten, Out.FrameCount)
                        : Lumina::Format("Finished with {} frame(s).", Out.FramesWritten));
                });
        }

        void RegisterMovieStop(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceRefParams, SMovieStatusResult>(
                Owner, "movie.stop",
                "Finish a movie render early, keeping what has been recorded.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceRefParams&, SMovieStatusResult& Out)
                {
                    MovieCapture::Stop();
                    FillStatus(MovieCapture::GetStatus(), Out);
                    return Agent::FToolResult::Ok(Lumina::Format("Stopped with {} frame(s).", Out.FramesWritten));
                });
        }

        void RegisterCameraView(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCameraViewParams, SCameraViewResult>(
                Owner, "camera.get_view",
                "Report where the live camera is and what it faces, with a key ready for sequence.shot. Fly the editor camera to a good angle and read it here.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SCameraViewParams& In, SCameraViewResult& Out)
                {
                    FString Error;
                    CWorld* World = In.bEditorWorld ? nullptr : FindWorld(true);
                    Out.World = World != nullptr ? "Game" : "Editor";
                    if (World == nullptr)
                    {
                        World = SessionOps::GetSceneWorld(Error);
                    }
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error.empty() ? FString("No world is open.") : Error);
                    }

                    const ECS::FEntity Camera = World->GetActiveCameraEntity();
                    const STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
                        ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
                    if (Transform == nullptr)
                    {
                        return Agent::FToolResult::Error("The world has no live camera.");
                    }

                    const FVector3 Location = Transform->GetWorldLocation();
                    const FVector3 Rotation = Math::Degrees(Math::YawFirstEulerAngles(Transform->GetWorldRotation()));
                    FVector3 Forward = Transform->GetWorldRotation() * FVector3(0.0f, 0.0f, -1.0f);
                    Out.FOV = 90.0f;
                    if (const SCameraComponent* Component = World->TryGetComponent<SCameraComponent>(Camera))
                    {
                        Forward = Component->GetForwardVector();
                        Out.FOV = Component->FOV;
                    }

                    Out.Location = { Location.x, Location.y, Location.z };
                    Out.Rotation = { Rotation.x, Rotation.y, Rotation.z };
                    Out.Forward = { Forward.x, Forward.y, Forward.z };
                    Out.ShotKey = Lumina::Format("{{\"Time\":0,\"Location\":[{:.2f},{:.2f},{:.2f}],\"Rotation\":[{:.2f},{:.2f},{:.2f}],\"FOV\":{:.1f}}}",
                        Location.x, Location.y, Location.z, Rotation.x, Rotation.y, Rotation.z, Out.FOV);
                    return Agent::FToolResult::Ok(Out.ShotKey);
                });
        }
    }

    namespace SequenceTools
    {
        void RegisterSetCameraView(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetCameraViewParams, SCameraViewResult>(
                Owner, "camera.set_view",
                "Move the editor viewport camera to a location, facing a Target point or a Rotation, to frame a screenshot.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetCameraViewParams& In, SCameraViewResult& Out)
                {
                    if (In.Location.size() != 3 || (In.Target.size() != 3 && In.Rotation.size() != 3))
                    {
                        return Agent::FToolResult::Error("Location is [x,y,z], plus a Target [x,y,z] or a Rotation [pitch,yaw,roll].");
                    }

                    FString Error;
                    CWorld* World = SessionOps::GetSceneWorld(Error);
                    if (World == nullptr)
                    {
                        return Agent::FToolResult::Error(Error.empty() ? FString("No world is open.") : Error);
                    }

                    const ECS::FEntity Camera = World->GetActiveCameraEntity();
                    STransformComponent* Transform = Camera != ECS::NullEntity && World->IsValidEntity(Camera)
                        ? World->TryGetComponent<STransformComponent>(Camera) : nullptr;
                    if (Transform == nullptr)
                    {
                        return Agent::FToolResult::Error("The editor world has no viewport camera.");
                    }

                    const FVector3 Location(In.Location[0], In.Location[1], In.Location[2]);
                    const FQuat Rotation = In.Target.size() == 3
                        ? Math::FindLookAtRotation(FVector3(In.Target[0], In.Target[1], In.Target[2]), Location)
                        : FQuat(Math::Radians(FVector3(In.Rotation[0], In.Rotation[1], In.Rotation[2])));
                    Transform->SetLocation(Location);
                    Transform->SetRotation(Rotation);

                    const FVector3 Euler = Math::Degrees(Math::YawFirstEulerAngles(Rotation));
                    Out.World = "Editor";
                    Out.Location = { Location.x, Location.y, Location.z };
                    Out.Rotation = { Euler.x, Euler.y, Euler.z };
                    return Agent::FToolResult::Ok(Lumina::Format("Camera at [{:.2f},{:.2f},{:.2f}].", Location.x, Location.y, Location.z));
                });
        }

        void RegisterGroundHeight(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SGroundHeightParams, SGroundHeightResult>(
                Owner, "world.ground_height",
                "Report the height of the first surface under each [x,z] point, for placing cameras and events on uneven terrain.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SGroundHeightParams& In, SGroundHeightResult& Out)
                {
                    nlohmann::json Points;
                    FString Error;
                    if (!ParseJson(In.Points, Points, Error) || !Points.is_array())
                    {
                        return Agent::FToolResult::Error("Points is an array of [x,z] pairs.");
                    }
                    for (const nlohmann::json& Point : Points)
                    {
                        if (!Point.is_array() || Point.size() < 2 || !Point[0].is_number() || !Point[1].is_number())
                        {
                            return Agent::FToolResult::Error("Each point is [x,z].");
                        }
                        Out.Heights.push_back(GroundHeight(Point[0].get<float>(), Point[Point.size() - 1].get<float>()));
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("Probed {} point(s).", Out.Heights.size()));
                });
        }
    }

    void RegisterSequenceTools(FStringView Owner)
    {
        SequenceTools::RegisterGroundHeight(Owner);
        SequenceTools::RegisterCreate(Owner);
        SequenceTools::RegisterSave(Owner);
        SequenceTools::RegisterSettings(Owner);
        SequenceTools::RegisterDescribe(Owner);
        SequenceTools::RegisterTrackTypes(Owner);
        SequenceTools::RegisterAddBinding(Owner);
        SequenceTools::RegisterRemoveBinding(Owner);
        SequenceTools::RegisterAddTrack(Owner);
        SequenceTools::RegisterRemoveTrack(Owner);
        SequenceTools::RegisterSetTrackProperty(Owner);
        SequenceTools::RegisterSetKeys(Owner);
        SequenceTools::RegisterAddCameraCut(Owner);
        SequenceTools::RegisterAddEvent(Owner);
        SequenceTools::RegisterAddAudio(Owner);
        SequenceTools::RegisterShot(Owner);
        SequenceTools::RegisterPlay(Owner);
        SequenceTools::RegisterControl(Owner);
        SequenceTools::RegisterState(Owner);
        SequenceTools::RegisterMovieRender(Owner);
        SequenceTools::RegisterMovieStatus(Owner);
        SequenceTools::RegisterMovieStop(Owner);
        SequenceTools::RegisterCameraView(Owner);
        SequenceTools::RegisterSetCameraView(Owner);
    }
}
