#pragma once

#include <Containers/Name.h>
#include <Containers/String.h>
#include <Core/LuminaMacros.h>
#include <Core/Assertions/Assert.h>
#include <Platform/GenericPlatform.h>
#include "ObjectFlags.h"
#include "GUID/GUID.h"
#include "ConstructObjectParams.h"

namespace Lumina
{
    namespace Object::Internal
    {
        class FCObjectArray;
    }

    // Publishes what is being constructed, so an object's own constructor can read its class, name and package.
    class RUNTIME_API FScopedObjectConstruction
    {
    public:

        explicit FScopedObjectConstruction(const FConstructCObjectParams& Params);
        ~FScopedObjectConstruction();

        LE_NO_COPYMOVE(FScopedObjectConstruction);

        static const FConstructCObjectParams* Current();

    private:

        const FConstructCObjectParams* Previous;
    };
    class CObjectBase;
    class CClass;
    class CEnum;
    class CStruct;
    
    class CObjectBase
    {
    public:
        friend class Object::Internal::FCObjectArray;
        friend class CPackage;
        friend class FManagedInstanceTable;

        RUNTIME_API CObjectBase();
        RUNTIME_API virtual ~CObjectBase();

        CObjectBase(const CObjectBase&) = delete;
        CObjectBase(CObjectBase&&) = delete;

        CObjectBase& operator=(const CObjectBase&) = delete;
        CObjectBase& operator=(CObjectBase&&) = delete;
        
        RUNTIME_API CObjectBase(EObjectFlags InFlags);
        RUNTIME_API CObjectBase(CClass* InClass, EObjectFlags InFlags, CPackage* Package, FName InName, const FGuid& GUID);
        
        RUNTIME_API void BeginRegister();
        RUNTIME_API void FinishRegister(CClass* InClass, const TCHAR* InName);

        RUNTIME_API EObjectFlags GetFlags() const { return ObjectFlags; }

        // The managed wrapper reads this flag in place rather than crossing back to ask whether an object died.
        static size_t GetFlagsOffsetForInterop() { return offsetof(CObjectBase, ObjectFlags); }

        RUNTIME_API void ClearFlags(EObjectFlags Flags) const { EnumRemoveFlags(ObjectFlags, Flags); }
        RUNTIME_API void SetFlag(EObjectFlags Flags) const { EnumAddFlags(ObjectFlags, Flags); }
        RUNTIME_API bool HasAnyFlag(EObjectFlags Flag) const { return EnumHasAnyFlags(ObjectFlags, Flag); }
        RUNTIME_API bool HasAllFlags(EObjectFlags Flags) const { return EnumHasAllFlags(ObjectFlags, Flags); }

        RUNTIME_API void ForceDestroyNow();
        RUNTIME_API void ConditionalBeginDestroy();
        RUNTIME_API int32 GetStrongRefCount() const;

        // Rewires hash buckets without any safety checks, which the caller must guarantee.
        RUNTIME_API void HandleNameChange(const FName& NewName, CPackage* NewPackage = nullptr) noexcept;

        // Takes this object out of its package, which a null package in HandleNameChange or Rename cannot do.
        RUNTIME_API void ClearPackage() noexcept;

        // Called just before the destructor and free.
        RUNTIME_API virtual void OnDestroy() { }

        RUNTIME_API int32 GetInternalIndex() const { return InternalIndex; }

        // Re-keys this object's GUID in the object hash, so another object can take the one it had.
        RUNTIME_API void HandleGUIDChange(const FGuid& NewGUID) noexcept;

        RUNTIME_API void AddToRoot();
        RUNTIME_API void RemoveFromRoot();

        // Strips common CObject prefixes.
        RUNTIME_API virtual FFixedString MakeDisplayName() const;

        // Claims an object array slot and enters the hash tables, once the constructor has run.
        RUNTIME_API void AddObject();

    private:


        RUNTIME_API void RunOnDestroyOnce();

        RUNTIME_API void DestroyInternal();
        
        RUNTIME_API void BeginDestroyForShutdown();
        RUNTIME_API void FinishDestroyForShutdown();

    public:

        // Forces base classes to register first.
        RUNTIME_API virtual void RegisterDependencies() { }

        CClass* GetClass() const
        {
            return ClassPrivate;
        }

        FName GetName() const
        {
            return NamePrivate;
        }

        const FGuid& GetGUID() const
        {
            return GUIDPrivate;
        }

        CPackage* GetPackage() const
        {
            return PackagePrivate;
        }

        RUNTIME_API int64 GetLoaderIndex() const
        {
            return LoaderIndex;
        }
    
    private:
        
        template<typename TClassType>
        static bool IsAHelper(const TClassType* Class, const TClassType* TestClass)
        {
            return Class->IsChildOf(TestClass);
        }
        
    public:

        // Templated to break the cyclic dep between CObjectBase and CClass.
        template<typename OtherClassType>
        bool IsA(OtherClassType Base) const
        {
            const CClass* SomeBase = Base;
            (void)SomeBase;

            const CClass* ThisClass = GetClass();

            ASSUME(SomeBase);
            ASSUME(ThisClass);

            return IsAHelper(ThisClass, SomeBase);
        }
        
        template<typename T>
        bool IsA() const
        {
            return IsA(T::StaticClass());
        }

    private:
        
        mutable EObjectFlags    ObjectFlags = OF_None;
        int32                   ManagedInstanceSlot = -1;
        CClass*                 ClassPrivate = nullptr;
        CPackage*               PackagePrivate = nullptr;
        FName                   NamePrivate;
        FGuid                   GUIDPrivate;
        int32                   InternalIndex = -1;
        
        // Index into this object's package export map.
        int32                   LoaderIndex = 0;
    };


    using FEnumConstructor   = CEnum* (*)();
    using FStructConstructor = CStruct* (*)();
    using FClassConstructor  = CClass* (*)();

    // Every reflected type one module compiled in, constructed together once the module is registered.
    struct FCompiledInTypes
    {
        const FEnumConstructor*   Enums = nullptr;
        uint32                    NumEnums = 0;
        const FStructConstructor* Structs = nullptr;
        uint32                    NumStructs = 0;
        const FClassConstructor*  Classes = nullptr;
        uint32                    NumClasses = 0;
        const char*               Module = nullptr;
        // The reflector's hash of every size and offset it laid out, which the module's C# bindings carry too.
        uint64                    LayoutHash = 0;
    };

    // Queues a module's types from its static initializer, so they are built once the reflection system is up.
    struct FTypeRegistrar
    {
        RUNTIME_API explicit FTypeRegistrar(const FCompiledInTypes& Types);
    };

    // The layout hash the named module registered, or zero when no loaded module has that name.
    RUNTIME_API uint64 FindModuleLayoutHash(FStringView Module);

    RUNTIME_API void InitializeCObjectSystem();
    RUNTIME_API void ShutdownCObjectSystem();

    RUNTIME_API void FinishRegistration(CObjectBase* Object);

    // Builds every queued type and its default object, bases before the classes that derive from them.
    RUNTIME_API void RegisterPendingTypes();

    // Runs RegisterPendingTypes unless it is already running, for a caller that may be inside that pass.
    RUNTIME_API void SettlePendingTypes();

    RUNTIME_API int32 SnapshotPendingTypes();

    // Drops modules queued since the snapshot, whose constructors point into a DLL that is about to be freed.
    RUNTIME_API void RollbackPendingTypes(int32 Snapshot);
}
