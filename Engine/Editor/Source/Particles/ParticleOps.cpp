#include "ParticleOps.h"

#include "ParticleEmitterStack.h"
#include "ParticleStockModules.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/ShaderCompiler.h"
#include "UI/Tools/NodeGraph/Particle/ParticleCompiler.h"

#include <cctype>

namespace Lumina::ParticleOps
{
    namespace
    {
        constexpr FStringView ModuleClassPrefix = "CParticleModule_";

        bool EqualsIgnoreCase(FStringView A, FStringView B)
        {
            if (A.size() != B.size())
            {
                return false;
            }

            for (size_t Index = 0; Index < A.size(); ++Index)
            {
                if (std::tolower((unsigned char)A[Index]) != std::tolower((unsigned char)B[Index]))
                {
                    return false;
                }
            }

            return true;
        }

        CParticleEmitter* EmitterAt(CParticleSystem* System, int32 EmitterIndex)
        {
            if (System == nullptr || EmitterIndex < 0 || EmitterIndex >= (int32)System->Emitters.size())
            {
                return nullptr;
            }

            return System->Emitters[EmitterIndex].Get();
        }

        FVector2 LifetimeInputRange(const CParticleSystem* System, const SParticleParam& Input)
        {
            if (!Input.ParameterName.IsNone())
            {
                if (const FParticleParameter* Bound = System->FindUserParameter(Input.ParameterName))
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
            for (const TObjectPtr<CParticleModule>& Module : Stack->SpawnModules)
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
    }

    CParticleEmitterStack* FindOrCreateStack(CParticleSystem* System, int32 EmitterIndex)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        if (Emitter == nullptr || System->GetPackage() == nullptr)
        {
            return nullptr;
        }

        // Persisted on the emitter, so reordering or renaming never re-points a stack.
        if (Emitter->AuthoringStackName.empty())
        {
            Emitter->AuthoringStackName = FString("ParticleStack_") + Format("{}", EmitterIndex).c_str();
        }

        CParticleEmitterStack* Stack = Cast<CParticleEmitterStack>(System->GetPackage()->LoadObjectByName(Emitter->AuthoringStackName));
        if (Stack == nullptr)
        {
            Stack = NewObject<CParticleEmitterStack>(System->GetPackage(), Emitter->AuthoringStackName);
            Stack->EnsureDefaultStack();
        }

        return Stack;
    }

    TVector<CClass*> GetModuleTypes()
    {
        TVector<CClass*> Types;
        CClass* BaseClass = CParticleModule::StaticClass();

        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
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
                Types.push_back(Class);
            }
        });

        return Types;
    }

    CClass* ResolveModuleType(FStringView Name)
    {
        for (CClass* Class : GetModuleTypes())
        {
            const FString ClassName(Class->GetName().ToString().c_str());
            const FString DisplayName = Class->GetDefaultObject<CParticleModule>()->GetDisplayName();
            const FStringView ShortName = FStringView(ClassName).starts_with(ModuleClassPrefix)
                ? FStringView(ClassName).substr(ModuleClassPrefix.size())
                : FStringView(ClassName);
            if (EqualsIgnoreCase(FStringView(ClassName), Name) || EqualsIgnoreCase(ShortName, Name) || EqualsIgnoreCase(FStringView(DisplayName), Name))
            {
                return Class;
            }
        }

        return nullptr;
    }

    TVector<TObjectPtr<CParticleModule>>* GetModules(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage)
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

        TVector<TObjectPtr<CParticleModule>>& Modules = Stack->GetStack(Module->GetStage());
        const int32 Last = (int32)Modules.size() - 1;
        if (InsertIndex >= 0 && InsertIndex < Last)
        {
            TObjectPtr<CParticleModule> Added = Modules[Last];
            Modules.erase(Modules.begin() + Last);
            Modules.insert(Modules.begin() + InsertIndex, Added);
        }

        return Module;
    }

    bool RemoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 Index)
    {
        TVector<TObjectPtr<CParticleModule>>* Modules = GetModules(System, EmitterIndex, Stage);
        if (Modules == nullptr || Index < 0 || Index >= (int32)Modules->size())
        {
            return false;
        }

        Modules->erase(Modules->begin() + Index);
        return true;
    }

    bool MoveModule(CParticleSystem* System, int32 EmitterIndex, EParticleModuleStage Stage, int32 From, int32 To)
    {
        TVector<TObjectPtr<CParticleModule>>* Modules = GetModules(System, EmitterIndex, Stage);
        if (Modules == nullptr || From < 0 || From >= (int32)Modules->size() || To < 0 || To >= (int32)Modules->size())
        {
            return false;
        }

        TObjectPtr<CParticleModule> Moved = (*Modules)[From];
        Modules->erase(Modules->begin() + From);
        Modules->insert(Modules->begin() + To, Moved);
        return true;
    }

    bool CompileEmitter(CParticleSystem* System, int32 EmitterIndex, FParticleEmitterCompileOutput& Out)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        if (Emitter == nullptr || Stack == nullptr)
        {
            Out.Errors.push_back("That emitter does not exist.");
            return false;
        }

        FParticleCompiler Compiler;
        Stack->CompileStacks(Compiler);

        if (Compiler.HasErrors())
        {
            for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
            {
                Out.Errors.push_back(Error.Name + ": " + Error.Description);
            }
            return false;
        }

        const FString Source = Compiler.BuildShader();
        if (Source.empty())
        {
            Out.Errors.push_back("The module stack produced no shader source.");
            return false;
        }

        bool bCompiled = false;
        IShaderCompiler* ShaderCompiler = GShaderCompiler;
        ShaderCompiler->CompilerShaderRaw(Source, {}, [Emitter, &bCompiled](const FShaderHeader& Header) mutable
        {
            Emitter->ComputeShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
            bCompiled = !Header.Binaries.empty();
        });
        ShaderCompiler->Flush();

        if (!bCompiled)
        {
            Out.Errors.push_back("The generated shader failed to compile. The editor log has the Slang diagnostics.");
            return false;
        }

        // RefreshModuleParams checks against this before doing a value-only update.
        Emitter->ModuleParamValues   = Compiler.GetParamValues();
        Emitter->ParamBindings       = Compiler.GetParamBindings();
        Emitter->CompiledCodeHash    = Compiler.GetGeneratedCodeHash();
        // Structural, so it only moves with a rebuild, which is when the renderer resizes its buffer.
        Emitter->AttributeFloatCount = Compiler.GetAttributeFloatCount();

        // The shared vertex shader cannot know the generated layout, so it receives these as uniforms.
        Emitter->RenderAttributeSlots.resize((size_t)ParticleRenderAttribute::Count);
        for (int32 Attribute = 0; Attribute < (int32)ParticleRenderAttribute::Count; ++Attribute)
        {
            Emitter->RenderAttributeSlots[Attribute] = Compiler.FindAttributeSlot(ParticleRenderAttribute::Names[Attribute]);
        }

        // Nothing on screen says whether the trail wiring resolved, so the output has to say it.
        if (Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX] >= 0)
        {
            Out.Notes.push_back(Format("Trail active, previous position in attribute slots {}/{}/{} of {} floats per particle.",
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosX],
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosY],
                Emitter->RenderAttributeSlots[ParticleRenderAttribute::PrevPosZ],
                Emitter->AttributeFloatCount).c_str());
        }

        // Route the renderer to the generated module-stack shader.
        Emitter->ShaderMode = EParticleShaderMode::Custom;
        SyncLifetimeRange(System, Emitter, Stack);
        Emitter->PostLoad();
        Out.bSucceeded = true;
        return true;
    }

    bool RefreshModuleParams(CParticleSystem* System, int32 EmitterIndex)
    {
        CParticleEmitter* Emitter = EmitterAt(System, EmitterIndex);
        CParticleEmitterStack* Stack = FindOrCreateStack(System, EmitterIndex);
        if (Emitter == nullptr || Stack == nullptr || Emitter->ComputeShaderBinaries.empty())
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
        Emitter->ModuleParamValues = Compiler.GetParamValues();
        Emitter->ParamBindings     = Compiler.GetParamBindings();
        SyncLifetimeRange(System, Emitter, Stack);
        return true;
    }

    FString StageName(EParticleModuleStage Stage)
    {
        return Stage == EParticleModuleStage::Spawn ? "Spawn" : "Update";
    }

    bool ParseStage(FStringView Name, EParticleModuleStage& OutStage)
    {
        if (EqualsIgnoreCase(Name, "Spawn"))
        {
            OutStage = EParticleModuleStage::Spawn;
            return true;
        }

        if (EqualsIgnoreCase(Name, "Update"))
        {
            OutStage = EParticleModuleStage::Update;
            return true;
        }

        return false;
    }
}
