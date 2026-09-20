#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Containers/String.h"
#include "UI/UITypes.h"
#include "UILibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // The script-facing half of the RmlUi bridge, reflected so C# binds it instead of hand-written thunks.
    REFLECT()
    class RUNTIME_API CUILibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        //~ Documents

        /** Loads a virtual path into the world's screen context. The document starts hidden. */
        FUNCTION()
        static FUIDocument LoadDocument(CWorld* World, const FString& Path);

        /** SourceUrl resolves the document's relative includes such as stylesheets and images. */
        FUNCTION()
        static FUIDocument LoadDocumentFromMemory(CWorld* World, const FString& Rml, const FString& SourceUrl);

        FUNCTION()
        static void UnloadDocument(CWorld* World, FUIDocument Document);

        /** Modal blocks focus to other documents, AutoFocus focuses the document's first autofocus element. */
        FUNCTION()
        static void ShowDocument(FUIDocument Document, bool bModal = false, bool bAutoFocus = true);

        FUNCTION()
        static void HideDocument(FUIDocument Document);

        FUNCTION()
        static void BringDocumentToFront(FUIDocument Document);

        /** The document's body element. */
        FUNCTION()
        static FUIElement GetDocumentRoot(FUIDocument Document);

        FUNCTION()
        static FUIElement GetElementById(FUIDocument Document, const FString& Id);

        //~ Elements

        FUNCTION()
        static FUIElement QuerySelector(FUIElement Element, const FString& Selector);

        FUNCTION()
        static void SetInnerRml(FUIElement Element, const FString& Rml);

        FUNCTION()
        static FString GetInnerRml(FUIElement Element);

        FUNCTION()
        static void SetAttribute(FUIElement Element, const FString& Name, const FString& Value);

        FUNCTION()
        static FString GetAttribute(FUIElement Element, const FString& Name);

        /** Sets an inline CSS property. */
        FUNCTION()
        static void SetProperty(FUIElement Element, const FString& Name, const FString& Value);

        FUNCTION()
        static void RemoveProperty(FUIElement Element, const FString& Name);

        FUNCTION()
        static void SetClass(FUIElement Element, const FString& Class, bool bActive);

        FUNCTION()
        static bool IsClassSet(FUIElement Element, const FString& Class);

        FUNCTION()
        static void FocusElement(FUIElement Element);

        FUNCTION()
        static void BlurElement(FUIElement Element);

        FUNCTION()
        static void ClickElement(FUIElement Element);

        /** Border box in document pixels as X, Y, Width, Height. Zeroes when the element is gone. */
        FUNCTION()
        static FVector4 GetElementBox(FUIElement Element);

        //~ Event listeners

        FUNCTION()
        static FUIEventListener AddEventListener(CWorld* World, FUIElement Element, const FString& EventType);

        FUNCTION()
        static void RemoveEventListener(CWorld* World, FUIEventListener Listener);

        /** The listener's FScriptDelegateBase, which script binds its handler to. */
        FUNCTION()
        static uint64 GetEventListenerDelegate(FUIEventListener Listener);

        //~ Data models

        // Context is the managed GCHandle handed back to the two thunks, which are managed function pointers.
        FUNCTION()
        static FUIDataModel CreateDataModel(CWorld* World, const FString& Name, uint64 Context,
            uint64 SetThunk, uint64 EventThunk);

        FUNCTION()
        static void DestroyDataModel(FUIDataModel Model);

        /** Registers a scalar variable and returns its field id, or -1 on failure. */
        FUNCTION()
        static int32 BindScalar(FUIDataModel Model, const FString& Name, EUIVarType Type);

        /** Registers a callback bound by data-event-*, dispatched back under CommandId. */
        FUNCTION()
        static void BindCommand(FUIDataModel Model, const FString& Name, int32 CommandId);

        FUNCTION()
        static void SetNumber(FUIDataModel Model, int32 Field, double Value);

        FUNCTION()
        static void SetString(FUIDataModel Model, int32 Field, const FString& Value);

        /** Marks a variable dirty so RmlUi refreshes its views on the next context update. */
        FUNCTION()
        static void MarkDirty(FUIDataModel Model, int32 Field);

        FUNCTION()
        static void MarkAllDirty(FUIDataModel Model);

        //~ Data model lists, the array-of-struct variables data-for iterates

        /** Returns a list field id in its own index space, separate from the scalar fields, or -1. */
        FUNCTION()
        static int32 BindList(FUIDataModel Model, const FString& Name);

        /** Adds a named column reachable in RML as {{ item.Name }} and returns its index. */
        FUNCTION()
        static int32 BindListMember(FUIDataModel Model, int32 ListField, const FString& MemberName);

        /** Clears the list to RowCount empty rows. Set every cell next, then mark it dirty. */
        FUNCTION()
        static void ResizeList(FUIDataModel Model, int32 ListField, int32 RowCount);

        FUNCTION()
        static void SetListCell(FUIDataModel Model, int32 ListField, int32 Row, int32 Col, const FString& Value);

        FUNCTION()
        static void MarkListDirty(FUIDataModel Model, int32 ListField);
    };
}
