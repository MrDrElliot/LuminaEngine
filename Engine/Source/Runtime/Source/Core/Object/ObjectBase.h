#pragma once

#include <Containers/Name.h>
#include <Containers/String.h>
#include <Core/LuminaMacros.h>
#include <Core/Assertions/Assert.h>
#include <Platform/GenericPlatform.h>
#include "ObjectFlags.h"
#include "GUID/GUID.h"
#include "Initializer/ObjectInitializer.h"

namespace Lumina
{
    struct FMetaDataPairParam;
    class CObjectBase;
    class CClass;
    
    /** Low-level CObject implementation. */
    class CObjectBase
    {
    public:
        friend class FCObjectArray;
        friend class CPackage;
        friend class FManagedInstanceTable;

        RUNTIME_API CObjectBase();
        RUNTIME_API virtual ~CObjectBase();

        CObjectBase(const CObjectBase&) = delete;
        CObjectBase(CObjectBase&&) = delete;

        CObjectBase& operator=(const CObjectBase&) = delete;
        CObjectBase& operator=(CObjectBase&&) = delete;
        
        RUNTIME_API virtual void ConstructInternal(const FObjectInitializer& OI);
        
        RUNTIME_API CObjectBase(EObjectFlags InFlags);
        RUNTIME_API CObjectBase(CClass* InClass, EObjectFlags InFlags, CPackage* Package, FName InName, const FGuid& GUID);
        
        RUNTIME_API void BeginRegister();
        RUNTIME_API void FinishRegister(CClass* InClass, const TCHAR* InName);

        RUNTIME_API EObjectFlags GetFlags() const { return ObjectFlags; }

        RUNTIME_API void ClearFlags(EObjectFlags Flags) const { EnumRemoveFlags(ObjectFlags, Flags); }
        RUNTIME_API void SetFlag(EObjectFlags Flags) const { EnumAddFlags(ObjectFlags, Flags); }
        RUNTIME_API bool HasAnyFlag(EObjectFlags Flag) const { return EnumHasAnyFlags(ObjectFlags, Flag); }
        RUNTIME_API bool HasAllFlags(EObjectFlags Flags) const { return EnumHasAllFlags(ObjectFlags, Flags); }

        RUNTIME_API void ForceDestroyNow();
        RUNTIME_API void ConditionalBeginDestroy();
        RUNTIME_API int32 GetStrongRefCount() const;
        RUNTIME_API int32 GetWeakRefCount() const;

        /** Low-level rename; rewires hash buckets. Caller must guarantee safety. */
        RUNTIME_API void HandleNameChange(const FName& NewName, CPackage* NewPackage = nullptr) noexcept;

        /** Called just before destructor + free. */
        RUNTIME_API virtual void OnDestroy() { }

        RUNTIME_API int32 GetInternalIndex() const { return InternalIndex; }

        /** Roots the object, preventing destruction. */
        RUNTIME_API void AddToRoot();
        RUNTIME_API void RemoveFromRoot();

        /** Strips common CObject prefixes. */
        RUNTIME_API virtual FFixedString MakeDisplayName() const;

    private:

        RUNTIME_API void AddObject();
        
        RUNTIME_API void DestroyInternal();
        
        RUNTIME_API void BeginDestroyForShutdown();
        RUNTIME_API void FinishDestroyForShutdown();

    public:

        /** Force base classes to register first. */
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

        // Initialized here, not left to the allocator zeroing the block: ConstructInternal ADDS the
        // requested flags to whatever this holds, so it has to start from a known value.
        mutable EObjectFlags    ObjectFlags = OF_None;
        // Slot in the ManagedInstances table holding this object's C# wrapper, or INDEX_NONE. Deliberately
        // placed here: it occupies the padding between the 4-byte ObjectFlags and the pointer that follows,
        // so caching a managed instance costs no object growth. Keep it adjacent to ObjectFlags -- the
        // static_assert on sizeof(CObjectBase) in ManagedInstance.cpp guards a reorder that would grow every
        // CObject in the engine.
        int32                   ManagedInstanceSlot = -1;
        CClass*                 ClassPrivate = nullptr;
        CPackage*               PackagePrivate = nullptr;
        FName                   NamePrivate;
        FGuid                   GUIDPrivate;
        int32                   InternalIndex = -1;
        /** Index into this object's package export map. */
        int32                   LoaderIndex = 0;
    };


    /** Static-registration helper for generated reflection code. */
    struct FRegisterCompiledInInfo
    {
        template<typename ... TArgs>
        FRegisterCompiledInInfo(TArgs&& ... Args) noexcept
        {
            RegisterCompiledInInfo(std::forward<TArgs>(Args)...);
        }
    };

    struct FClassRegisterCompiledInInfo
    {
        class CClass* (*RegisterFn)();
        const TCHAR* Package;
        const TCHAR* Name;
        
        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FStructRegisterCompiledInInfo
    {
        class CStruct* (*RegisterFn)();
        const TCHAR* Name;
        
        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FEnumRegisterCompiledInInfo
    {
        class CEnum* (*RegisterFn)();
        const TCHAR* Name;
        
        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };


    RUNTIME_API void InitializeCObjectSystem();
    RUNTIME_API void ShutdownCObjectSystem();

    /** Called from generated-code static ctors. */
    RUNTIME_API void RegisterCompiledInInfo(CClass* (*RegisterFn)(), const TCHAR* Package, const TCHAR* Name);

    RUNTIME_API void RegisterCompiledInInfo(CEnum* (*RegisterFn)(), const FEnumRegisterCompiledInInfo& Info);

    RUNTIME_API void RegisterCompiledInInfo(CStruct* (*RegisterFn)(), const FStructRegisterCompiledInInfo& Info);
    
    RUNTIME_API void RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* Info, size_t NumClassInfo);

    RUNTIME_API void RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo, const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo);

    RUNTIME_API void RegisterCompiledInInfo(const FEnumRegisterCompiledInInfo* EnumInfo, size_t NumEnumInfo, const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo, const FStructRegisterCompiledInInfo* StructInfo, size_t NumStructInfo);

    RUNTIME_API CEnum* GetStaticEnum(CEnum* (*RegisterFn)(), const TCHAR* Name);
    
    RUNTIME_API void CObjectForceRegistration(CObjectBase* Object);

    
    RUNTIME_API void ProcessNewlyLoadedCObjects();

    /** How many compiled-in registrations are queued, per registry. */
    struct FDeferredRegistrationSnapshot
    {
        size_t NumClasses = 0;
        size_t NumEnums   = 0;
        size_t NumStructs = 0;
    };

    RUNTIME_API FDeferredRegistrationSnapshot SnapshotDeferredRegistrations();

    /** Discards every registration queued since the snapshot.
     *
     *  Loading a DLL runs its static registrars, so by the time a module can be inspected -- and possibly
     *  refused -- its reflected types are already queued, as function pointers INTO that DLL. Unloading it
     *  without this leaves ProcessNewlyLoadedCObjects() calling into unmapped memory. Call before freeing
     *  the handle, and only for a module whose registrations have not been processed yet.
     */
    RUNTIME_API void RollbackDeferredRegistrations(const FDeferredRegistrationSnapshot& Snapshot);
}
