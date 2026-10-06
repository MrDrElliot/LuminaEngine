#pragma once

#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "WorldSubsystem.generated.h"

namespace Lumina
{
    class CWorld;

    // A service owned by one world, created during InitializeWorld and destroyed in TeardownWorld.
    REFLECT(Scriptable)
    class RUNTIME_API CWorldSubsystem : public CObject
    {
        GENERATED_BODY()

    public:

        // Answered on the default object before an instance exists, so a subsystem can decline a world.
        FUNCTION()
        virtual bool ShouldCreate() { return true; }

        // This subsystem's own setup, where a sibling subsystem may not exist yet.
        FUNCTION()
        virtual void OnInitialize() {}

        // Every subsystem in the world now exists, which is where one may reach another.
        FUNCTION()
        virtual void OnWorldReady() {}

        // Per frame, ahead of the update stages. Only a subsystem that overrides it is dispatched.
        FUNCTION()
        virtual void OnUpdate(float DeltaTime) {}

        // Runs before the registry and the systems are torn down, so the world is still readable.
        FUNCTION()
        virtual void OnTeardown() {}

        // The owning world, valid from OnInitialize onwards.
        FUNCTION()
        CWorld* GetWorld() const { return OwningWorld; }

        // Set by the driver before OnInitialize runs.
        void SetOwningWorld(CWorld* InWorld) { OwningWorld = InWorld; }

    private:

        CWorld* OwningWorld = nullptr;
    };

    // Language agnostic like the entity script driver, so a C# subsystem costs nothing beyond its class.
    namespace WorldSubsystems
    {
        // Creates any subsystem class not already instanced, then initializes and readies only the new ones.
        RUNTIME_API int32 CreateMissing(CWorld& World, TVector<TStrongObjectPtr<CWorldSubsystem>>& Out);

        // Tears down the subsystems backed by a script class, so a hot reload can rebuild them.
        RUNTIME_API void DropScripted(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems);

        RUNTIME_API void Update(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, float DeltaTime);

        // Runs OnTeardown on each and empties the list.
        RUNTIME_API void DestroyAll(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems);

        // The first subsystem whose class derives from Class, or null.
        RUNTIME_API CWorldSubsystem* Find(const TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, const CClass* Class);
    }
}
