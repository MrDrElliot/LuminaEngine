#include "RuntimePCH.h"
#include "CSharpScriptSystem.h"

#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptExports.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptValueBridge.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Components/CSharpScriptComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/InputComponent.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "Input/InputEvent.h"

namespace Lumina
{
    class CWorld;

    void* BindScriptInstance(uint64 World, uint32 Entity, SScriptComponent& Component, int32 SlotIndex, int32 Generation, bool bHotReload)
    {
        // Read the class by value first: CreateEntityScript runs the script's OnAttach, which may add
        // scripts and reallocate this entity's Scripts vector, so we must not hold a slot reference across it.
        FString ClassNameStr;
        {
            SScriptInstance& Slot = Component.Scripts[SlotIndex];
            FString Resolved = DotNet::ResolveScriptClassName(FStringView(Slot.ScriptClass.c_str(), Slot.ScriptClass.size()));
            if (!Resolved.empty() && Resolved != Slot.ScriptClass)
            {
                Slot.ScriptClass = Resolved;
            }
            ClassNameStr = Slot.ScriptClass;
        }

        const FStringView ClassName(ClassNameStr.c_str(), ClassNameStr.size());
        void* Instance = DotNet::CreateEntityScript(ClassName, World, Entity);

        // Re-acquire the slot by index; the vector may have moved during OnAttach. Guard the rare case
        // where OnAttach removed earlier slots and shifted this one out of range.
        if (SlotIndex >= (int32)Component.Scripts.size())
        {
            return Instance;
        }
        SScriptInstance& Slot = Component.Scripts[SlotIndex];
        if (Instance == nullptr)
        {
            // The script type is gone (deleted, or renamed with no [Alias]) or failed to construct. If the
            // value store still holds a previous layout, rebind it to the now-current (possibly null) one:
            // EnsureLayout migrates the live values to tagged bytes and drops the old layout's strong ref,
            // so the previous generation's minted CScriptStruct tree (instanced-list candidates and all) is
            // torn down instead of stranded on this slot. The bytes are retained, so re-adding the type
            // later restores the values. Guarded on GetLayout() so a persistently missing slot does this
            // once, not every frame.
            if (Slot.Values.GetLayout() != nullptr)
            {
                Slot.Values.EnsureLayout(DotNet::GetScriptStruct(ClassName));
            }
            Slot.CallbackFlags = 0;
            return nullptr;
        }

        Slot.Instance = Instance;
        Slot.Generation = Generation;
        Slot.CallbackFlags = DotNet::GetScriptCallbackFlags(Instance);
        Slot.BindState = ECSharpBindState::Attached;

        // Bind the value buffer to the script's current layout, then push it onto the fresh instance.
        const CScriptStruct* Layout = DotNet::GetScriptStruct(ClassName);
        Slot.Values.EnsureLayout(Layout);
        if (Layout != nullptr && Slot.Values.GetBuffer() != nullptr)
        {
            // On a hot reload, [SkipHotReload] fields take the new default instead of the carried value.
            if (bHotReload)
            {
                Layout->ResetHotReloadFields(Slot.Values.GetBuffer());
            }
            TVector<Scripting::FScriptPropertyEntry> Values;
            Scripting::ReadStructToValues(Layout, Slot.Values.GetBuffer(), Values);
            DotNet::ApplyScriptProperties(Instance, Values);
        }
        return Instance;
    }

    void SCSharpScriptSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        if (!DotNet::IsInitialized())
        {
            return;
        }

        const uint64 World        = reinterpret_cast<uint64>(Context.GetRegistry().ctx().get<CWorld*>());
        const int32  Generation   = DotNet::GetScriptGeneration();
        const float  DeltaSeconds = (float)Context.GetDeltaTime();
        const bool   bPrePhysics  = (Context.GetUpdateStage() == EUpdateStage::PrePhysics);

        // A [UpdatePhase(EScriptPhase.PostPhysics)] script carries this bit in its callback flags (set by the
        // managed TypeLibrary). Bit index MUST match TypeLibrary.ComputeCallbackFlags. No bit => PrePhysics.
        constexpr int32 PostPhysicsPhaseBit = 1 << 16;

        // SScriptDisabledTag is the outliner's per-entity script toggle: the entity keeps ticking, only its
        // scripts stop. Excluding it HERE covers every pass below -- binding, input dispatch, OnReady,
        // OnFixedUpdate and OnUpdate all iterate this one view, so the toggle cannot be honoured by some
        // passes and missed by others.
        //
        // Already-bound instances are left alive rather than destroyed, so re-enabling resumes the script
        // instead of re-running OnAttach from a fresh instance.
        auto View = Context.CreateView<SScriptComponent>(entt::exclude<SDisabledTag, SScriptDisabledTag>);

        // Binding, input dispatch and OnReady happen ONCE per frame, in the PrePhysics pass. A post-physics
        // script is still created and readied here; only its OnUpdate is deferred to the PostPhysics pass.
        if (bPrePhysics)
        {
            View.each([&](entt::entity Entity, SScriptComponent& Component)
            {
                const uint32 EntityId = (uint32)entt::to_integral(Entity);
                // Index-based: BindScriptInstance runs user OnAttach, which may append slots (reallocating).
                for (int32 i = 0; i < (int32)Component.Scripts.size(); ++i)
                {
                    SScriptInstance& Slot = Component.Scripts[i];
                    if (Slot.ScriptClass.empty())
                    {
                        continue;
                    }
                    if (Slot.Generation == Generation && Slot.Instance != nullptr)
                    {
                        continue;
                    }

                    // Generation changed, managed already freed the old instance handle on unload, so drop
                    // our stale pointer WITHOUT calling destroy (that would touch a freed GCHandle).
                    const bool bHotReload = Slot.Generation >= 0 && Slot.Generation != Generation;
                    Slot.Instance = nullptr;
                    Slot.BindState = ECSharpBindState::Unbound;
                    Slot.Generation = Generation;

                    BindScriptInstance(World, EntityId, Component, i, Generation, bHotReload);
                }
            });

            {
                CWorld* CW = Context.GetRegistry().ctx().get<CWorld*>();
                const FInputViewport* V = FInputViewportRegistry::Get().FindViewportForWorld(CW);
                const TVector<SInputEvent>* Events = (V != nullptr) ? &V->GetContext().GetFrameEvents() : nullptr;
                if (Events != nullptr && !Events->empty())
                {
                    constexpr int32 OnInputBit = 1 << 4;
                    View.each([&](entt::entity Entity, SScriptComponent& Component)
                    {
                        const SInputComponent* Input = Context.GetRegistry().try_get<SInputComponent>(Entity);
                        if (Input == nullptr || !Input->bReceivingInput)
                        {
                            return;
                        }

                        // Snapshot the listeners first: OnInput runs user code that may add/remove scripts on
                        // this entity, reallocating Component.Scripts and dangling a held Slot reference.
                        TVector<void*> Listeners;
                        for (SScriptInstance& Slot : Component.Scripts)
                        {
                            if (Slot.Instance != nullptr && (Slot.CallbackFlags & OnInputBit) != 0)
                            {
                                Listeners.push_back(Slot.Instance);
                            }
                        }

                        for (void* Instance : Listeners)
                        {
                            for (const SInputEvent& E : *Events)
                            {
                                const int32 bMouse  = (E.Key.Device == EKeyDevice::Mouse) ? 1 : 0;
                                const int32 KeyCode = bMouse ? (int32)E.Key.MouseButton : (int32)E.Key.Key;
                                const int32 Mods    = (E.Key.bShift ? 1 : 0) | (E.Key.bCtrl ? 2 : 0) | (E.Key.bAlt ? 4 : 0);
                                DotNet::DispatchScriptInput(Instance, (int32)E.Type, KeyCode, bMouse, Mods,
                                    E.bRepeat ? 1 : 0, E.MouseX, E.MouseY, E.DeltaX, E.DeltaY, E.Scroll);
                            }
                        }
                    });
                }
            }

            // OnReady runs user code that may add scripts to this entity (reallocating Component.Scripts), so
            // flip the bind state in the gather pass while the slot is still valid, then dispatch from a
            // snapshot instead of holding a Slot reference across the user callback.
            {
                TVector<void*> ToReady;
                View.each([&](entt::entity, SScriptComponent& Component)
                {
                    for (SScriptInstance& Slot : Component.Scripts)
                    {
                        if (Slot.Instance != nullptr && Slot.BindState == ECSharpBindState::Attached)
                        {
                            Slot.BindState = ECSharpBindState::Ready;
                            ToReady.push_back(Slot.Instance);
                        }
                    }
                });
                for (void* Instance : ToReady)
                {
                    DotNet::OnReadyScript(Instance);
                }
            }

            // Fixed update: dispatch OnFixedUpdate at the physics fixed rate, BEFORE OnUpdate and before
            // physics. A game-thread accumulator matching the physics scene's own (same Hz/cap), so it runs
            // the same number of steps per frame without a cross-thread query. Runs 0..N times this frame.
            {
                CWorld* CW = Context.GetRegistry().ctx().get<CWorld*>();
                const SDefaultWorldSettings& Settings = CW->GetDefaultWorldSettings();
                const float FixedDt  = 1.0f / eastl::max(10.0f, Settings.PhysicsHz);
                const int32 MaxSteps = (int32)Settings.MaxPhysicsSteps;

                auto& Ctx = Context.GetRegistry().ctx();
                FScriptFixedUpdateState* StatePtr = Ctx.find<FScriptFixedUpdateState>();
                FScriptFixedUpdateState& FixedState = StatePtr ? *StatePtr : Ctx.emplace<FScriptFixedUpdateState>();

                // Accumulate + clamp (spiral-of-death guard), mirroring JoltPhysicsScene::Update.
                FixedState.Accumulator = eastl::min(FixedState.Accumulator + DeltaSeconds, (float)MaxSteps * FixedDt);
                const int32 Steps = (FixedState.Accumulator >= FixedDt)
                    ? eastl::min(MaxSteps, (int32)(FixedState.Accumulator / FixedDt))
                    : 0;

                if (Steps > 0)
                {
                    FixedState.Accumulator -= (float)Steps * FixedDt;
                    constexpr int32 OnFixedUpdateBit = 1 << 9; // must match TypeLibrary.ComputeCallbackFlags

                    for (int32 Step = 0; Step < Steps; ++Step)
                    {
                        // Re-gather each step so a script that destroys itself in OnFixedUpdate can't leave a
                        // stale handle in a later step.
                        TVector<void*> FixedScripts;
                        FixedScripts.reserve(View.size_hint());
                        View.each([&](entt::entity, SScriptComponent& Component)
                        {
                            for (SScriptInstance& Slot : Component.Scripts)
                            {
                                if (Slot.Instance != nullptr && Slot.BindState == ECSharpBindState::Ready
                                    && (Slot.CallbackFlags & OnFixedUpdateBit) != 0)
                                {
                                    FixedScripts.push_back(Slot.Instance);
                                }
                            }
                        });
                        if (!FixedScripts.empty())
                        {
                            DotNet::FixedUpdateScripts(FixedScripts.data(), (int32)FixedScripts.size(), FixedDt);
                        }
                    }
                }
            }
        }

        // OnUpdate: dispatch only ready scripts that override OnUpdate (bit 10) and whose declared phase matches
        // the current stage. Gating by the flag skips the crossing + managed virtual call for non-overriding scripts.
        constexpr int32 OnUpdateBit = 1 << 10; // must match TypeLibrary.ComputeCallbackFlags
        TVector<void*> Ready;
        Ready.reserve(View.size_hint());
        View.each([&](entt::entity, SScriptComponent& Component)
        {
            for (SScriptInstance& Slot : Component.Scripts)
            {
                if (Slot.Instance == nullptr || Slot.BindState != ECSharpBindState::Ready
                    || (Slot.CallbackFlags & OnUpdateBit) == 0)
                {
                    continue;
                }
                const bool bScriptPost   = (Slot.CallbackFlags & PostPhysicsPhaseBit) != 0;
                const bool bRunThisStage = bScriptPost ? !bPrePhysics : bPrePhysics;
                if (bRunThisStage)
                {
                    Ready.push_back(Slot.Instance);
                }
            }
        });

        if (!Ready.empty())
        {
            DotNet::UpdateScripts(Ready.data(), (int32)Ready.size(), DeltaSeconds);
        }
    }
}
