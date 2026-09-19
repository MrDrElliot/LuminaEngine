#include "Platform/GenericPlatform.h"
#include "World/ECS/Registry.h"
#include "Scripting/DotNet/LayoutRegistry.h"
#include "Containers/String.h"
#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/NavMeshSystem.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "Scripting/EntityScript.h"
#include "Scripting/EntityScript.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "AI/Navigation/NavTypes.h"
#include "GameplayTags/GameplayTagRegistry.h"
#include "GameplayTags/GameplayTagComponent.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "UI/RmlUiBridge.h"
#include "Input/InputActionMap.h"
#include "Input/InputQuery.h"
#include "Input/InputViewport.h"
#include "Input/InputContext.h"
#include "Input/InputMode.h"
#include "Events/MouseCodes.h"

// World is an opaque pointer and Entity an entity id, matching the component ops convention.

using namespace Lumina;
using namespace Lumina::DotNet;   // AsWorld / AsEntity / ToId

// Only the not-yet-reflectable surface lives here, the rest is generated from CWorld's declarations.

// The world's own FSystemContext, so a managed system can be handed a valid context outside a tick.
LUMINA_DOTNET_EXPORT(const void*, World_GetSystemContext)(uint64 World)
{
    CWorld* W = AsWorld(World);
    return W ? &W->GetSystemContext() : nullptr;
}

// Game, the engine-level session operations.

//~ Keyed on the script's CClass, so the same calls find a C++ script and a C# one.

// C# wraps the returned pointer, so the managed instance is the canonical one for that object.
LUMINA_DOTNET_EXPORT(void*, AddEntityScript)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return nullptr;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(FStringView(ClassName, (size_t)ClassLen)));
    return EntityScripts::Attach(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass);
}

// The first script on the entity whose class IS-A the named class, or null.
LUMINA_DOTNET_EXPORT(void*, FindEntityScript)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return nullptr;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(FStringView(ClassName, (size_t)ClassLen)));
    return EntityScripts::Find(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass);
}

// Returns the total count, so an under-sized buffer can retry as with the two-pass string protocol.
LUMINA_DOTNET_EXPORT(int32, FindEntityScripts)(uint64 World, uint32 Entity, const char* ClassName, int32 ClassLen,
    void** OutScripts, int32 Capacity)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || ClassName == nullptr)
    {
        return 0;
    }
    CClass* ScriptClass = FindObject<CClass>(FName(FStringView(ClassName, (size_t)ClassLen)));

    TVector<CEntityScript*> Found;
    EntityScripts::FindAll(ECS::GetWorldRegistry(*W), AsEntity(Entity), ScriptClass, Found);

    const int32 Count = (int32)Found.size();
    if (OutScripts != nullptr)
    {
        for (int32 Index = 0; Index < Count && Index < Capacity; ++Index)
        {
            OutScripts[Index] = Found[Index];
        }
    }
    return Count;
}

// Removes the slot holding the given instance handle, destroying the managed instance.
LUMINA_DOTNET_EXPORT(void, RemoveEntityScript)(uint64 World, uint32 Entity, void* Instance)
{
    CWorld* W = AsWorld(World);
    if (W == nullptr || Instance == nullptr)
    {
        return;
    }
    EntityScripts::Remove(ECS::GetWorldRegistry(*W), AsEntity(Entity), static_cast<CEntityScript*>(Instance));
}


// Multiple shakes sum, and each Play returns a handle to stop it. Game thread only.


// Debug draw, the World.Draw surface.

// Net, the role and mode queries, from which C# derives the rest.

// Valid only for the duration of the OnUpdate crossing, forwarding to the matching context method.

LUMINA_DOTNET_EXPORT(float, SystemContext_GetDeltaTime)(const FSystemContext* Ctx)
{
    return Ctx ? (float)Ctx->GetDeltaTime() : 0.0f;
}

LUMINA_DOTNET_EXPORT(double, SystemContext_GetTime)(const FSystemContext* Ctx)
{
    return Ctx ? Ctx->GetTime() : 0.0;
}

LUMINA_DOTNET_EXPORT(uint32, SystemContext_Create)(const FSystemContext* Ctx)
{
    return Ctx ? ToId(Ctx->Create()) : ToId(ECS::NullEntity);
}

LUMINA_DOTNET_EXPORT(void, SystemContext_Destroy)(const FSystemContext* Ctx, uint32 Entity)
{
    if (Ctx)
    {
        Ctx->Destroy(AsEntity(Entity));
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_SetEntityLocation)(const FSystemContext* Ctx, uint32 Entity, FVector3 Location)
{
    // The scheduler owns a non-const context, so removing const here is safe.
    if (Ctx)
    {
        const_cast<FSystemContext*>(Ctx)->SetEntityLocation(AsEntity(Entity), Location);
    }
}

LUMINA_DOTNET_EXPORT(void, SystemContext_DrawDebugLine)(const FSystemContext* Ctx, FVector3 Start, FVector3 End, FVector4 Color)
{
    if (Ctx)
    {
        Ctx->DrawDebugLine(Start, End, Color);
    }
}

// The process-global registry is the single source of truth, and id 0 means none.

// Queries are hierarchical, so an entity tagged with a leaf matches a query on its parent.

// IsEnabled lets the managed side skip per-script scope calls when nobody is recording.

// The document walking and locking lives in the bridge, and these are the flat ABI wrappers.

namespace
{
    FStringView UIView(const char* P, int32 Len)
    {
        return (P != nullptr && Len > 0) ? FStringView(P, (size_t)Len) : FStringView();
    }

    // A two-pass string return, sizing with a null buffer then filling, returning the full length.
    int32 UICopyOut(const FString& Value, char* Buffer, int32 Capacity)
    {
        const int32 Len = (int32)Value.size();
        if (Buffer != nullptr && Capacity > 0)
        {
            const int32 N = Len < Capacity ? Len : Capacity;
            for (int32 i = 0; i < N; ++i)
            {
                Buffer[i] = Value[(size_t)i];
            }
        }
        return Len;
    }
}

LUMINA_DOTNET_EXPORT(void*, UI_LoadDocument)(uint64 World, const char* Path, int32 Len)
{
    return RmlUi::LoadScreenDocument(AsWorld(World), UIView(Path, Len));
}

LUMINA_DOTNET_EXPORT(void*, UI_LoadDocumentFromMemory)(uint64 World, const char* Body, int32 BodyLen, const char* Url, int32 UrlLen)
{
    return RmlUi::LoadScreenDocumentFromMemory(AsWorld(World), UIView(Body, BodyLen), UIView(Url, UrlLen));
}

LUMINA_DOTNET_EXPORT(void, UI_UnloadDocument)(uint64 World, void* Document)
{
    RmlUi::UnloadScreenDocument(AsWorld(World), Document);
}

LUMINA_DOTNET_EXPORT(void, UI_ShowDocument)(void* Document, int32 Modal, int32 AutoFocus)
{
    RmlUi::ShowDocument(Document, Modal != 0, AutoFocus != 0);
}

LUMINA_DOTNET_EXPORT(void, UI_HideDocument)(void* Document)
{
    RmlUi::HideDocument(Document);
}

LUMINA_DOTNET_EXPORT(void, UI_PullDocumentToFront)(void* Document)
{
    RmlUi::PullDocumentToFront(Document);
}

LUMINA_DOTNET_EXPORT(void*, UI_GetDocumentRoot)(void* Document)
{
    return RmlUi::GetDocumentRoot(Document);
}

LUMINA_DOTNET_EXPORT(void*, UI_GetElementById)(void* Document, const char* Id, int32 Len)
{
    return RmlUi::DocumentGetElementById(Document, UIView(Id, Len));
}

LUMINA_DOTNET_EXPORT(void*, UI_QuerySelector)(void* Element, const char* Selector, int32 Len)
{
    return RmlUi::ElementQuerySelector(Element, UIView(Selector, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_SetInnerRml)(void* Element, const char* Rml, int32 Len)
{
    RmlUi::ElementSetInnerRml(Element, UIView(Rml, Len));
}

LUMINA_DOTNET_EXPORT(int32, UI_GetInnerRml)(void* Element, char* Buffer, int32 Capacity)
{
    return UICopyOut(RmlUi::ElementGetInnerRml(Element), Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(void, UI_SetAttribute)(void* Element, const char* Name, int32 NameLen, const char* Value, int32 ValueLen)
{
    RmlUi::ElementSetAttribute(Element, UIView(Name, NameLen), UIView(Value, ValueLen));
}

LUMINA_DOTNET_EXPORT(int32, UI_GetAttribute)(void* Element, const char* Name, int32 NameLen, char* Buffer, int32 Capacity)
{
    return UICopyOut(RmlUi::ElementGetAttribute(Element, UIView(Name, NameLen)), Buffer, Capacity);
}

LUMINA_DOTNET_EXPORT(void, UI_SetProperty)(void* Element, const char* Name, int32 NameLen, const char* Value, int32 ValueLen)
{
    RmlUi::ElementSetProperty(Element, UIView(Name, NameLen), UIView(Value, ValueLen));
}

LUMINA_DOTNET_EXPORT(void, UI_RemoveProperty)(void* Element, const char* Name, int32 NameLen)
{
    RmlUi::ElementRemoveProperty(Element, UIView(Name, NameLen));
}

LUMINA_DOTNET_EXPORT(void, UI_SetClass)(void* Element, const char* Class, int32 ClassLen, int32 Active)
{
    RmlUi::ElementSetClass(Element, UIView(Class, ClassLen), Active != 0);
}

LUMINA_DOTNET_EXPORT(int32, UI_IsClassSet)(void* Element, const char* Class, int32 ClassLen)
{
    return RmlUi::ElementIsClassSet(Element, UIView(Class, ClassLen)) ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, UI_ElementFocus)(void* Element) { RmlUi::ElementFocus(Element); }
LUMINA_DOTNET_EXPORT(void, UI_ElementBlur)(void* Element)  { RmlUi::ElementBlur(Element); }
LUMINA_DOTNET_EXPORT(void, UI_ElementClick)(void* Element) { RmlUi::ElementClick(Element); }

LUMINA_DOTNET_EXPORT(void, UI_GetElementBox)(void* Element, float* OutXYWH, int32 Count)
{
    if (Count >= 4)
    {
        RmlUi::ElementGetBox(Element, OutXYWH);
    }
}

LUMINA_DOTNET_EXPORT(void*, UI_AddEventListener)(uint64 World, void* Element, const char* Type, int32 Len)
{
    return RmlUi::AddElementEventListener(AsWorld(World), Element, UIView(Type, Len));
}

LUMINA_DOTNET_EXPORT(void*, UI_GetEventListenerDelegate)(void* Listener)
{
    return RmlUi::GetElementEventListenerDelegate(Listener);
}

LUMINA_DOTNET_EXPORT(void, UI_RemoveEventListener)(uint64 World, void* Listener)
{
    RmlUi::RemoveElementEventListener(AsWorld(World), Listener);
}

// Variables register before the document loads, and values cross as doubles or string pairs.
LUMINA_DOTNET_EXPORT(void*, UI_CreateDataModel)(uint64 World, const char* Name, int32 Len, void* Context, void* SetThunk, void* EventThunk)
{
    return RmlUi::CreateDataModel(AsWorld(World), UIView(Name, Len), Context,
        reinterpret_cast<RmlUi::FManagedDataSetThunk>(SetThunk),
        reinterpret_cast<RmlUi::FManagedDataEventThunk>(EventThunk));
}

LUMINA_DOTNET_EXPORT(int32, UI_ModelBindScalar)(void* Model, const char* Name, int32 Len, int32 Type)
{
    return RmlUi::DataModelBindScalar(Model, UIView(Name, Len), Type);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelBindCommand)(void* Model, const char* Name, int32 Len, int32 CommandId)
{
    RmlUi::DataModelBindCommand(Model, UIView(Name, Len), CommandId);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelSetNumber)(void* Model, int32 Field, double Value)
{
    RmlUi::DataModelSetNumber(Model, Field, Value);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelSetString)(void* Model, int32 Field, const char* Value, int32 Len)
{
    RmlUi::DataModelSetString(Model, Field, UIView(Value, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelDirty)(void* Model, int32 Field)
{
    RmlUi::DataModelDirty(Model, Field);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelDirtyAll)(void* Model)
{
    RmlUi::DataModelDirtyAll(Model);
}

LUMINA_DOTNET_EXPORT(void, UI_DestroyDataModel)(void* Model)
{
    RmlUi::DestroyDataModel(Model);
}

// Array-of-struct variables with string cells, pushed as a snapshot on change.
LUMINA_DOTNET_EXPORT(int32, UI_ModelBindList)(void* Model, const char* Name, int32 Len)
{
    return RmlUi::DataModelBindList(Model, UIView(Name, Len));
}

LUMINA_DOTNET_EXPORT(int32, UI_ModelBindListMember)(void* Model, int32 ListField, const char* Name, int32 Len)
{
    return RmlUi::DataModelBindListMember(Model, ListField, UIView(Name, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListResize)(void* Model, int32 ListField, int32 RowCount)
{
    RmlUi::DataModelListResize(Model, ListField, RowCount);
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListSetCell)(void* Model, int32 ListField, int32 Row, int32 Col, const char* Value, int32 Len)
{
    RmlUi::DataModelListSetCell(Model, ListField, Row, Col, UIView(Value, Len));
}

LUMINA_DOTNET_EXPORT(void, UI_ModelListDirty)(void* Model, int32 ListField)
{
    RmlUi::DataModelListDirty(Model, ListField);
}

// A binding resolves its name once per settings generation, so it costs no crossing per frame.
