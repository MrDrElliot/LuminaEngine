#pragma once

#include "Assets/AssetRef.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Scripting/EntityScript.h"
#include "UI/UIDesignModel.h"
#include "UI/UITypes.h"
#include "UIScript.generated.h"

namespace Lumina
{
    class CClass;
    class CObject;
    class CStruct;
    class CWorld;
    class FFunction;
    class FProperty;

    // An entity script that is its own screen UI. It shows Document with its PROPERTY(Bind) values and FUNCTION(Bind) commands as the data model, the C++ twin of LuminaSharp.UIScript.
    REFLECT()
    class RUNTIME_API CUIScript : public CEntityScript
    {
        GENERATED_BODY()

    public:

        // The .rml this script shows. Empty uses the class's REFLECT(UIDocument = ...).
        PROPERTY(Editable, Category = "UI", AssetType = "rml")
        FAssetRef Document;

        // Show the document as soon as it loads. Off keeps it hidden until Show().
        PROPERTY(Editable, Category = "UI")
        bool bShowOnStart = true;

        // Free the cursor and let the document take clicks while it is shown, for menus. Off leaves gameplay input alone, for a HUD.
        PROPERTY(Editable, Category = "UI")
        bool bInteractive = false;

        FUNCTION()
        void Show();

        FUNCTION()
        void Hide();

        FUNCTION()
        void Toggle();

        FUNCTION()
        bool IsShown() const;

        FUNCTION()
        FUIDocument GetDocument() const;

        // Runs after every load, including a reload from an edited .rml, once PROPERTY(Element) members point at the new elements.
        virtual void OnDocumentLoaded() {}
    };

    // What a class exposes to a UI document, shared by the runtime binding and the editor preview.
    namespace UIBinding
    {
        struct FBoundValue
        {
            FString    Name;
            FProperty* Property = nullptr;
        };

        struct FBoundFunction
        {
            FString          Name;
            const FFunction* Function = nullptr;
            bool             bComputed = false;
        };

        RUNTIME_API void GetBoundValues(const CStruct* Type, TVector<FBoundValue>& Out);
        RUNTIME_API void GetBoundFunctions(const CClass* Class, TVector<FBoundFunction>& Out);

        // The data-model name a class registers under, REFLECT(DataModel = ...) or the class name without its C prefix or namespace.
        RUNTIME_API FString ModelName(const CClass* Class);

        // The class registering as Name that binds anything, or null.
        RUNTIME_API CClass* FindModelClass(FStringView Name);

        // Every class that binds anything, by model name, for the editor's model list.
        RUNTIME_API void EnumerateModelClasses(TVector<FUIModelInfo>& Out);

        // Members, defaults and commands of Object as the editor preview shows them.
        RUNTIME_API void Describe(const CObject* Object, FUIDesignModel& Out);

        // The property a bound path such as Player.Name reaches from Object, with OutContainer set to whatever holds it.
        RUNTIME_API const FProperty* ResolvePath(CObject* Object, FStringView Path, void*& OutContainer);
    }

    // Drives every script that shows a UI, C++ CUIScript subclasses and LuminaSharp.UIScript subclasses alike.
    namespace UIScripts
    {
        RUNTIME_API bool IsUIScript(const CClass* Class);

        // Called by the script driver just before OnReady and just after OnDetach.
        RUNTIME_API void Open(CEntityScript* Script);
        RUNTIME_API void Close(CEntityScript* Script);

        // The script is being destroyed, possibly after its world's UI already went.
        RUNTIME_API void Forget(CEntityScript* Script);

        // A reload replaced Old with New, which keeps its UI and binds it again on the next tick.
        RUNTIME_API void Replace(CEntityScript* Old, CEntityScript* New);

        RUNTIME_API void Show(CEntityScript* Script);
        RUNTIME_API void Hide(CEntityScript* Script);
        RUNTIME_API bool IsShown(const CEntityScript* Script);
        RUNTIME_API void* GetDocument(const CEntityScript* Script);

        // Once a frame per world before its UI updates, so a script reload rebinds against the reminted class.
        RUNTIME_API void Tick(CWorld* World);

        // An .rml was saved, so every script showing it loads the new markup.
        RUNTIME_API void DocumentChanged(FStringView Path);

        RUNTIME_API void ForgetWorld(CWorld* World);
    }
}
