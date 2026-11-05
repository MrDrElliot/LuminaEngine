#include "RuntimePCH.h"
#include "ImGuiDragDrop.h"
#include "World/ECS/Registry.h"

#include <cctype>
#include "imgui.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina::DragDrop
{
    static FPayload GPayload;

    // ImGui needs a non-zero buffer, and the typed data lives in the payload rather than here.
    static char GSentinel = 0;

    static FFixedString MakeFixed(FStringView View)
    {
        return FFixedString(View.GetData(), View.Len());
    }

    static FFixedString ExtractExtension(FStringView Path)
    {
        const int32 Dot = Path.FindLastOf('.');
        if (Dot == Constants::kIndexNone || Dot + 1 >= Path.Len())
        {
            return {};
        }
        FFixedString Lower;
        for (int32 i = Dot + 1; i < Path.Len(); ++i)
        {
            Lower.AppendChar((char)std::tolower((unsigned char)Path[i]));
        }
        return Lower;
    }

    static bool IExtensionEquals(const FFixedString& A, FStringView B)
    {
        if (A.Len() != B.Len()) return false;
        for (int32 i = 0; i < A.Len(); ++i)
        {
            if (std::tolower((unsigned char)A[i]) != std::tolower((unsigned char)B[i]))
            {
                return false;
            }
        }
        return true;
    }

    static void Reset()
    {
        GPayload = FPayload{};
    }

    void SetAssetPayload(FName ClassName, FStringView Path, CObject* Object)
    {
        Reset();
        GPayload.Kind = EPayloadKind::Asset;
        GPayload.AssetClassName = ClassName;
        GPayload.AssetPath = MakeFixed(Path);
        GPayload.AssetObject = Object;
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void SetAssetPayload(const FAssetData& Asset)
    {
        SetAssetPayload(Asset.AssetClass, FStringView(*Asset.Path, Asset.Path.Len()), nullptr);
    }

    void SetAssetPayload(CObject* Asset)
    {
        if (!Asset)
        {
            return;
        }
        FName ClassName = Asset->GetClass() ? Asset->GetClass()->GetName() : FName();
        FName Name = Asset->GetName();
        FString PathStr = Name.ToString();
        SetAssetPayload(ClassName, FStringView(*PathStr, PathStr.Len()), Asset);
    }

    void SetEntityPayload(CWorld* World, ECS::FEntity Entity)
    {
        Reset();
        GPayload.Kind = EPayloadKind::Entity;
        GPayload.World = World;
        GPayload.Entity = Entity;
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void SetSceneFolderPayload(CWorld* World, uint32 FolderID)
    {
        Reset();
        GPayload.Kind = EPayloadKind::SceneFolder;
        GPayload.World = World;
        GPayload.SceneFolderID = FolderID;
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void SetFilePayload(FStringView VirtualPath)
    {
        Reset();
        GPayload.Kind = EPayloadKind::File;
        GPayload.FilePath = MakeFixed(VirtualPath);
        GPayload.FileExtension = ExtractExtension(VirtualPath);
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void SetNewEntityPayload(FName EntityName, CStruct* ComponentType, CObject* Seed)
    {
        Reset();
        GPayload.Kind = EPayloadKind::NewEntity;
        GPayload.EntityName = EntityName;
        GPayload.ComponentType = ComponentType;
        GPayload.AssetObject = Seed;
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void SetPaletteItemPayload(FName Palette, int32 Index)
    {
        Reset();
        GPayload.Kind = EPayloadKind::PaletteItem;
        GPayload.PaletteName = Palette;
        GPayload.PaletteIndex = Index;
        ImGui::SetDragDropPayload(GImGuiPayloadType, &GSentinel, 1, ImGuiCond_Once);
    }

    void MarkSourceClosed()
    {
        if (GPayload.Kind != EPayloadKind::None)
        {
            GPayload.bSourceClosed = true;
        }
    }

    const FPayload* PeekPayload()
    {
        const ImGuiPayload* P = ImGui::GetDragDropPayload();
        if (P && P->IsDataType(GImGuiPayloadType))
        {
            return &GPayload;
        }
        return nullptr;
    }

    bool IsDelivered()
    {
        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        return P && P->IsDelivery();
    }

    CObject* AcceptAssetOfClass(CClass* Class)
    {
        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (!P)
        {
            return nullptr;
        }
        if (GPayload.Kind != EPayloadKind::Asset)
        {
            return nullptr;
        }
        if (Class != nullptr)
        {
            CClass* PayloadClass = FindObject<CClass>(GPayload.AssetClassName);
            if (PayloadClass == nullptr || !PayloadClass->IsChildOf(Class))
            {
                return nullptr;
            }
        }
        if (!P->IsDelivery())
        {
            return nullptr;
        }
        if (GPayload.AssetObject)
        {
            return GPayload.AssetObject;
        }
        // Lazy loaded by virtual path, so a source handing over only metadata still resolves on delivery.
        if (!GPayload.AssetPath.IsEmpty())
        {
            return StaticLoadObject(FStringView(*GPayload.AssetPath, GPayload.AssetPath.Len()));
        }
        return nullptr;
    }

    bool AcceptPaletteItem(FName Palette, int32& OutIndex)
    {
        if (GPayload.Kind != EPayloadKind::PaletteItem || GPayload.PaletteName != Palette)
        {
            return false;
        }

        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (P == nullptr || !P->IsDelivery())
        {
            return false;
        }

        OutIndex = GPayload.PaletteIndex;
        return true;
    }

    bool AcceptEntity(CWorld** OutWorld, ECS::FEntity* OutEntity)
    {
        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (!P)
        {
            return false;
        }
        if (GPayload.Kind != EPayloadKind::Entity)
        {
            return false;
        }
        if (!P->IsDelivery())
        {
            return false;
        }
        if (OutWorld)
        {
            *OutWorld = GPayload.World;
        }
        if (OutEntity)
        {
            *OutEntity = GPayload.Entity;
        }
        return true;
    }

    bool AcceptSceneFolder(CWorld** OutWorld, uint32* OutFolderID)
    {
        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (!P)
        {
            return false;
        }
        if (GPayload.Kind != EPayloadKind::SceneFolder)
        {
            return false;
        }
        if (!P->IsDelivery())
        {
            return false;
        }
        if (OutWorld)
        {
            *OutWorld = GPayload.World;
        }
        if (OutFolderID)
        {
            *OutFolderID = GPayload.SceneFolderID;
        }
        return true;
    }

    bool AcceptFile(FStringView ExtensionFilter, FFixedString& OutPath)
    {
        const ImGuiPayload* P = ImGui::AcceptDragDropPayload(GImGuiPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (!P)
        {
            return false;
        }
        if (GPayload.Kind != EPayloadKind::File)
        {
            return false;
        }
        if (!ExtensionFilter.IsEmpty() && !IExtensionEquals(GPayload.FileExtension, ExtensionFilter))
        {
            return false;
        }
        if (!P->IsDelivery())
        {
            return false;
        }
        OutPath = GPayload.FilePath;
        return true;
    }

    void EndFrameTick()
    {
        // Mirror ImGui clearing its payload on drag end so a stale FPayload doesn't outlive the drag.
        if (ImGui::GetDragDropPayload() == nullptr && GPayload.Kind != EPayloadKind::None)
        {
            Reset();
        }
    }
}
