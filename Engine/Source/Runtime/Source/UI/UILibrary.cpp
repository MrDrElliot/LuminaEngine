#include "UI/UILibrary.h"

#include "UI/RmlUiBridge.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        template<typename THandle>
        void* AsPtr(THandle Handle)
        {
            return reinterpret_cast<void*>(Handle.Handle);
        }

        template<typename THandle>
        THandle FromPtr(void* Ptr)
        {
            THandle Handle;
            Handle.Handle = reinterpret_cast<uint64>(Ptr);
            return Handle;
        }
    }

    FVector2 CUILibrary::GetLayoutSize(CWorld* World)
    {
        const FUIntVector2 Size = RmlUi::GetWorldLayoutSize(World);
        return FVector2((float)Size.x, (float)Size.y);
    }

    FUIDocument CUILibrary::LoadDocument(CWorld* World, const FString& Path)
    {
        return FromPtr<FUIDocument>(RmlUi::LoadScreenDocument(World, Path));
    }

    FUIDocument CUILibrary::LoadDocumentFromMemory(CWorld* World, const FString& Rml, const FString& SourceUrl)
    {
        return FromPtr<FUIDocument>(RmlUi::LoadScreenDocumentFromMemory(World, Rml, SourceUrl));
    }

    void CUILibrary::UnloadDocument(CWorld* World, FUIDocument Document)
    {
        RmlUi::UnloadScreenDocument(World, AsPtr(Document));
    }

    void CUILibrary::ShowDocument(FUIDocument Document, bool bModal, bool bAutoFocus)
    {
        RmlUi::ShowDocument(AsPtr(Document), bModal, bAutoFocus);
    }

    void CUILibrary::HideDocument(FUIDocument Document)
    {
        RmlUi::HideDocument(AsPtr(Document));
    }

    void CUILibrary::BringDocumentToFront(FUIDocument Document)
    {
        RmlUi::PullDocumentToFront(AsPtr(Document));
    }

    FUIElement CUILibrary::GetDocumentRoot(FUIDocument Document)
    {
        return FromPtr<FUIElement>(RmlUi::GetDocumentRoot(AsPtr(Document)));
    }

    FUIElement CUILibrary::GetElementById(FUIDocument Document, const FString& Id)
    {
        return FromPtr<FUIElement>(RmlUi::DocumentGetElementById(AsPtr(Document), Id));
    }

    FUIElement CUILibrary::QuerySelector(FUIElement Element, const FString& Selector)
    {
        return FromPtr<FUIElement>(RmlUi::ElementQuerySelector(AsPtr(Element), Selector));
    }

    void CUILibrary::SetInnerRml(FUIElement Element, const FString& Rml)
    {
        RmlUi::ElementSetInnerRml(AsPtr(Element), Rml);
    }

    FString CUILibrary::GetInnerRml(FUIElement Element)
    {
        return RmlUi::ElementGetInnerRml(AsPtr(Element));
    }

    void CUILibrary::SetAttribute(FUIElement Element, const FString& Name, const FString& Value)
    {
        RmlUi::ElementSetAttribute(AsPtr(Element), Name, Value);
    }

    FString CUILibrary::GetAttribute(FUIElement Element, const FString& Name)
    {
        return RmlUi::ElementGetAttribute(AsPtr(Element), Name);
    }

    void CUILibrary::SetProperty(FUIElement Element, const FString& Name, const FString& Value)
    {
        RmlUi::ElementSetProperty(AsPtr(Element), Name, Value);
    }

    void CUILibrary::RemoveProperty(FUIElement Element, const FString& Name)
    {
        RmlUi::ElementRemoveProperty(AsPtr(Element), Name);
    }

    void CUILibrary::SetClass(FUIElement Element, const FString& Class, bool bActive)
    {
        RmlUi::ElementSetClass(AsPtr(Element), Class, bActive);
    }

    bool CUILibrary::IsClassSet(FUIElement Element, const FString& Class)
    {
        return RmlUi::ElementIsClassSet(AsPtr(Element), Class);
    }

    void CUILibrary::FocusElement(FUIElement Element)
    {
        RmlUi::ElementFocus(AsPtr(Element));
    }

    void CUILibrary::BlurElement(FUIElement Element)
    {
        RmlUi::ElementBlur(AsPtr(Element));
    }

    void CUILibrary::ClickElement(FUIElement Element)
    {
        RmlUi::ElementClick(AsPtr(Element));
    }

    FVector4 CUILibrary::GetElementBox(FUIElement Element)
    {
        float XYWH[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        RmlUi::ElementGetBox(AsPtr(Element), XYWH);
        return FVector4(XYWH[0], XYWH[1], XYWH[2], XYWH[3]);
    }

    FUIEventListener CUILibrary::AddEventListener(CWorld* World, FUIElement Element, const FString& EventType)
    {
        return FromPtr<FUIEventListener>(RmlUi::AddElementEventListener(World, AsPtr(Element), EventType));
    }

    void CUILibrary::RemoveEventListener(CWorld* World, FUIEventListener Listener)
    {
        RmlUi::RemoveElementEventListener(World, AsPtr(Listener));
    }

    uint64 CUILibrary::GetEventListenerDelegate(FUIEventListener Listener)
    {
        return reinterpret_cast<uint64>(RmlUi::GetElementEventListenerDelegate(AsPtr(Listener)));
    }

    FUIDataModel CUILibrary::CreateDataModel(CWorld* World, const FString& Name, uint64 Context,
        uint64 SetThunk, uint64 EventThunk)
    {
        return FromPtr<FUIDataModel>(RmlUi::CreateDataModel(World, Name, reinterpret_cast<void*>(Context),
            reinterpret_cast<RmlUi::FManagedDataSetThunk>(SetThunk),
            reinterpret_cast<RmlUi::FManagedDataEventThunk>(EventThunk)));
    }

    void CUILibrary::DestroyDataModel(FUIDataModel Model)
    {
        RmlUi::DestroyDataModel(AsPtr(Model));
    }

    int32 CUILibrary::BindScalar(FUIDataModel Model, const FString& Name, EUIVarType Type)
    {
        return RmlUi::DataModelBindScalar(AsPtr(Model), Name, (int32)Type);
    }

    void CUILibrary::BindCommand(FUIDataModel Model, const FString& Name, int32 CommandId)
    {
        RmlUi::DataModelBindCommand(AsPtr(Model), Name, CommandId);
    }

    void CUILibrary::SetNumber(FUIDataModel Model, int32 Field, double Value)
    {
        RmlUi::DataModelSetNumber(AsPtr(Model), Field, Value);
    }

    void CUILibrary::SetString(FUIDataModel Model, int32 Field, const FString& Value)
    {
        RmlUi::DataModelSetString(AsPtr(Model), Field, Value);
    }

    void CUILibrary::MarkDirty(FUIDataModel Model, int32 Field)
    {
        RmlUi::DataModelDirty(AsPtr(Model), Field);
    }

    void CUILibrary::MarkAllDirty(FUIDataModel Model)
    {
        RmlUi::DataModelDirtyAll(AsPtr(Model));
    }

    int32 CUILibrary::BindList(FUIDataModel Model, const FString& Name)
    {
        return RmlUi::DataModelBindList(AsPtr(Model), Name);
    }

    int32 CUILibrary::BindListMember(FUIDataModel Model, int32 ListField, const FString& MemberName)
    {
        return RmlUi::DataModelBindListMember(AsPtr(Model), ListField, MemberName);
    }

    void CUILibrary::ResizeList(FUIDataModel Model, int32 ListField, int32 RowCount)
    {
        RmlUi::DataModelListResize(AsPtr(Model), ListField, RowCount);
    }

    void CUILibrary::SetListCell(FUIDataModel Model, int32 ListField, int32 Row, int32 Col, const FString& Value)
    {
        RmlUi::DataModelListSetCell(AsPtr(Model), ListField, Row, Col, Value);
    }

    void CUILibrary::MarkListDirty(FUIDataModel Model, int32 ListField)
    {
        RmlUi::DataModelListDirty(AsPtr(Model), ListField);
    }
}
