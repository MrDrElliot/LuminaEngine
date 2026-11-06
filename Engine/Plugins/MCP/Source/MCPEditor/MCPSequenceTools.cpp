#include "MCPSequenceTools.h"
#include "MCPSceneTools.h"
#include "MCPServer.h"
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
#include "Containers/Algorithm.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Session/SessionOps.h"
#include "Tools/Screenshot/MovieCapture.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "World/WorldManager.h"

#include <functional>

namespace Lumina::MCP
{
    namespace MCPSequenceToolsInternal
    {
        struct FSequenceTrackType
        {
            const char* Name;
            CClass*     (*Class)();
            bool        bNeedsBinding;
            const char* Description;
        };

        const FSequenceTrackType GSequenceTrackTypes[] =
        {
            { "Transform",    &CSequenceTrack_Transform::StaticClass,    true,  "Keys the binding's Location, Rotation (pitch, yaw, roll degrees) and Scale." },
            { "CameraCut",    &CSequenceTrack_CameraCut::StaticClass,    false, "Which camera binding is live over each time range, with optional blends." },
            { "Event",        &CSequenceTrack_Event::StaticClass,        false, "Named moments gameplay reacts to through World.GetSequences().ConsumeEvents." },
            { "Property",     &CSequenceTrack_Property::StaticClass,     true,  "A curve driving one reflected number on one component, such as SCameraComponent FOV." },
            { "Audio",        &CSequenceTrack_Audio::StaticClass,        false, "Sounds started as the playhead passes them, at the binding when there is one." },
            { "Fade",         &CSequenceTrack_Fade::StaticClass,         false, "Fades the whole picture toward Color, holding across cuts." },
            { "TimeDilation", &CSequenceTrack_TimeDilation::StaticClass, false, "World time scale for slow motion while the edit keeps real time." },
            { "LookAt",       &CSequenceTrack_LookAt::StaticClass,       true,  "Aims the binding at another binding or a fixed point, optionally pulling focus." },
            { "CameraShake",  &CSequenceTrack_CameraShake::StaticClass,  false, "A looping drift over the live view for a handheld feel, scaled by Intensity." },
        };

        const FSequenceTrackType* FindSequenceTrackType(FStringView Name)
        {
            for (const FSequenceTrackType& Type : GSequenceTrackTypes)
            {
                if (Name == Type.Name)
                {
                    return &Type;
                }
            }
            return nullptr;
        }

        FString SequenceTrackTypeName(const CSequenceTrack* Track)
        {
            for (const FSequenceTrackType& Type : GSequenceTrackTypes)
            {
                if (Track->GetClass() == Type.Class())
                {
                    return Type.Name;
                }
            }
            return FString(*Track->GetClass()->GetName().ToString());
        }

        const char* SequenceBindingKindName(ESequenceBindingKind Kind)
        {
            switch (Kind)
            {
                case ESequenceBindingKind::Spawn:  return "Spawn";
                case ESequenceBindingKind::Camera: return "Camera";
                default:                           return "Possess";
            }
        }

        int32 FindSequenceBinding(const CSequence* Sequence, FStringView Name)
        {
            for (int32 Index = 0; Index < Sequence->Bindings.Num(); ++Index)
            {
                if (Sequence->Bindings[Index].Name.ToString() == Name)
                {
                    return Index;
                }
            }
            return Constants::kIndexNone;
        }

        // A binding by name or by index, where empty means none.
        bool ParseSequenceBinding(const CSequence* Sequence, const FString& Text, int32& Out, FString& OutError)
        {
            Out = Constants::kIndexNone;
            if (Text.IsEmpty())
            {
                return true;
            }

            Out = FindSequenceBinding(Sequence, FStringView(Text));
            if (Out != Constants::kIndexNone)
            {
                return true;
            }

            char* End = nullptr;
            const long Parsed = std::strtol(*Text, &End, 10);
            if (End != nullptr && *End == '\0' && Parsed >= 0 && Parsed < (long)Sequence->Bindings.Num())
            {
                Out = (int32)Parsed;
                return true;
            }

            OutError = Lumina::Format("The sequence has no binding '{}'. sequence.describe lists them.", Text);
            return false;
        }

        FString SequenceBindingName(const CSequence* Sequence, int32 Index)
        {
            return Index >= 0 && Index < Sequence->Bindings.Num() ? FString(*Sequence->Bindings[Index].Name.ToString()) : FString();
        }

        bool CheckSequenceTrack(const CSequence* Sequence, int32 Track, FString& OutError)
        {
            if (Track < 0 || Track >= Sequence->Tracks.Num() || Sequence->Tracks[Track] == nullptr)
            {
                OutError = Lumina::Format("Track {} does not exist. This sequence has {} track(s).", Track, Sequence->Tracks.Num());
                return false;
            }
            return true;
        }

        CSequenceTrack* NewSequenceTrack(CSequence* Sequence, CClass* Class, int32 Binding)
        {
            CSequenceTrack* Track = NewObject<CSequenceTrack>(Class, Sequence->GetPackage());
            Track->BindingIndex = Binding;
            Sequence->Tracks.Add(Track);
            return Track;
        }

        template<typename T>
        T* FindSequenceTrack(CSequence* Sequence, int32 Binding, int32* OutIndex = nullptr)
        {
            for (int32 Index = 0; Index < (int32)Sequence->Tracks.Num(); ++Index)
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
        T* FindOrAddSequenceTrack(CSequence* Sequence, int32 Binding, int32& OutIndex)
        {
            if (T* Existing = FindSequenceTrack<T>(Sequence, Binding, &OutIndex))
            {
                return Existing;
            }
            OutIndex = (int32)Sequence->Tracks.Num();
            return static_cast<T*>(NewSequenceTrack(Sequence, T::StaticClass(), Binding));
        }

        int32 IndexOfSequenceTrack(const CSequence* Sequence, const CSequenceTrack* Track)
        {
            for (int32 Index = 0; Index < Sequence->Tracks.Num(); ++Index)
            {
                if (Sequence->Tracks[Index].Get() == Track)
                {
                    return Index;
                }
            }
            return Constants::kIndexNone;
        }

        bool ParseSequenceJson(const FString& Text, FJsonValue& Out, FString& OutError)
        {
            if (!FJsonValue::Parse(Text, Out))
            {
                OutError = Lumina::Format("'{}' is not valid JSON.", Text);
                return false;
            }
            return true;
        }

        bool ReadSequenceVector(const FJsonValue& Value, FVector3& Out)
        {
            if (Value.IsArray() && Value.Num() == 3 && Value[0].IsNumber() && Value[1].IsNumber() && Value[2].IsNumber())
            {
                Out = FVector3(Value[0].AsFloat(), Value[1].AsFloat(), Value[2].AsFloat());
                return true;
            }
            if (Value["X"].IsNumber() && Value["Y"].IsNumber() && Value["Z"].IsNumber())
            {
                Out = FVector3(Value["X"].AsFloat(), Value["Y"].AsFloat(), Value["Z"].AsFloat());
                return true;
            }
            return false;
        }

        bool ParseSequenceInterp(FStringView Text, ECurveInterpMode& Out)
        {
            if (Text.IsEmpty() || Text == "Cubic")  { Out = ECurveInterpMode::Cubic; return true; }
            if (Text == "Linear")                 { Out = ECurveInterpMode::Linear; return true; }
            if (Text == "Constant")               { Out = ECurveInterpMode::Constant; return true; }
            return false;
        }

        void KeySequenceCurve(SKeyedCurve& Curve, float Time, float Value, ECurveInterpMode Mode)
        {
            const int32 Index = Curve.UpdateOrAddKey(Time, Value);
            Curve.Keys[Index].InterpMode = Mode;
        }

        void KeySequenceVector(SSequenceVectorCurve& Curve, float Time, const FVector3& Value, ECurveInterpMode Mode)
        {
            Curve.bEnabled = true;
            KeySequenceCurve(Curve.X.Curve, Time, Value.x, Mode);
            KeySequenceCurve(Curve.Y.Curve, Time, Value.y, Mode);
            KeySequenceCurve(Curve.Z.Curve, Time, Value.z, Mode);
        }

        void FinishSequenceVector(SSequenceVectorCurve& Curve, bool bAngles)
        {
            if (bAngles)
            {
                Curve.UnwindAngles();
            }
            Curve.X.Curve.ComputeAutoTangents();
            Curve.Y.Curve.ComputeAutoTangents();
            Curve.Z.Curve.ComputeAutoTangents();
        }

        FVector3 SequenceLookAtEuler(const FVector3& From, const FVector3& To)
        {
            return Math::Degrees(Math::YawFirstEulerAngles(Math::FindLookAtRotation(To, From)));
        }

        CWorld* RequireSequenceGameWorld(FString& OutError)
        {
            CWorld* World = FindGameWorld();
            if (World == nullptr)
            {
                OutError = "No game is playing. Start one with editor.play first, since sequences run in the game world.";
            }
            return World;
        }

        template<typename TFunc>
        void ForEachSequencePlayer(CWorld* World, const CSequence* Only, TFunc&& Visit)
        {
            TVector<ECS::FEntity> Players;
            auto View = World->View<SSequencePlayerComponent>();
            for (ECS::FEntity Entity : View)
            {
                const SSequencePlayerComponent& Player = View.Get<SSequencePlayerComponent>(Entity);
                if (Only == nullptr || Player.Sequence.Get() == Only)
                {
                    Players.Add(Entity);
                }
            }
            for (ECS::FEntity Entity : Players)
            {
                Visit(Entity);
            }
        }

        FString DescribeSequenceTrackValues(CSequenceTrack* Track, bool bSummary)
        {
            FJsonValue Values;
            if (!Agent::WriteStruct(Track->GetClass(), Track, Values).IsValid() || !Values.IsObject())
            {
                return FString();
            }

            if (bSummary)
            {
                // A curve reads as its key count and range, which is what matters when surveying a long edit.
                std::function<void(FJsonValue&)> Collapse = [&Collapse](FJsonValue& Node)
                {
                    if (!Node.IsObject())
                    {
                        return;
                    }
                    if (const FJsonValue& Keys = Node["Keys"]; Keys.IsArray())
                    {
                        FJsonValue Summary = FJsonValue::MakeObject({ { "KeyCount", Keys.Num() } });
                        if (!Keys.IsEmpty())
                        {
                            Summary.Set("From", Keys[0].GetValue("Time", 0.0f));
                            Summary.Set("To", Keys[Keys.Num() - 1].GetValue("Time", 0.0f));
                        }
                        Node = Move(Summary);
                        return;
                    }
                    for (FJsonMember& Entry : Node.Members())
                    {
                        Collapse(Entry.Value);
                    }
                };
                Collapse(Values);
            }
            return Values.ToString();
        }

        // With Ground set a key's heights are above the terrain, so a shot can be placed without knowing the island's elevation.
        void GroundSequencePoint(FVector3& Point, bool bGround)
        {
            if (bGround)
            {
                const float Height = GroundHeightAt(Point.x, Point.z);
                Point.y += Height > GNoGroundHeight ? Height : 0.0f;
            }
        }

        void FillMovieStatus(const MovieCapture::FStatus& Status, SMovieStatusResult& Out)
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

        void RegisterSequenceCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateSequenceParams, SCreateSequenceResult>(
                Owner, "sequence.create",
                "Create an empty cinematic sequence asset under /Game/Content. Build it with sequence.shot, sequence.track and sequence.set_keys; play it with sequence.playback.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateSequenceParams& In, SCreateSequenceResult& Out)
                {
                    if (In.Name.IsEmpty())
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

                    CSequence* Sequence = CFactory::CreateNewOf<CSequence>(FStringView(*Path, Path.Len()));
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
                    Out.Path = FString(*Path);
                    Out.Guid = FString(*Sequence->GetGUID().ToString());
                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterSequenceSave(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceRefParams, SCreateSequenceResult>(
                Owner, "sequence.save",
                "Write a sequence's edits to disk.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceRefParams& In, SCreateSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CPackage* Package = Sequence->GetPackage();
                    const FFixedString Path = Package != nullptr ? Package->GetPackagePath() : FFixedString();
                    if (Package == nullptr || !CPackage::SavePackage(Package, FStringView(*Path, Path.Len())))
                    {
                        return Agent::FToolResult::Error("Could not save the sequence.");
                    }

                    Out.Path = FString(*Path);
                    Out.Guid = FString(*Sequence->GetGUID().ToString());
                    return Agent::FToolResult::Ok(Lumina::Format("Saved {}.", Out.Path));
                });
        }

        void RegisterSequenceSetSettings(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceSettingsParams, SDescribeSequenceResult>(
                Owner, "sequence.set_settings",
                "Change a sequence's length, frame rate or letterbox.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceSettingsParams& In, SDescribeSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
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
                    MarkAssetDirty(Sequence);

                    Out.Name = FString(*Sequence->GetName().ToString());
                    Out.Duration = Sequence->Duration;
                    Out.FrameRate = Sequence->FrameRate;
                    Out.LetterboxAspect = Sequence->LetterboxAspect;
                    return Agent::FToolResult::Ok("Updated.");
                });
        }

        void RegisterSequenceDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeSequenceParams, SDescribeSequenceResult>(
                Owner, "sequence.describe",
                "Report a sequence's settings, bindings and every track with its values.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeSequenceParams& In, SDescribeSequenceResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name = FString(*Sequence->GetName().ToString());
                    Out.Duration = Sequence->Duration;
                    Out.FrameRate = Sequence->FrameRate;
                    Out.LetterboxAspect = Sequence->LetterboxAspect;

                    for (int32 Index = 0; Index < Sequence->Bindings.Num(); ++Index)
                    {
                        const SSequenceBinding& Binding = Sequence->Bindings[Index];
                        SSequenceBindingInfo Info;
                        Info.Index = Index;
                        Info.Name = FString(*Binding.Name.ToString());
                        Info.Kind = SequenceBindingKindName(Binding.Kind);
                        if (Binding.SpawnPrefab.IsValid())
                        {
                            Info.Prefab = FString(*Binding.SpawnPrefab->GetGUID().ToString());
                        }
                        Out.Bindings.Add(Move(Info));
                    }

                    for (int32 Index = 0; Index < Sequence->Tracks.Num(); ++Index)
                    {
                        CSequenceTrack* Track = Sequence->Tracks[Index].Get();
                        if (Track == nullptr)
                        {
                            continue;
                        }
                        SSequenceTrackInfo Info;
                        Info.Index = Index;
                        Info.Type = SequenceTrackTypeName(Track);
                        Info.Binding = SequenceBindingName(Sequence, Track->BindingIndex);
                        Info.bEnabled = Track->bEnabled;
                        Info.Values = DescribeSequenceTrackValues(Track, In.bSummary);
                        Out.Tracks.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} binding(s), {} track(s), {:.2f}s.", Out.Bindings.Num(), Out.Tracks.Num(), Out.Duration));
                });
        }

        void RegisterSequenceTrackTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceTrackTypesParams, SSequenceTrackTypesResult>(
                Owner, "sequence.track_types",
                "List the kinds of track a sequence can hold.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::Any,
                [](const SSequenceTrackTypesParams& In, SSequenceTrackTypesResult& Out)
                {
                    for (const FSequenceTrackType& Type : GSequenceTrackTypes)
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
                                Info.Schema = Schema.Schema.ToString();
                            }
                        }
                        Out.Types.Add(Move(Info));
                    }
                    return Agent::FToolResult::Ok(Lumina::Format("{} track type(s).", Out.Types.Num()));
                });
        }

        Agent::FToolResult SequenceAddBinding(const SSequenceBindingParams& In, SSequenceIndexResult& Out)
        {
            CSequence* Sequence = nullptr;
            FString Error;
            if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            if (In.Name.IsEmpty())
            {
                return Agent::FToolResult::Error("A binding needs a name.");
            }
            if (FindSequenceBinding(Sequence, FStringView(In.Name)) != Constants::kIndexNone)
            {
                return Agent::FToolResult::Error(Lumina::Format("The sequence already has a binding named '{}'.", In.Name));
            }

            SSequenceBinding Binding;
            Binding.Name = FName(*In.Name);
            if (In.Kind == "Spawn")
            {
                Binding.Kind = ESequenceBindingKind::Spawn;
                CObject* Asset = nullptr;
                if (!Agent::ResolveAssetObject(FStringView(In.Prefab), Asset, Error) || Cast<CPrefab>(Asset) == nullptr)
                {
                    return Agent::FToolResult::Error(Error.IsEmpty() ? FString("Prefab has to name a prefab asset.") : Error);
                }
                Binding.SpawnPrefab = Cast<CPrefab>(Asset);
            }
            else if (In.Kind == "Camera")
            {
                Binding.Kind = ESequenceBindingKind::Camera;
            }
            else if (!In.Kind.IsEmpty() && In.Kind != "Possess")
            {
                return Agent::FToolResult::Error("Kind has to be Possess, Spawn or Camera.");
            }

            Out.Index = Sequence->Bindings.Num();
            Sequence->Bindings.Add(Binding);
            MarkAssetDirty(Sequence);
            return Agent::FToolResult::Ok(Lumina::Format("Binding {} is {}.", Out.Index, In.Name));
        }

        Agent::FToolResult SequenceRemoveBinding(const SSequenceBindingParams& In, SSequenceIndexResult& Out)
        {
            CSequence* Sequence = nullptr;
            FString Error;
            int32 Removed = Constants::kIndexNone;
            if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !ParseSequenceBinding(Sequence, In.Binding, Removed, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            if (Removed == Constants::kIndexNone)
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

            for (int32 Index = Sequence->Tracks.Num() - 1; Index >= 0; --Index)
            {
                CSequenceTrack* Track = Sequence->Tracks[Index].Get();
                if (Track == nullptr || Track->BindingIndex == Removed)
                {
                    Sequence->Tracks.RemoveAt(Index);
                    continue;
                }
                Renumber(Track->BindingIndex);

                if (CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track))
                {
                    for (int32 Cut = Cuts->Cuts.Num() - 1; Cut >= 0; --Cut)
                    {
                        if (Cuts->Cuts[Cut].BindingIndex == Removed)
                        {
                            Cuts->Cuts.RemoveAt(Cut);
                            continue;
                        }
                        Renumber(Cuts->Cuts[Cut].BindingIndex);
                    }
                }
                else if (CSequenceTrack_LookAt* LookAt = Cast<CSequenceTrack_LookAt>(Track))
                {
                    if (LookAt->TargetBindingIndex == Removed)
                    {
                        LookAt->TargetBindingIndex = Constants::kIndexNone;
                    }
                    Renumber(LookAt->TargetBindingIndex);
                }
            }

            Sequence->Bindings.RemoveAt(Removed);
            MarkAssetDirty(Sequence);
            Out.Index = Removed;
            return Agent::FToolResult::Ok(Lumina::Format("Removed binding {}.", Removed));
        }

        void RegisterSequenceBinding(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceBindingParams, SSequenceIndexResult>(
                Owner, "sequence.binding",
                "Add something for the sequence to drive (an entity by Name, a prefab spawned for its length, or a camera), or Remove one with its tracks and cuts, renumbering later bindings.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceBindingParams& In, SSequenceIndexResult& Out)
                {
                    return In.Action == ESequenceBindingAction::Remove ? SequenceRemoveBinding(In, Out) : SequenceAddBinding(In, Out);
                });
        }

        Agent::FToolResult SequenceAddTrack(const SSequenceTrackParams& In, SSequenceIndexResult& Out)
        {
            CSequence* Sequence = nullptr;
            FString Error;
            int32 Binding = Constants::kIndexNone;
            if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !ParseSequenceBinding(Sequence, In.Binding, Binding, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            const FSequenceTrackType* Type = FindSequenceTrackType(FStringView(In.Type));
            if (Type == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("'{}' is not a track type. sequence.track_types lists them.", In.Type));
            }
            if (Type->bNeedsBinding && Binding == Constants::kIndexNone)
            {
                return Agent::FToolResult::Error(Lumina::Format("A {} track drives a binding, so name one.", Type->Name));
            }

            CSequenceTrack* Track = NewSequenceTrack(Sequence, Type->Class(), Binding);
            Out.Index = Sequence->Tracks.Num() - 1;

            if (!In.Settings.IsEmpty())
            {
                FJsonValue Settings;
                if (!ParseSequenceJson(In.Settings, Settings, Error))
                {
                    Sequence->Tracks.Pop();
                    return Agent::FToolResult::Error(Error);
                }
                if (!Settings.IsObject() && !Settings.IsNull())
                {
                    Sequence->Tracks.Pop();
                    return Agent::FToolResult::Error("Settings has to be a JSON object of property paths.");
                }
                for (const FJsonMember& Entry : Settings.Members())
                {
                    Agent::FResolvedProperty Resolved;
                    if (!Agent::ResolvePropertyPath(Track->GetClass(), Track, Entry.Key.View(), Resolved, Error))
                    {
                        Sequence->Tracks.Pop();
                        return Agent::FToolResult::Error(Error);
                    }
                    const Agent::FMarshalResult Read = Agent::ReadProperty(Entry.Value, Resolved.Property, Resolved.ValuePtr, Entry.Key.View());
                    if (!Read.IsValid())
                    {
                        Sequence->Tracks.Pop();
                        return Agent::FToolResult::Error(Read.Error);
                    }
                }
            }

            MarkAssetDirty(Sequence);
            return Agent::FToolResult::Ok(Lumina::Format("Track {} is a {} track.", Out.Index, Type->Name));
        }

        Agent::FToolResult SequenceRemoveTrack(const SSequenceTrackParams& In, SSequenceIndexResult& Out)
        {
            CSequence* Sequence = nullptr;
            FString Error;
            if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            if (In.Track < 0)
            {
                Out.Index = Sequence->Tracks.Num();
                Sequence->Tracks.Reset();
                MarkAssetDirty(Sequence);
                return Agent::FToolResult::Ok(Lumina::Format("Removed {} track(s).", Out.Index));
            }

            if (!CheckSequenceTrack(Sequence, In.Track, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            Sequence->Tracks.RemoveAt(In.Track);
            MarkAssetDirty(Sequence);
            Out.Index = In.Track;
            return Agent::FToolResult::Ok(Lumina::Format("Removed track {}. Later tracks moved down one.", In.Track));
        }

        void RegisterSequenceTrack(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceTrackParams, SSequenceIndexResult>(
                Owner, "sequence.track",
                "Add a track of a type from sequence.track_types, with optional Settings fields, or Remove the track at index Track (negative removes all).",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceTrackParams& In, SSequenceIndexResult& Out)
                {
                    return In.Action == ESequenceTrackAction::Remove ? SequenceRemoveTrack(In, Out) : SequenceAddTrack(In, Out);
                });
        }

        void RegisterSequenceSetTrackProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetSequenceTrackPropertyParams, SSequencePropertyResult>(
                Owner, "sequence.set_track_property",
                "Set any field on a track by path, such as bEnabled, TargetOffset, Color or Clips[0].Volume.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetSequenceTrackPropertyParams& In, SSequencePropertyResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !CheckSequenceTrack(Sequence, In.Track, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CSequenceTrack* Track = Sequence->Tracks[In.Track].Get();
                    Agent::FResolvedProperty Resolved;
                    if (!Agent::ResolvePropertyPath(Track->GetClass(), Track, FStringView(In.Path), Resolved, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Value;
                    if (!ParseSequenceJson(In.Value, Value, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Previous;
                    Agent::WriteProperty(Resolved.Property, Resolved.ValuePtr, Previous);
                    const Agent::FMarshalResult Read = Agent::ReadProperty(Value, Resolved.Property, Resolved.ValuePtr, FStringView(In.Path));
                    if (!Read.IsValid())
                    {
                        return Agent::FToolResult::Error(Read.Error);
                    }

                    FJsonValue Current;
                    Agent::WriteProperty(Resolved.Property, Resolved.ValuePtr, Current);
                    Out.Previous = Previous.ToString();
                    Out.Current = Current.ToString();
                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} is now {}.", In.Path, Out.Current));
                });
        }

        void RegisterSequenceSetKeys(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetSequenceKeysParams, SSetSequenceKeysResult>(
                Owner, "sequence.set_keys",
                "Key one curve (Channel) on a track. Value is [x,y,z] for a vector channel or a number for a single curve. Rotation keys are unwound so they turn the short way.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetSequenceKeysParams& In, SSetSequenceKeysResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !CheckSequenceTrack(Sequence, In.Track, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ECurveInterpMode DefaultMode;
                    if (!ParseSequenceInterp(FStringView(In.Interp), DefaultMode))
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

                    FJsonValue Keys;
                    if (!ParseSequenceJson(In.Keys, Keys, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (!Keys.IsArray())
                    {
                        return Agent::FToolResult::Error("Keys has to be an array.");
                    }

                    SSequenceVectorCurve* Vector = bVector ? static_cast<SSequenceVectorCurve*>(Resolved.ValuePtr) : nullptr;
                    SCurve* Scalar = bVector ? nullptr : static_cast<SCurve*>(Resolved.ValuePtr);

                    if (In.bReplace)
                    {
                        if (Vector != nullptr)
                        {
                            Vector->X.Curve.Keys.Reset();
                            Vector->Y.Curve.Keys.Reset();
                            Vector->Z.Curve.Keys.Reset();
                        }
                        else
                        {
                            Scalar->Curve.Keys.Reset();
                        }
                    }

                    for (const FJsonValue& Entry : Keys.AsArray())
                    {
                        if (!Entry.IsObject() || !Entry["Time"].IsNumber() || !Entry.Contains("Value"))
                        {
                            return Agent::FToolResult::Error("Every key needs a numeric Time and a Value.");
                        }

                        ECurveInterpMode Mode = DefaultMode;
                        if (Entry["Interp"].IsString() && !ParseSequenceInterp(Entry["Interp"].AsString(), Mode))
                        {
                            return Agent::FToolResult::Error("A key's Interp has to be Cubic, Linear or Constant.");
                        }

                        const float Time = Entry["Time"].AsFloat();
                        if (Vector != nullptr)
                        {
                            FVector3 Value;
                            if (!ReadSequenceVector(Entry["Value"], Value))
                            {
                                return Agent::FToolResult::Error("A vector channel's Value is [x,y,z].");
                            }
                            KeySequenceVector(*Vector, Time, Value, Mode);
                        }
                        else
                        {
                            if (!Entry["Value"].IsNumber())
                            {
                                return Agent::FToolResult::Error("A single curve's Value is a number.");
                            }
                            KeySequenceCurve(Scalar->Curve, Time, Entry["Value"].AsFloat(), Mode);
                        }
                    }

                    if (Vector != nullptr)
                    {
                        const bool bAngles = In.Channel.Find("Rotation") != Constants::kIndexNone;
                        FinishSequenceVector(*Vector, bAngles);
                        Out.KeyCount = Vector->X.Curve.NumKeys();
                    }
                    else
                    {
                        Scalar->bUseAsset = false;
                        Scalar->Curve.ComputeAutoTangents();
                        Out.KeyCount = Scalar->Curve.NumKeys();
                    }

                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} now has {} key(s).", In.Channel, Out.KeyCount));
                });
        }

        int32 AddSequenceCut(CSequence* Sequence, int32 Camera, float Start, float End, float BlendTime)
        {
            int32 TrackIndex = Constants::kIndexNone;
            CSequenceTrack_CameraCut* Track = FindOrAddSequenceTrack<CSequenceTrack_CameraCut>(Sequence, Constants::kIndexNone, TrackIndex);

            // A new shot takes over its range, trimming or dropping the cuts it lands on.
            TVector<SSequenceCameraCut> Kept;
            for (const SSequenceCameraCut& Cut : Track->Cuts)
            {
                if (Cut.EndTime <= Start || Cut.StartTime >= End)
                {
                    Kept.Add(Cut);
                    continue;
                }
                if (Cut.StartTime < Start)
                {
                    SSequenceCameraCut Before = Cut;
                    Before.EndTime = Start;
                    Kept.Add(Before);
                }
                if (Cut.EndTime > End)
                {
                    SSequenceCameraCut After = Cut;
                    After.StartTime = End;
                    After.BlendTime = 0.0f;
                    Kept.Add(After);
                }
            }

            SSequenceCameraCut Cut;
            Cut.BindingIndex = Camera;
            Cut.StartTime = Start;
            Cut.EndTime = End;
            Cut.BlendTime = Math::Max(BlendTime, 0.0f);
            Kept.Add(Cut);

            Algo::Sort(Kept, [](const SSequenceCameraCut& A, const SSequenceCameraCut& B) { return A.StartTime < B.StartTime; });
            Track->Cuts = Move(Kept);

            Sequence->Duration = Math::Max(Sequence->Duration, End);
            return TrackIndex;
        }

        void RegisterSequenceAddCameraCut(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceCameraCutParams, SSequenceIndexResult>(
                Owner, "sequence.add_camera_cut",
                "Make a camera binding the live view from Start to End, trimming any cut it overlaps.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceCameraCutParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Camera = Constants::kIndexNone;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !ParseSequenceBinding(Sequence, In.Camera, Camera, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (Camera == Constants::kIndexNone || In.End <= In.Start)
                    {
                        return Agent::FToolResult::Error("A cut needs a camera binding and an End after its Start.");
                    }

                    Out.Index = AddSequenceCut(Sequence, Camera, In.Start, In.End, In.BlendTime);
                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} is live from {:.2f}s to {:.2f}s.", SequenceBindingName(Sequence, Camera), In.Start, In.End));
                });
        }

        void RegisterSequenceAddEvent(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceEventParams, SSequenceIndexResult>(
                Owner, "sequence.add_event",
                "Fire a named event at a time, for gameplay to read through World.GetSequences().ConsumeEvents.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceEventParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Binding = Constants::kIndexNone;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !ParseSequenceBinding(Sequence, In.Binding, Binding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (In.Name.IsEmpty())
                    {
                        return Agent::FToolResult::Error("An event needs a name.");
                    }

                    int32 TrackIndex = Constants::kIndexNone;
                    CSequenceTrack_Event* Track = FindOrAddSequenceTrack<CSequenceTrack_Event>(Sequence, Binding, TrackIndex);

                    SSequenceEventKey Key;
                    Key.Time = Math::Max(In.Time, 0.0f);
                    Key.Name = FName(*In.Name);
                    Key.Payload = In.Payload;
                    auto Where = Algo::UpperBound(Track->Keys, Key.Time,
                        [](float Time, const SSequenceEventKey& Other) { return Time < Other.Time; });
                    Track->Keys.Insert(Key, static_cast<int32>(Where - Track->Keys.begin()));

                    Out.Index = TrackIndex;
                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} fires at {:.2f}s.", In.Name, Key.Time));
                });
        }

        void RegisterSequenceAddAudio(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddSequenceAudioParams, SSequenceIndexResult>(
                Owner, "sequence.add_audio",
                "Start a sound at a time, as music in stereo or at a binding in the world.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddSequenceAudioParams& In, SSequenceIndexResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    int32 Binding = Constants::kIndexNone;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error) || !ParseSequenceBinding(Sequence, In.Binding, Binding, Error))
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

                    int32 TrackIndex = Constants::kIndexNone;
                    CSequenceTrack_Audio* Track = FindOrAddSequenceTrack<CSequenceTrack_Audio>(Sequence, Binding, TrackIndex);
                    SSequenceAudioClip Clip;
                    Clip.StartTime = Math::Max(In.Start, 0.0f);
                    Clip.Sound = Sound;
                    Clip.Volume = Math::Max(In.Volume, 0.0f);
                    Clip.Pitch = Math::Max(In.Pitch, 0.01f);
                    // A clip bound to nothing is score, so it follows the music volume rather than the effects.
                    Clip.Bus = Binding == Constants::kIndexNone ? EAudioBus::Music : EAudioBus::SFX;
                    Track->Clips.Add(Clip);

                    Out.Index = TrackIndex;
                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} starts at {:.2f}s.", Sound->GetName(), Clip.StartTime));
                });
        }

        void KeySequenceCameraProperty(CSequence* Sequence, int32 Camera, const char* Path, float Time, float Value, ECurveInterpMode Mode, TVector<int32>& OutTracks)
        {
            CSequenceTrack_Property* Found = nullptr;
            int32 FoundIndex = Constants::kIndexNone;
            for (int32 Index = 0; Index < Sequence->Tracks.Num(); ++Index)
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
                FoundIndex = Sequence->Tracks.Num();
                Found = static_cast<CSequenceTrack_Property*>(NewSequenceTrack(Sequence, CSequenceTrack_Property::StaticClass(), Camera));
                Found->ComponentType = FName("SCameraComponent");
                Found->PropertyPath = Path;
            }

            KeySequenceCurve(Found->Curve.Curve, Time, Value, Mode);
            Found->Curve.Curve.ComputeAutoTangents();
            OutTracks.AddUnique(FoundIndex);
        }

        void RegisterSequenceShot(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequenceShotParams, SSequenceShotResult>(
                Owner, "sequence.shot",
                "Author a camera shot in one call: a camera binding with its move, aim, lens and cut. Keys aim with Rotation or Target; LookAt or LookAtPoint aims every frame. "
                "Use one camera per shot.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequenceShotParams& In, SSequenceShotResult& Out)
                {
                    CSequence* Sequence = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (In.End <= In.Start)
                    {
                        return Agent::FToolResult::Error("A shot needs an End after its Start.");
                    }

                    ECurveInterpMode Mode;
                    if (!ParseSequenceInterp(FStringView(In.Interp), Mode))
                    {
                        return Agent::FToolResult::Error("Interp has to be Cubic, Linear or Constant.");
                    }

                    FJsonValue Keys;
                    if (!ParseSequenceJson(In.Keys.IsEmpty() ? FString("[]") : In.Keys, Keys, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }
                    if (!Keys.IsArray() || Keys.IsEmpty())
                    {
                        return Agent::FToolResult::Error("A shot needs at least one key with a Location.");
                    }

                    int32 LookAtBinding = Constants::kIndexNone;
                    if (!ParseSequenceBinding(Sequence, In.LookAt, LookAtBinding, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FVector3 LookAtPoint(0.0f);
                    bool bHasLookAtPoint = false;
                    if (!In.LookAtPoint.IsEmpty())
                    {
                        FJsonValue Point;
                        if (!ParseSequenceJson(In.LookAtPoint, Point, Error) || !ReadSequenceVector(Point, LookAtPoint))
                        {
                            return Agent::FToolResult::Error("LookAtPoint is [x,y,z].");
                        }
                        GroundSequencePoint(LookAtPoint, In.bGround);
                        bHasLookAtPoint = true;
                    }

                    FVector3 LookAtOffset(0.0f);
                    if (!In.LookAtOffset.IsEmpty())
                    {
                        FJsonValue Offset;
                        if (!ParseSequenceJson(In.LookAtOffset, Offset, Error) || !ReadSequenceVector(Offset, LookAtOffset))
                        {
                            return Agent::FToolResult::Error("LookAtOffset is [x,y,z].");
                        }
                    }

                    FString CameraName = In.Camera;
                    if (CameraName.IsEmpty())
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
                        while (FindSequenceBinding(Sequence, FStringView(CameraName)) != Constants::kIndexNone);
                    }

                    int32 Camera = FindSequenceBinding(Sequence, FStringView(CameraName));
                    if (Camera == Constants::kIndexNone)
                    {
                        SSequenceBinding Binding;
                        Binding.Name = FName(*CameraName);
                        Binding.Kind = ESequenceBindingKind::Camera;
                        Camera = Sequence->Bindings.Num();
                        Sequence->Bindings.Add(Binding);
                    }

                    int32 TransformIndex = Constants::kIndexNone;
                    CSequenceTrack_Transform* Transform = FindOrAddSequenceTrack<CSequenceTrack_Transform>(Sequence, Camera, TransformIndex);
                    Out.Tracks.Add(TransformIndex);

                    const bool bFollows = LookAtBinding != Constants::kIndexNone || bHasLookAtPoint;
                    for (const FJsonValue& Entry : Keys.AsArray())
                    {
                        FVector3 Location;
                        if (!Entry.IsObject() || !ReadSequenceVector(Entry["Location"], Location))
                        {
                            return Agent::FToolResult::Error("Every shot key needs a Location as [x,y,z].");
                        }

                        const FJsonValue* GroundValue = Entry.Find("Ground");
                        const FJsonValue* TimeValue = Entry.Find("Time");
                        if (GroundValue != nullptr && !GroundValue->IsBool())
                        {
                            return Agent::FToolResult::Error("A shot key's Ground has to be true or false.");
                        }
                        if (TimeValue != nullptr && !TimeValue->IsNumber())
                        {
                            return Agent::FToolResult::Error("A shot key's Time has to be a number.");
                        }

                        const bool bGround = GroundValue != nullptr ? GroundValue->AsBool() : In.bGround;
                        GroundSequencePoint(Location, bGround);
                        const float Time = In.Start + Math::Clamp(TimeValue != nullptr ? TimeValue->AsFloat() : 0.0f, 0.0f, In.End - In.Start);
                        KeySequenceVector(Transform->Location, Time, Location, Mode);

                        FVector3 Rotation;
                        FVector3 Target;
                        if (Entry.Contains("Rotation") && ReadSequenceVector(Entry["Rotation"], Rotation))
                        {
                            KeySequenceVector(Transform->Rotation, Time, Rotation, Mode);
                        }
                        else if (Entry.Contains("Target") && ReadSequenceVector(Entry["Target"], Target))
                        {
                            GroundSequencePoint(Target, bGround);
                            KeySequenceVector(Transform->Rotation, Time, SequenceLookAtEuler(Location, Target), Mode);
                        }
                        else if (bHasLookAtPoint)
                        {
                            // Keyed too, so the rotation is right even before the look-at track takes over.
                            KeySequenceVector(Transform->Rotation, Time, SequenceLookAtEuler(Location, LookAtPoint), Mode);
                        }

                        if (Entry["FOV"].IsNumber())
                        {
                            KeySequenceCameraProperty(Sequence, Camera, "FOV", Time, Entry["FOV"].AsFloat(), Mode, Out.Tracks);
                        }
                        if (Entry["Focus"].IsNumber())
                        {
                            KeySequenceCameraProperty(Sequence, Camera, "PostProcess.DepthOfFieldFocusDistance", Time, Entry["Focus"].AsFloat(), Mode, Out.Tracks);
                        }
                        if (Entry["FStop"].IsNumber())
                        {
                            KeySequenceCameraProperty(Sequence, Camera, "PostProcess.DepthOfFieldFStop", Time, Entry["FStop"].AsFloat(), Mode, Out.Tracks);
                        }
                    }

                    FinishSequenceVector(Transform->Location, false);
                    if (Transform->Rotation.bEnabled)
                    {
                        FinishSequenceVector(Transform->Rotation, true);
                    }

                    if (In.FOV > 0.0f)
                    {
                        KeySequenceCameraProperty(Sequence, Camera, "FOV", In.Start, In.FOV, ECurveInterpMode::Constant, Out.Tracks);
                    }
                    if (In.FStop > 0.0f)
                    {
                        KeySequenceCameraProperty(Sequence, Camera, "PostProcess.DepthOfFieldFStop", In.Start, In.FStop, ECurveInterpMode::Constant, Out.Tracks);
                    }

                    if (bFollows)
                    {
                        int32 LookAtIndex = Constants::kIndexNone;
                        CSequenceTrack_LookAt* LookAt = FindOrAddSequenceTrack<CSequenceTrack_LookAt>(Sequence, Camera, LookAtIndex);
                        LookAt->TargetBindingIndex = LookAtBinding;
                        LookAt->TargetOffset = LookAtBinding != Constants::kIndexNone ? LookAtOffset : LookAtPoint + LookAtOffset;
                        LookAt->bAutoFocus = In.bAutoFocus && In.FStop > 0.0f;
                        Out.Tracks.Add(LookAtIndex);
                    }

                    if (In.Handheld > 0.0f)
                    {
                        // The shake runs over whatever view is live, so its strength is keyed to this shot's range alone.
                        int32 ShakeIndex = Constants::kIndexNone;
                        CSequenceTrack_CameraShake* Shake = FindOrAddSequenceTrack<CSequenceTrack_CameraShake>(Sequence, Constants::kIndexNone, ShakeIndex);
                        KeySequenceCurve(Shake->Intensity.Curve, In.Start - 0.001f, 0.0f, ECurveInterpMode::Constant);
                        KeySequenceCurve(Shake->Intensity.Curve, In.Start, In.Handheld, ECurveInterpMode::Constant);
                        KeySequenceCurve(Shake->Intensity.Curve, In.End, 0.0f, ECurveInterpMode::Constant);
                        Out.Tracks.AddUnique(ShakeIndex);
                    }

                    if (In.bCut)
                    {
                        const int32 CutIndex = AddSequenceCut(Sequence, Camera, In.Start, In.End, In.BlendTime);
                        Out.Tracks.Add(CutIndex);
                    }
                    Sequence->Duration = Math::Max(Sequence->Duration, In.End);

                    Out.Camera = CameraName;
                    Out.CameraBinding = Camera;
                    MarkAssetDirty(Sequence);
                    return Agent::FToolResult::Ok(Lumina::Format("{} covers {:.2f}s to {:.2f}s with {} key(s).", CameraName, In.Start, In.End, Keys.Num()));
                });
        }

        void CollectSequenceState(CWorld* World, SSequenceStateResult& Out)
        {
            ForEachSequencePlayer(World, nullptr, [World, &Out](ECS::FEntity Entity)
            {
                const SSequencePlayerComponent& Player = World->GetComponent<SSequencePlayerComponent>(Entity);
                SSequencePlayerInfo Info;
                Info.Player = Agent::FEntityTokens::Mint(World->GetEntityRegistry(), Entity);
                Info.Time = Player.Time;
                Info.bPlaying = Player.bPlaying;
                Info.bPaused = Player.bPaused;
                if (const CSequence* Sequence = Player.Sequence.Get())
                {
                    Info.Sequence = FString(*Sequence->GetName().ToString());
                    Info.Duration = Sequence->Duration;
                    for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
                    {
                        const CSequenceTrack_CameraCut* Cuts = Cast<CSequenceTrack_CameraCut>(Track.Get());
                        const int32 Cut = Cuts != nullptr ? Cuts->FindCutAt(Player.Time) : Constants::kIndexNone;
                        if (Cut != Constants::kIndexNone)
                        {
                            Info.LiveCamera = SequenceBindingName(Sequence, Cuts->Cuts[Cut].BindingIndex);
                            break;
                        }
                    }
                }
                Out.Players.Add(Move(Info));
            });
        }

        Agent::FToolResult SequencePlay(const SSequencePlaybackParams& In, SSequenceStateResult& Out)
        {
            CSequence* Sequence = nullptr;
            FString Error;
            if (!ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
            {
                return Agent::FToolResult::Error(Error);
            }
            CWorld* World = RequireSequenceGameWorld(Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            ForEachSequencePlayer(World, Sequence, [World](ECS::FEntity Player) { World->GetSequences()->StopSequence(Player); });

            const ECS::FEntity Player = World->GetSequences()->PlaySequence(Sequence, In.bLoop, In.PlayRate);
            SSequencePlayerComponent& Component = World->GetComponent<SSequencePlayerComponent>(Player);
            if (In.StartTime > 0.0f)
            {
                Component.Seek(In.StartTime);
            }
            if (In.bPaused)
            {
                Component.Pause();
            }

            SSequencePlayerInfo& Info = Out.Players.EmplaceGetRef();
            Info.Player = Agent::FEntityTokens::Mint(World->GetEntityRegistry(), Player);
            Info.Sequence = FString(*Sequence->GetName().ToString());
            Info.Time = Component.Time;
            Info.Duration = Sequence->Duration;
            Info.bPlaying = true;
            Info.bPaused = In.bPaused;
            return Agent::FToolResult::Ok(Lumina::Format("Playing {} from {:.2f}s{}.", Info.Sequence, Info.Time, In.bPaused ? ", paused" : ""));
        }

        const char* SequencePlaybackActionName(ESequencePlaybackAction Action)
        {
            switch (Action)
            {
                case ESequencePlaybackAction::Pause:  return "Pause";
                case ESequencePlaybackAction::Resume: return "Resume";
                case ESequencePlaybackAction::Stop:   return "Stop";
                case ESequencePlaybackAction::Seek:   return "Seek";
                default:                              return "Restart";
            }
        }

        Agent::FToolResult SequenceControlPlayback(const SSequencePlaybackParams& In, SSequenceStateResult& Out)
        {
            FString Error;
            CWorld* World = RequireSequenceGameWorld(Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            CSequence* Only = nullptr;
            if (!In.Sequence.IsEmpty() && !ResolveEditableAsset(FStringView(In.Sequence), Only, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            int32 Touched = 0;
            ForEachSequencePlayer(World, Only, [&](ECS::FEntity Entity)
            {
                ++Touched;
                if (In.Action == ESequencePlaybackAction::Stop)
                {
                    World->GetSequences()->StopSequence(Entity);
                    return;
                }

                SSequencePlayerComponent& Player = World->GetComponent<SSequencePlayerComponent>(Entity);
                switch (In.Action)
                {
                    case ESequencePlaybackAction::Pause:  Player.Pause(); break;
                    case ESequencePlaybackAction::Resume: Player.Resume(); break;
                    case ESequencePlaybackAction::Seek:   Player.Seek(In.Time); break;
                    default:
                        Player.Seek(0.0f);
                        Player.Play(true);
                        break;
                }
            });

            CollectSequenceState(World, Out);
            return Touched > 0
                ? Agent::FToolResult::Ok(Lumina::Format("{} applied to {} player(s).", SequencePlaybackActionName(In.Action), Touched))
                : Agent::FToolResult::Error("No sequence is playing. Start one with sequence.playback Play.");
        }

        Agent::FToolResult SequencePlaybackState(SSequenceStateResult& Out)
        {
            FString Error;
            CWorld* World = RequireSequenceGameWorld(Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }
            CollectSequenceState(World, Out);
            return Agent::FToolResult::Ok(Lumina::Format("{} player(s).", Out.Players.Num()));
        }

        void RegisterSequencePlayback(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSequencePlaybackParams, SSequenceStateResult>(
                Owner, "sequence.playback",
                "Play a sequence in the running game (replacing any player of it), Pause, Resume, Stop, Seek or Restart players (Sequence empty means all), or report State.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSequencePlaybackParams& In, SSequenceStateResult& Out)
                {
                    switch (In.Action)
                    {
                        case ESequencePlaybackAction::Play:  return SequencePlay(In, Out);
                        case ESequencePlaybackAction::State: return SequencePlaybackState(Out);
                        default:                             return SequenceControlPlayback(In, Out);
                    }
                });
        }

        Agent::FToolResult MovieRenderStart(const SMovieRenderParams& In, SMovieStatusResult& Out)
        {
            FString Error;
            CWorld* World = RequireSequenceGameWorld(Error);
            if (World == nullptr)
            {
                return Agent::FToolResult::Error(Error);
            }

            CSequence* Sequence = nullptr;
            if (!In.Sequence.IsEmpty() && !ResolveEditableAsset(FStringView(In.Sequence), Sequence, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            MovieCapture::FSettings Settings;
            Settings.OutputPath = In.OutputPath;
            Settings.PngDirectory = In.PngDirectory;
            Settings.PngEvery = (uint32)Math::Max(In.PngEvery, 1);
            if (Settings.OutputPath.IsEmpty() && Settings.PngDirectory.IsEmpty())
            {
                const FString Stem = Sequence != nullptr ? FString(*Sequence->GetName().ToString()) : FString("Gameplay");
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
                ForEachSequencePlayer(World, nullptr, [World](ECS::FEntity Player) { World->GetSequences()->StopSequence(Player); });
                const ECS::FEntity Player = World->GetSequences()->PlaySequence(Sequence, false, 1.0f);
                World->GetComponent<SSequencePlayerComponent>(Player).Pause();

                Settings.OnRecordingStarted = [Sequence]()
                {
                    if (CWorld* Game = FindGameWorld())
                    {
                        ForEachSequencePlayer(Game, Sequence, [Game](ECS::FEntity Entity) { Game->GetComponent<SSequencePlayerComponent>(Entity).Resume(); });
                    }
                };
            }

            if (!MovieCapture::Start(Move(Settings), Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            FillMovieStatus(MovieCapture::GetStatus(), Out);
            return Agent::FToolResult::Ok(Out.FrameCount > 0
                ? Lumina::Format("Recording {} frame(s) to {}.", Out.FrameCount, Out.OutputPath.IsEmpty() ? Out.PngDirectory : Out.OutputPath)
                : Lumina::Format("Recording to {} until movie.render Stop.", Out.OutputPath.IsEmpty() ? Out.PngDirectory : Out.OutputPath));
        }

        Agent::FToolResult MovieRenderStatus(SMovieStatusResult& Out)
        {
            FillMovieStatus(MovieCapture::GetStatus(), Out);
            if (!Out.Error.IsEmpty())
            {
                return Agent::FToolResult::Error(Out.Error);
            }
            return Agent::FToolResult::Ok(Out.bActive
                ? Lumina::Format("{} of {} frame(s).", Out.FramesWritten, Out.FrameCount)
                : Lumina::Format("Finished with {} frame(s).", Out.FramesWritten));
        }

        Agent::FToolResult MovieRenderStop(SMovieStatusResult& Out)
        {
            MovieCapture::Stop();
            FillMovieStatus(MovieCapture::GetStatus(), Out);
            return Agent::FToolResult::Ok(Lumina::Format("Stopped with {} frame(s).", Out.FramesWritten));
        }

        void RegisterMovieRender(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMovieRenderParams, SMovieStatusResult>(
                Owner, "movie.render",
                "Start renders the game to an H.264 MP4 at a locked frame rate: a Sequence from start to end, else Seconds of gameplay. It returns at once; poll Status until bFinished. Stop ends early.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SMovieRenderParams& In, SMovieStatusResult& Out)
                {
                    switch (In.Action)
                    {
                        case EMovieRenderAction::Status: return MovieRenderStatus(Out);
                        case EMovieRenderAction::Stop:   return MovieRenderStop(Out);
                        default:                         return MovieRenderStart(In, Out);
                    }
                });
        }

    }

    using namespace MCPSequenceToolsInternal;

    void RegisterSequenceTools(FStringView Owner)
    {
        RegisterSequenceCreate(Owner);
        RegisterSequenceSave(Owner);
        RegisterSequenceSetSettings(Owner);
        RegisterSequenceDescribe(Owner);
        RegisterSequenceTrackTypes(Owner);
        RegisterSequenceBinding(Owner);
        RegisterSequenceTrack(Owner);
        RegisterSequenceSetTrackProperty(Owner);
        RegisterSequenceSetKeys(Owner);
        RegisterSequenceAddCameraCut(Owner);
        RegisterSequenceAddEvent(Owner);
        RegisterSequenceAddAudio(Owner);
        RegisterSequenceShot(Owner);
        RegisterSequencePlayback(Owner);
        RegisterMovieRender(Owner);
    }
}
