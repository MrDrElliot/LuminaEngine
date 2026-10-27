#pragma once

#include "World/ECS/Registry.h"

#include "SystemContext.h"
#include "SystemAccess.h"
#include "Containers/FunctionRef.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/UpdateStage.h"
#include "EntitySystem.generated.h"

namespace Lumina
{
    class CSaveGame;
    class CWorld;

    // One stage-scheduled worker over a world's component store, instanced once per world.
    REFLECT(Scriptable)
    class RUNTIME_API CEntitySystem : public CObject
    {
        GENERATED_BODY()

    public:

        // Answered on the default object before an instance exists, so a system can decline a world.
        FUNCTION()
        virtual bool ShouldCreate() { return true; }

        // Declares this system's stages and component access, before it is scheduled and before any startup.
        FUNCTION()
        virtual void Configure() {}

        // Runs once when the world starts, after every system has been created and configured.
        FUNCTION()
        virtual void OnStartup(const SSystemContext& Context) {}

        // Runs every frame on each stage this system required, possibly on a worker thread.
        FUNCTION()
        virtual void OnUpdate(const SSystemContext& Context) {}

        // Asked on the main thread as each stage starts. False leaves this system out of that frame, so it neither runs nor separates the systems around it.
        FUNCTION()
        virtual bool HasWork(const SSystemContext& Context) { return true; }

        // Runs once when the world tears down, or when this system is disabled.
        FUNCTION()
        virtual void OnTeardown(const SSystemContext& Context) {}

        // The world is about to be captured into SaveGame, so state kept outside SaveGame fields can be folded in.
        FUNCTION()
        virtual void OnWorldSaving(CSaveGame* SaveGame) {}

        // SaveGame just restored the world, so anything this system derived from saved fields is stale.
        FUNCTION()
        virtual void OnWorldRestored(CSaveGame* SaveGame) {}

        // The owning world, valid from Configure onwards.
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        //~ Begin Configure declarations.

        // Schedules this system in Stage, where only the systems it declares to run after or before order it.
        FUNCTION()
        void RequireUpdate(EUpdateStage Stage);

        // Runs after the named system and any system deriving from it, in every stage both tick in. A name reaches a system in a module this one does not link.
        FUNCTION()
        void RunsAfterSystem(FName System);

        FUNCTION()
        void RunsBeforeSystem(FName System);

        template<typename... Ts>
        void RunsAfter() { (RunsAfterSystem(Ts::StaticClass()->GetName()), ...); }

        template<typename... Ts>
        void RunsBefore() { (RunsBeforeSystem(Ts::StaticClass()->GetName()), ...); }

        // Named rather than typed, so a script can declare access to a component it only knows by name.
        FUNCTION()
        void DeclareWrite(FName Component);

        FUNCTION()
        void DeclareRead(FName Component);

        template<typename... Ts>
        void Writes() { Access.Write<Ts...>(); }

        template<typename... Ts>
        void Reads() { Access.Read<Ts...>(); }

        // Declares the read and hands back the reader that OnUpdate drains with Context.ReadEvents.
        template<typename T>
        NODISCARD ECS::TEventReader<T> ReadsEvent()
        {
            Access.Reads.push_back(static_cast<uint32>(ECS::GetComponentTypeID<T>()));
            return {};
        }

        // Declares that OnUpdate sends T with Context.SendEvent, so readers of T run after this system's batch.
        template<typename... Ts>
        void SendsEvent() { (Access.Writes.push_back(static_cast<uint32>(ECS::GetComponentTypeID<Ts>())), ...); }

        //~ End Configure declarations.

        bool RunsInStage(EUpdateStage Stage) const { return (StageMask & (1u << (uint32)Stage)) != 0; }
        const TVector<FName>& GetRunsAfter() const { return After; }
        const TVector<FName>& GetRunsBefore() const { return Before; }
        const FSystemAccess& GetAccess() const { return Access; }

        // Set by the driver before Configure runs.
        void SetOwningWorld(CWorld* InWorld) { OwningWorld = InWorld; }

        // Runs Configure and settles what it declared into the final access set.
        void ConfigureSystem();

        bool HasStarted() const { return bStarted; }
        void MarkStarted() { bStarted = true; }

    private:

        CWorld*             OwningWorld = nullptr;
        TVector<FName>      After;
        TVector<FName>      Before;
        FSystemAccess       Access;
        uint8               StageMask = 0;
        bool                bStarted = false;
    };

    // Language agnostic like the world subsystem driver, so a C# system costs nothing beyond its class.
    namespace EntitySystems
    {
        // Creates and configures any system class not already instanced and not named in Disabled.
        RUNTIME_API int32 CreateMissing(CWorld& World, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Out);

        // Tears down the systems backed by a script class, so a hot reload can rebuild them.
        RUNTIME_API void DropScripted(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Tears down the systems whose HostOnly, ClientOnly or Cosmetic no longer fits the world, after its net mode changed.
        RUNTIME_API void DropOutsideRealm(const SSystemContext& Context, CWorld& World, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Tears down the systems the editor just disabled.
        RUNTIME_API void DropDisabled(const SSystemContext& Context, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Runs OnStartup on every system that has not had it yet.
        RUNTIME_API void StartupPending(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Every system class in the process, for the editor's system list. Includes disabled ones.
        RUNTIME_API void ForEachSystemClass(TFunctionRef<void(CClass*)> Visitor);

        // Runs OnTeardown on each and empties the list.
        RUNTIME_API void DestroyAll(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // The first system whose class derives from Class, or null.
        RUNTIME_API CEntitySystem* Find(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class);
    }
}
