#pragma once
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina
{
    class CClass;
    class CObject;
    class CMaterial;
    class CMaterialInstance;
    class CMaterialNodeGraph;
    class CMaterialOutputNode;
    class CEdGraphNode;
    class CEdNodeGraphPin;
    class CTexture;

    // Procedural material-graph authoring so import code can build a CMaterialNodeGraph headlessly. Call FinalizeGraph once after a batch of wires.
    namespace MaterialGraphBuilder
    {
        // Seeds the graph without creating an ImGui editor context, since Initialize() does and is unsafe off the main thread.
        EDITOR_API CMaterialNodeGraph* CreateHeadlessGraph(CMaterial* Material);

        EDITOR_API CMaterialOutputNode* GetOutputNode(CMaterialNodeGraph* Graph);

        EDITOR_API CEdGraphNode* AddNode(CMaterialNodeGraph* Graph, CClass* NodeClass, float X, float Y);

        template<typename T>
        T* AddNode(CMaterialNodeGraph* Graph, float X, float Y)
        {
            return static_cast<T*>(AddNode(Graph, T::StaticClass(), X, Y));
        }

        // Connects an output pin to an input pin (registers the link on both pins). Call FinalizeGraph after.
        EDITOR_API void Connect(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin);

        // Rebuilds the serialized connection list from the live pin links (so the graph saves correctly).
        EDITOR_API void FinalizeGraph(CMaterialNodeGraph* Graph);
    }

    namespace Import::Materials
    {
        // Returns one instance per source material, with one master per distinct render state. bSourceHasVertexColors is per import since a material is shared across meshes.
        EDITOR_API TVector<CMaterialInstance*> GenerateMaterials(
            TSpan<const Import::Mesh::FMeshImportMaterial>  SourceMaterials,
            TSpan<CTexture* const>                          ImageAssets,
            const FFixedString&                             MaterialsDir,
            const FFixedString&                             BaseName,
            TVector<TStrongObjectPtr<CObject>>&                   OutCreated,
            bool                                            bSourceHasVertexColors);
    }
}
