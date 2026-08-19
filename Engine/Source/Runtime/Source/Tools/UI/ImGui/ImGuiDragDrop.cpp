#include "RuntimePCH.h"
#include "ImGuiDragDrop.h"

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

    // ImGui needs a non-zero data buffer for SetDragDropPayload. We only use
    // the channel as a signal, the typed data lives in GPayload.
    static char GSentinel = 0;

    static FFixedString MakeFixed(FStringView View)
    {
        return FFixedString(View.data(), View.size());
    }

    static FFixedString ExtractExtension(FStringView Path)
    {
        const size_t Dot = Path.find_last_of('.');
        if (Dot == FStringView::npos || Dot + 1 >= Path.size())
        {
            return {};
        }
        FFixedString Lower;
        for (size_t i = Dot + 1; i < Path.size(); ++i)
        {
            Lower.push_back((char)std::tolower((unsigned char)Path[i]));
        }
        return Lower;
    }

    static bool IExtensionEquals(const FFixedString& A, FStringView B)
    {
        if (A.size() != B.size()) return false;
        for (size_t i = 0; i < A.size(); ++i)
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
        SetAssetPayload(Asset.AssetClass, FStringView(Asset.Path.c_str(), Asset.Path.size()), nullptr);
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
        SetAssetPayload(ClassName, FStringView(PathStr.c_str(), PathStr.size()), Asset);
    }

    void SetEntityPayload(CWorld* World, entt::entity Entity)
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
        // Lazy load by virtual path so Sources that hand us only metadata still
        // resolve to a usable asset on delivery.
        if (!GPayload.AssetPath.empty())
        {
            return StaticLoadObject(FStringView(GPayload.AssetPath.c_str(), GPayload.AssetPath.size()));
        }
        return nullptr;
    }

    bool AcceptEntity(CWorld** OutWorld, entt::entity* OutEntity)
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
        if (!ExtensionFilter.empty() && !IExtensionEquals(GPayload.FileExtension, ExtensionFilter))
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

    bool AcceptScript(FFixedString& OutPath)
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
        if (!IExtensionEquals(GPayload.FileExtension, FStringView("luau"))
         && !IExtensionEquals(GPayload.FileExtension, FStringView("lua")))
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
