#include "ParticleOps.h"

#include "ParticleEmitterStack.h"
#include "ParticleStockModules.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/Factories/Factory.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Renderer/ShaderCompiler.h"
#include "UI/Tools/EditorAssetActions.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

namespace Lumina
{
    FParticleEmitterCompileJob::FParticleEmitterCompileJob() = default;
    FParticleEmitterCompileJob::~FParticleEmitterCompileJob() = default;
}

namespace Lumina::ParticleOps
{
    namespace
    {
        constexpr FStringView ModuleClassPrefix = "CParticleModule_";

        CParticleEmitter* EmitterAt(CParticleSystem* System, int32 EmitterIndex)
        {
            if (System == nullptr || EmitterIndex < 0 || EmitterIndex >= System->Emitters.Num())
            {
                return nullptr;
            }

            return System->Emitters[EmitterIndex].Get();
        }

        FVector2 LifetimeInputRange(const CParticleSystem* System, const SParticleParam& Input)
        {
            if (!Input.ParameterName.IsNone())
            {
                if (const SParticleParameter* Bound = System->FindUserParameter(Input.ParameterName))
                {
                    return Bound->Type == EParticleParameterType::Float ? FVector2(Bound->Scalar) : FVector2(Bound->Vector.x, Bound->Vector.y);
                }
            }

            return FVector2(Input.Constant.x, Input.Constant.y);
        }

        // The renderer keeps an emitter simulating and drawing only this long past its last spawn, so it must match the stack.
        void SyncLifetimeRange(const CParticleSystem* System, CParticleEmitter* Emitter, const CParticleEmitterStack* Stack)
        {
            constexpr float TemplateDefaultLifetime = 1.0f;
            FVector2 Range(TemplateDefaultLifetime);
            bool bFound = false;
            for (const TStrongObjectPtr<CParticleModule>& Module : Stack->SpawnModules)
            {
                const CParticleModule_Lifetime* Lifetime = Cast<CParticleModule_Lifetime>(Module.Get());
                if (Lifetime == nullptr || !Lifetime->bEnabled)
                {
                    continue;
                }

                const FVector2 Input = LifetimeInputRange(System, Lifetime->LifetimeRange);
                Range = bFound ? FVector2(Math::Min(Range.x, Input.x), Math::Max(Range.y, Input.y)) : Input;
                bFound = true;
            }

            Emitter->LifetimeRange = Range;
        }

        // An emitter asset keeps its stack in its own package, which for an emitter inside a system is the system's.
        CParticleEmitterStack* StackOf(const CParticleEmitter* Emitter)
        {
            if (Emitter == nullptr || Emitter->GetPackage() == nullptr || Emitter->AuthoringStackName.IsEmpty())
            {
                return nullptr;
            }
            return Cast<CParticleEmitterStack>(Emitter->GetPackage()->LoadObjectByName(FName(*Emitter->AuthoringStackName)));
        }

        FName UniqueStackName(CPackage* Package)
        {
            for (int32 Suffix = 0; ; ++Suffix)
            {
                const FName Candidate(*Format("ParticleStack_{}", Suffix));
                if (Package->LoadObjectByName(Candidate) == nullptr)
                {
                    return Candidate;
                }
            }
        }
    }

    CParticleEmitterStack* FindOrCreateStack(CParticleSystem* System, int32 EmitterIndex)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        if (Emitter == nullptr || Emitter->GetPackage() == nullptr)
        {
            return nullptr;
        }

        // Persisted on the emitter, so reordering or renaming never re-points a stack.
        if (Emitter->AuthoringStackName.IsEmpty())
        {
            Emitter->AuthoringStackName = FString("ParticleStack_") + *Format("{}", EmitterIndex);
        }

        CParticleEmitterStack* Stack = StackOf(Emitter);
        if (Stack == nullptr)
        {
            Stack = NewObject<CParticleEmitterStack>(Emitter->GetPackage(), Emitter->AuthoringStackName);
            Stack->EnsureDefaultStack();
        }

        return Stack;
    }

    CParticleEmitterStack* FindStack(CParticleSystem* System, int32 EmitterIndex)
    {
        return StackOf(EmitterAt(System, EmitterIndex));
    }

    TStrongObjectPtr<CParticleSystem> WrapEmitter(CParticleEmitter* Emitter)
    {
        // Strong from the start, since nothing else holds the wrapper and a compile job dropping its ref would free it.
        TStrongObjectPtr<CParticleSystem> System = NewObject<CParticleSystem>();
        System->Emitters.Add(Emitter);
        return System;
    }

    CParticleEmitter* CopyEmitter(CParticleEmitter* Source, CPackage* Dest, FName EmitterName)
    {
        if (Source == nullptr || Dest == nullptr)
        {
            return nullptr;
        }

        TVector<CObject*> Sources = { Source };
        CParticleEmitterStack* Stack = StackOf(Source);
        if (Stack != nullptr)
        {
            Sources.Add(Stack);
            for (EParticleModuleStage Stage : GetStages())
            {
                for (const TStrongObjectPtr<CParticleModule>& Module : Stack->GetStack(Stage))
                {
                    Sources.Add(Module.Get());
                    Module->GetOwnedObjects(Sources);
                }
            }
        }

        // Dest may already hold another emitter's stack under the source's name, so the copy takes a free one.
        const FName StackName = UniqueStackName(Dest);
        const THashMap<CObject*, CObject*> Remap = CopyObjectsInto(Sources, Dest, [&](CObject* Object)
        {
            return Object == Source ? EmitterName : Object == Stack ? StackName : NAME_None;
        });
        if (Remap.IsEmpty())
        {
            return nullptr;
        }

        CParticleEmitter* Copy = Cast<CParticleEmitter>(Remap.FindChecked(Source));
        Copy->AuthoringStackName = Stack != nullptr ? FString(*StackName) : FString();
        return Copy;
    }

    CParticleEmitter* SaveEmitterAsAsset(CParticleSystem* System, int32 EmitterIndex, FStringView Path)
    {
        CParticleEmitter* Source = EmitterAt(System, EmitterIndex);
        CPackage* Package = Source != nullptr ? CPackage::CreatePackage(Path) : nullptr;

        // Held strongly, since a fresh object dies the first time a ref the save takes is released.
        TStrongObjectPtr<CParticleEmitter> Asset(CopyEmitter(Source, Package, FName(VFS::FileName(Path, true))));
        if (!Asset || !CPackage::SavePackage(Package, Path))
        {
            Asset.Reset();
            CFactory::DiscardNewPackage(Package);
            return nullptr;
        }

        FAssetRegistry::Get().AssetCreated(Asset.Get());
        System->Emitters[EmitterIndex] = Asset;
        return Asset.Get();
    }

    CParticleEmitter* MakeEmitterLocal(CParticleSystem* System, int32 EmitterIndex)
    {
        CParticleEmitter* Local = CopyEmitter(EmitterAt(System, EmitterIndex), System != nullptr ? System->GetPackage() : nullptr);
        if (Local != nullptr)
        {
            System->Emitters[EmitterIndex] = Local;
        }
        return Local;
    }

    TVector<CClass*> GetModuleTypes()
    {
        TVector<CClass*> Types;
        CClass* BaseClass = CParticleModule::StaticClass();

        ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class == BaseClass || !Class->IsChildOf(BaseClass))
            {
                return;
            }

            // The CDO carries every label and is the only way to ask whether the class wants listing.
            CParticleModule* CDO = Class->GetDefaultObject<CParticleModule>();
            if (CDO != nullptr && CDO->IsPaletteVisible())
            {
                Types.Add(Class);
            }
        });

        return Types;
    }

    CClass* ResolveModuleType(FStringView Name)
    {
        for (CClass* Class : GetModuleTypes())
        {
            const FString ClassName(*Class->GetName().ToString());
            const FString DisplayName = Class->GetDefaultObject<CParticleModule>()->GetDisplayName();
            const FStringView ShortName = FStringView(ClassName).StartsWith(ModuleClassPrefix)
                ? FStringView(ClassName).Mid(ModuleClassPrefix.Len())
                : FStringView(ClassName);
            if (EqualsIgnoreCase(FStringView(ClassName), Name) || EqualsIgnoreCase(ShortName, Name) || EqualsIgnoreCase(FStringView(DisplayName), Name))
            {
                return Class;
            }
        }

        return nullptr;
    }

    TVector<TStrongObjectPtr<CParticleModule>>* GetModules(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage)
    {
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        return Stack != nullptr ? &Stack->GetStack(Stage) : nullptr;
    }

    CParticleModule* AddModule(CParticleSystem* System, int32 EmitterIndex, CClass* ModuleClass, int32 InsertIndex)
    {
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        if (Stack == nullptr)
        {
            return nullptr;
        }

        CParticleModule* Module = Stack->AddModule(ModuleClass);
        if (Module == nullptr)
        {
            return nullptr;
        }

        TVector<TStrongObjectPtr<CParticleModule>>& Modules = Stack->GetStack(Module->GetStage());
        const int32 Last = Modules.Num() - 1;
        if (InsertIndex >= 0 && InsertIndex < Last)
        {
            TStrongObjectPtr<CParticleModule> Added = Modules[Last];
            Modules.RemoveAt(Last);
            Modules.Insert(Added, InsertIndex);
        }

        return Module;
    }

    bool RemoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 Index)
    {
        TVector<TStrongObjectPtr<CParticleModule>>* Modules = GetModules(System, EmitterIndex, Stage);
        if (Modules == nullptr || Index < 0 || Index >= Modules->Num())
        {
            return false;
        }

        Modules->RemoveAt(Index);
        return true;
    }

    bool MoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 From, int32 To)
    {
        TVector<TStrongObjectPtr<CParticleModule>>* Modules = GetModules(System, EmitterIndex, Stage);
        if (Modules == nullptr || From < 0 || From >= Modules->Num() || To < 0 || To >= Modules->Num())
        {
            return false;
        }

        TStrongObjectPtr<CParticleModule> Moved = (*Modules)[From];
        Modules->RemoveAt(From);
        Modules->Insert(Moved, To);
        return true;
    }

    bool CompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileOutput& Out)
    {
        FParticleEmitterCompileJob Job;
        if (!BeginCompileEmitter(System, EmitterIndex, Job, Out))
        {
            return false;
        }
        Job.Waiter.Wait();
        return FinishCompileEmitter(Job, Out);
    }

    bool BeginCompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileJob& Job,
                             FParticleEmitterCompileOutput& Out, Jobs::EJobPriority Priority)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        if (Emitter == nullptr || Stack == nullptr)
        {
            Out.Errors.Add("That emitter does not exist.");
            return false;
        }
        if (Global::GShaderCompiler == nullptr)
        {
            Out.Errors.Add("No shader compiler is running in this process.");
            return false;
        }

        Job.System       = System;
        Job.EmitterIndex = EmitterIndex;
        Job.Compiler     = MakeUnique<FParticleCompiler>();
        Job.Binaries     = MakeShared<TVector<uint32>>();

        FParticleCompiler& Compiler = *Job.Compiler;
        Stack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
            {
                Out.Errors.Add(Error.Name + ": " + Error.Description);
            }
            return false;
        }

        const FString Source = Compiler.BuildShader();
        if (Source.IsEmpty())
        {
            Out.Errors.Add("The module stack produced no shader source.");
            return false;
        }

        FShaderCompileOptions Options;
        Options.DebugName = FString(*System->GetName()) + "_" + Emitter->EmitterName + "_Sim";
        // Without a template path the raw cache keys to zero and skips itself, so every save paid a full Slang compile.
        Options.TemplateVirtualPath = "/Engine/Resources/Shaders/Particles/ParticleSimulateTemplate.slang";
        Options.Priority = Priority;
        Global::GShaderCompiler->CompilerShaderRaw(Source, Options, [Binaries = Job.Binaries](const FShaderHeader& Header)
        {
            Binaries->Reset();
            Binaries->Append(Header.Binaries.begin(), Header.Binaries.end());
        }, Job.Waiter.Track());
        return true;
    }

    bool FinishCompileEmitter(FParticleEmitterCompileJob& Job, FParticleEmitterCompileOutput& Out)
    {
        CParticleSystem* System = Job.System.Get();
        CParticleEmitter* Emitter = EmitterAt(System, Job.EmitterIndex);
        CParticleEmitterStack* Stack = FindStack(System, Job.EmitterIndex);
        if (Emitter == nullptr || Stack == nullptr || Job.Compiler == nullptr)
        {
            Out.Errors.Add("The emitter went away while its shader compiled.");
            return false;
        }

        if (Job.Binaries == nullptr || Job.Binaries->IsEmpty())
        {
            Out.Errors.Add("The generated shader failed to compile. The editor log has the Slang diagnostics.");
            return false;
        }

        const FParticleCompiler& Compiler = *Job.Compiler;
        Emitter->ComputeShaderBinaries = Move(*Job.Binaries);
        Emitter->CompiledTemplateHash  = CParticleEmitter::GetSimulationTemplateHash();

        // RefreshModuleParams checks against this before doing a value-only update.
        Emitter->ModuleParamValues   = Compiler.GetParamValues();
        Emitter->ParamBindings       = Compiler.GetParamBindings();
        Emitter->CompiledCodeHash    = Compiler.GetGeneratedCodeHash();
        // Structural, so it only moves with a rebuild, which is when the renderer resizes its buffer.
        Emitter->AttributeFloatCount = Compiler.GetAttributeFloatCount();
        Emitter->GridChannelCount    = Compiler.GetGridChannelCount();
        Emitter->GridStageIterations = Compiler.GetGridStageIterations();
        Emitter->GridChannelNames.Reset();
        for (const FString& Channel : Compiler.GetGridChannelNames())
        {
            Emitter->GridChannelNames.Emplace(*Channel);
        }
        Emitter->bUsesNeighborGrid = Compiler.UsesNeighborGrid();
        Emitter->ArrayNames.Reset();
        for (const FString& Array : Compiler.GetArrayNames())
        {
            Emitter->ArrayNames.Emplace(*Array);
        }

        if (Emitter->GridChannelCount > 0)
        {
            FString Channels;
            for (const FString& Channel : Compiler.GetGridChannelNames())
            {
                Channels += (Channels.IsEmpty() ? "" : ", ") + Channel;
            }
            Out.Notes.Add(*Format("Grid of {} by {} cells with {} stages and channels {}.", Emitter->GridWidth, Emitter->GridHeight,
                Emitter->GridStageIterations.Num(), Channels));
        }

        // The shared vertex shader cannot know the generated layout, so it receives these as uniforms.
        Emitter->RenderAttributeSlots.SetNum(static_cast<int32>(ParticleRenderAttribute::Count));
        for (int32 Attribute = 0; Attribute < (int32)ParticleRenderAttribute::Count; ++Attribute)
        {
            Emitter->RenderAttributeSlots[Attribute] = Compiler.FindAttributeSlot(ParticleRenderAttribute::Names[Attribute]);
        }

        // Nothing on screen says whether the trail wiring resolved, so the output has to say it.
        if (Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX] >= 0)
        {
            Out.Notes.Add(*Format("Trail active, previous position in attribute slots {}/{}/{} of {} floats per particle.",
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX],
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosY],
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosZ],
                Emitter->AttributeFloatCount));
        }

        // Route the renderer to the generated module-stack shader.
        Emitter->ShaderMode = EParticleShaderMode::Custom;
        SyncLifetimeRange(System, Emitter, Stack);
        Emitter->StoreShaderInArtifactCache();
        Emitter->PostLoad();
        Out.bSucceeded = true;
        return true;
    }

    bool RefreshModuleParams(CParticleSystem* System, int32 EmitterIndex)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        if (Emitter == nullptr || Stack == nullptr || !Emitter->HasCustomComputeShader())
        {
            return false;
        }

        // The emitted HLSL is discarded, so there is no pipeline swap and no preview restart.
        FParticleCompiler Compiler;
        Stack->CompileStacks(Compiler);
        if (Compiler.HasErrors())
        {
            return false;
        }

        // Layout drift means the running shader indexes slots this value set no longer describes.
        if (Compiler.GetGeneratedCodeHash() != Emitter->CompiledCodeHash)
        {
            return false;
        }

        // Binding an input leaves the code identical, so this is the only path that ever writes them.
        Emitter->ModuleParamValues   = Compiler.GetParamValues();
        Emitter->ParamBindings       = Compiler.GetParamBindings();
        // Iterations only change how often the renderer dispatches a stage, never the code.
        Emitter->GridStageIterations = Compiler.GetGridStageIterations();
        SyncLifetimeRange(System, Emitter, Stack);
        return true;
    }

    static constexpr EParticleModuleStage AllStages[] = { EParticleModuleStage::Spawn, EParticleModuleStage::Update, EParticleModuleStage::Grid, EParticleModuleStage::Target };

    FString StageName(EParticleModuleStage Stage)
    {
        switch (Stage)
        {
        case EParticleModuleStage::Spawn:  return "Spawn";
        case EParticleModuleStage::Grid:   return "Grid";
        case EParticleModuleStage::Target: return "Target";
        default:                           return "Update";
        }
    }

    bool ParseStage(FStringView Name, EParticleModuleStage& OutStage)
    {
        for (EParticleModuleStage Stage : AllStages)
        {
            if (StageName(Stage).EqualsIgnoreCase(Name))
            {
                OutStage = Stage;
                return true;
            }
        }
        return false;
    }

    TSpan<const EParticleModuleStage> GetStages()
    {
        return TSpan<const EParticleModuleStage>(AllStages);
    }
}
