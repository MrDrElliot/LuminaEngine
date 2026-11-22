#pragma once

#include <concepts>
#include <initializer_list>
#include "Containers/HashTable.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CObject;
    class CClass;
    class FEditorTool;
    class IEditorToolContext;
}

namespace Lumina
{
    // Tools are built with Memory::New, so the deleter has to be the one that frees that way.
    using FEditorToolPtr = TUniquePtr<FEditorTool, smart_ptr_deleter<FEditorTool>>;

    // Constrains the registration templates so a bad type is reported at the call site.

    template<typename T>
    concept TEditorToolType = std::derived_from<T, FEditorTool>;

    template<typename T>
    concept TEditorAssetType = std::derived_from<T, CObject> && requires
    {
        { T::StaticClass() } -> std::convertible_to<CClass*>;
    };

    template<typename T>
    concept TAssetEditorToolType = TEditorToolType<T> && std::constructible_from<T, IEditorToolContext*, CObject*>;

    template<typename T>
    concept TFileEditorToolType = TEditorToolType<T> && std::constructible_from<T, IEditorToolContext*, FStringView>;

    // Returned tool is freshly constructed but not yet initialized; the caller owns it.
    using FAssetEditorFactory = TFunction<FEditorToolPtr(IEditorToolContext*, CObject*)>;
    using FFileEditorFactory  = TFunction<FEditorToolPtr(IEditorToolContext*, FStringView)>;

    // Maps asset classes and file extensions to editor tools. A module must UnregisterAll(Owner) in ShutdownModule, or a stale callable crashes the next lookup.
    class EDITOR_API FEditorToolRegistry
    {
    public:

        // Owner the editor's own built-ins register under.
        static FName BuiltInOwner();

        static FEditorToolRegistry& Get();

        // A later registration replaces the earlier one, and the displaced editor is not restored on unregister.
        void RegisterAssetEditor(CClass* AssetClass, FAssetEditorFactory Factory, FName Owner);

        template<TEditorAssetType TAsset, TAssetEditorToolType TTool>
        void RegisterAssetEditor(FName Owner)
        {
            RegisterAssetEditor(
                TAsset::StaticClass(),
                [](IEditorToolContext* Context, CObject* Asset) -> FEditorToolPtr
                {
                    return FEditorToolPtr(Memory::New<TTool>(Context, Asset));
                },
                Owner);
        }

        // Extension is the leading-dot form such as ".rml", matched case-insensitively.
        void RegisterFileEditor(FStringView Extension, FFileEditorFactory Factory, FName Owner);

        template<TFileEditorToolType TTool>
        void RegisterFileEditor(std::initializer_list<FStringView> Extensions, FName Owner)
        {
            FFileEditorFactory Factory = [](IEditorToolContext* Context, FStringView Path) -> FEditorToolPtr
            {
                return FEditorToolPtr(Memory::New<TTool>(Context, Path));
            };

            for (FStringView Extension : Extensions)
            {
                RegisterFileEditor(Extension, Factory, Owner);
            }
        }

        // Return false when there was nothing registered, which a caller tearing down can ignore.
        bool UnregisterAssetEditor(CClass* AssetClass);

        template<TEditorAssetType TAsset>
        bool UnregisterAssetEditor()
        {
            return UnregisterAssetEditor(TAsset::StaticClass());
        }

        bool UnregisterFileEditor(FStringView Extension);

        void UnregisterFileEditors(std::initializer_list<FStringView> Extensions);

        // Drops everything registered under this owner and returns the count. A default-constructed owner is refused.
        int32 UnregisterAll(FName Owner);

        // The most-derived registered class wins, and an unregistered one falls back to the generic property grid.
        FEditorToolPtr CreateAssetEditor(IEditorToolContext* Context, CObject* Asset) const;

        FEditorToolPtr CreateFileEditor(IEditorToolContext* Context, FStringView VirtualPath) const;

        // Exact class only, ignoring base classes.
        bool HasAssetEditor(CClass* AssetClass) const;

        bool HasFileEditor(FStringView Extension) const;

        // For diagnosing which plugin claimed a type. None when nothing is registered.
        FName GetAssetEditorOwner(CClass* AssetClass) const;

    private:

        // Must stay a copyable aggregate since the hash map instantiates the copy constructor regardless of use.
        template<typename TFactory>
        struct TRegistration
        {
            TFactory Factory;
            FName    Owner;
        };

        using FAssetRegistration = TRegistration<FAssetEditorFactory>;
        using FFileRegistration  = TRegistration<FFileEditorFactory>;

        THashMap<CClass*, FAssetRegistration> AssetEditors;
        THashMap<FString, FFileRegistration>  FileEditors;
    };
}
