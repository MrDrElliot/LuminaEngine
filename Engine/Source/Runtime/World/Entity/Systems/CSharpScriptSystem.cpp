#include "pch.h"
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

        auto View = Context.CreateView<SScriptComponent>(entt::exclude<SDisabledTag>);

        // Binding, input dispatch and OnReady happen ONCE per frame, in the PrePhysics pass. A post-physics
        // script is still created and readied here; only its OnUpdate is deferred to the PostPhysics pass.
        if (bPrePhysics)
        {
            View.each([&](entt::entity Entity, SScriptComponent& Component)
            {
                if (Component.ScriptClass.empty())
                {
                    return;
                }
                if (Component.Generation == Generation && Component.Instance != nullptr)
                {
                    return;
                }

                // Generation changed, managed already freed the old instance handle on unload, so drop our
                // stale pointer WITHOUT calling destroy (that would touch a freed GCHandle).
                const bool bHotReload = Component.Generation >= 0 && Component.Generation != Generation;
                Component.Instance = nullptr;
                Component.BindState = ECSharpBindState::Unbound;
                Component.Generation = Generation;

                // Self-heal a renamed script by rewriting the reference to the current type name.
                FString Resolved = DotNet::ResolveScriptClassName(FStringView(Component.ScriptClass.c_str(), Component.ScriptClass.size()));
                if (!Resolved.empty() && Resolved != Component.ScriptClass)
                {
                    Component.ScriptClass = Resolved;
                }

                const FStringView ClassName(Component.ScriptClass.c_str(), Component.ScriptClass.size());
                void* Instance = DotNet::CreateEntityScript(ClassName, World, (uint32)entt::to_integral(Entity));
                if (Instance == nullptr)
                {
                    Component.CallbackFlags = 0;
                    return;
                }

                Component.Instance = Instance;
                Component.CallbackFlags = DotNet::GetScriptCallbackFlags(Instance);
                Component.BindState = ECSharpBindState::Attached;

                // Bind the value buffer to the script's current layout, then push it onto the fresh instance.
                const CScriptStruct* Layout = DotNet::GetScriptStruct(ClassName);
                Component.Values.EnsureLayout(Layout);
                if (Layout != nullptr && Component.Values.GetBuffer() != nullptr)
                {
                    // On a hot reload, [SkipHotReload] fields take the new default instead of the carried value.
                    if (bHotReload)
                    {
                        Layout->ResetHotReloadFields(Component.Values.GetBuffer());
                    }
                    TVector<Scripting::FScriptPropertyEntry> Values;
                    Scripting::ReadStructToValues(Layout, Component.Values.GetBuffer(), Values);
                    DotNet::ApplyScriptProperties(Instance, Values);
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
                        if (Component.Instance == nullptr || (Component.CallbackFlags & OnInputBit) == 0)
                        {
                            return;
                        }
                        const SInputComponent* Input = Context.GetRegistry().try_get<SInputComponent>(Entity);
                        if (Input == nullptr || !Input->bReceivingInput)
                        {
                            return;
                        }
                        for (const SInputEvent& E : *Events)
                        {
                            const int32 bMouse  = (E.Key.Device == EKeyDevice::Mouse) ? 1 : 0;
                            const int32 KeyCode = bMouse ? (int32)E.Key.MouseButton : (int32)E.Key.Key;
                            const int32 Mods    = (E.Key.bShift ? 1 : 0) | (E.Key.bCtrl ? 2 : 0) | (E.Key.bAlt ? 4 : 0);
                            DotNet::DispatchScriptInput(Component.Instance, (int32)E.Type, KeyCode, bMouse, Mods,
                                E.bRepeat ? 1 : 0, E.MouseX, E.MouseY, E.DeltaX, E.DeltaY, E.Scroll);
                        }
                    });
                }
            }

            View.each([&](entt::entity, SScriptComponent& Component)
            {
                if (Component.Instance != nullptr && Component.BindState == ECSharpBindState::Attached)
                {
                    DotNet::OnReadyScript(Component.Instance);
                    Component.BindState = ECSharpBindState::Ready;
                }
            });

            // --- Fixed update: dispatch OnFixedUpdate at the physics fixed rate, BEFORE OnUpdate and before
            //     physics. A game-thread accumulator matching the physics scene's own (same Hz/cap), so it runs
            //     the same number of steps per frame without a cross-thread query. Runs 0..N times this frame.
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
                            if (Component.Instance != nullptr && Component.BindState == ECSharpBindState::Ready
                                && (Component.CallbackFlags & OnFixedUpdateBit) != 0)
                            {
                                FixedScripts.push_back(Component.Instance);
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
            if (Component.Instance == nullptr || Component.BindState != ECSharpBindState::Ready
                || (Component.CallbackFlags & OnUpdateBit) == 0)
            {
                return;
            }
            const bool bScriptPost   = (Component.CallbackFlags & PostPhysicsPhaseBit) != 0;
            const bool bRunThisStage = bScriptPost ? !bPrePhysics : bPrePhysics;
            if (bRunThisStage)
            {
                Ready.push_back(Component.Instance);
            }
        });

        if (!Ready.empty())
        {
            DotNet::UpdateScripts(Ready.data(), (int32)Ready.size(), DeltaSeconds);
        }
    }
}
