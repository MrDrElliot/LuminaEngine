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
        virtual void OnStartup() {}

        // Runs every frame on each stage this system required, possibly on a worker thread.
        FUNCTION()
        virtual void OnUpdate() {}

        // Runs once when the world tears down, or when this system is disabled.
        FUNCTION()
        virtual void OnTeardown() {}

        // The owning world, valid from Configure onwards.
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        // The tick context for the stage currently running. Only meaningful inside the callbacks above.
        const FSystemContext& GetContext() const;

        //~ Begin Configure declarations.

        // Schedules this system in Stage. Lower priority runs first, so Highest is 0 and Low is 192.
        FUNCTION()
        void RequireUpdate(EUpdateStage Stage, int32 Priority = (int32)EUpdatePriority::Default);

        void RequireUpdate(EUpdateStage Stage, EUpdatePriority Priority) { RequireUpdate(Stage, (int32)Priority); }

        // Named rather than typed, so a script can declare access to a component it only knows by name.
        FUNCTION()
        void DeclareWrite(FName Component);

        FUNCTION()
        void DeclareRead(FName Component);

        template<typename... Ts>
        void Writes() { Access.Write<Ts...>(); }

        template<typename... Ts>
        void Reads() { Access.Read<Ts...>(); }

        //~ End Configure declarations.

        const FUpdatePriorityList& GetPriorities() const { return Priorities; }
        const FSystemAccess& GetAccess() const { return Access; }

        // Set by the driver before Configure runs.
        void SetOwningWorld(CWorld* InWorld) { OwningWorld = InWorld; }

        // Runs Configure and settles what it declared into the final access set.
        void ConfigureSystem();

        bool HasStarted() const { return bStarted; }
        void MarkStarted() { bStarted = true; }

    private:

        CWorld*             OwningWorld = nullptr;
        FUpdatePriorityList Priorities;
        FSystemAccess       Access;
        bool                bStarted = false;
    };

    // Language agnostic like the world subsystem driver, so a C# system costs nothing beyond its class.
    namespace EntitySystems
    {
        // Creates and configures any system class not already instanced and not named in Disabled.
        RUNTIME_API int32 CreateMissing(CWorld& World, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Out);

        // Tears down the systems backed by a script class, so a hot reload can rebuild them.
        RUNTIME_API void DropScripted(TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Tears down the systems whose HostOnly, ClientOnly or Cosmetic no longer fits the world, after its net mode changed.
        RUNTIME_API void DropOutsideRealm(CWorld& World, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Tears down the systems the editor just disabled.
        RUNTIME_API void DropDisabled(const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Runs OnStartup on every system that has not had it yet.
        RUNTIME_API void StartupPending(TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // Every system class in the process, for the editor's system list. Includes disabled ones.
        RUNTIME_API void ForEachSystemClass(TFunctionRef<void(CClass*)> Visitor);

        // Runs OnTeardown on each and empties the list.
        RUNTIME_API void DestroyAll(TVector<TStrongObjectPtr<CEntitySystem>>& Systems);

        // The first system whose class derives from Class, or null.
        RUNTIME_API CEntitySystem* Find(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class);
    }
}
