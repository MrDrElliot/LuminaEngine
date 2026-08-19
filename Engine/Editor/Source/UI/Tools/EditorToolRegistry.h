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

    // Constrains the registration templates, so a bad type is reported at the registration
    // rather than as a template error inside the registry.

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

    // Maps asset classes and file extensions to the editor tool that opens them. Built-ins register
    // at startup; plugins register in StartupModule (EditorInit phase).
    //
    // A registration holds a callable whose code lives in the registering DLL, and nothing can
    // detect that the code is gone once that DLL unloads: the pointer still looks valid and the
    // process dies on the next lookup. A module that registers MUST unregister before it unloads.
    // That is why the owner is a required argument and not a handle to keep: UnregisterAll(Owner)
    // is one call in ShutdownModule that cannot fall out of step with what StartupModule did.
    class EDITOR_API FEditorToolRegistry
    {
    public:

        // Owner the editor's own built-ins register under.
        static FName BuiltInOwner();

        static FEditorToolRegistry& Get();

        // A later registration for the same class replaces the earlier one, which is how a plugin
        // substitutes its own editor for a built-in. The displaced one is not remembered, so
        // unregistering leaves the type with no editor rather than restoring what was there.
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

        // Extension is the leading-dot form, ".rml", matched case-insensitively. Replacement
        // behaves as it does for asset editors.
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

        // Drops everything registered under this owner and returns how many went. The call a
        // module's ShutdownModule wants. A default-constructed owner is refused.
        int32 UnregisterAll(FName Owner);

        // Walks the asset's class chain most-derived first, so a concrete registration wins over
        // one for a base type. Null when no class in the chain is registered.
        FEditorToolPtr CreateAssetEditor(IEditorToolContext* Context, CObject* Asset) const;

        FEditorToolPtr CreateFileEditor(IEditorToolContext* Context, FStringView VirtualPath) const;

        // Exact class only, ignoring base classes.
        bool HasAssetEditor(CClass* AssetClass) const;

        bool HasFileEditor(FStringView Extension) const;

        // For diagnosing which plugin claimed a type. None when nothing is registered.
        FName GetAssetEditorOwner(CClass* AssetClass) const;

    private:

        // Copyable aggregate, not anything owning: it is held by value in a hash map, and the table
        // instantiates the value's copy constructor whether or not it is used.
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
