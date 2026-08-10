#pragma once
#include "Containers/Array.h"
#include "Containers/String.h"
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

    // Procedural material-graph authoring. These mirror the manual editor actions (spawn node, position it,
    // wire a pin) so import / tooling code can build a CMaterialNodeGraph headlessly, then compile it via
    // CompileMaterialGraph. Connections are registered directly; call FinalizeGraph once after a batch of wires.
    namespace MaterialGraphBuilder
    {
        // Creates the "AssetMaterialGraph" child object in Material's package, links it to the material, and
        // seeds the single CMaterialOutputNode -- WITHOUT creating an ImGui editor context (Initialize() does,
        // which is unsafe off the main thread). The graph still round-trips into the editor on later open.
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
        // Generates the PBR master material(s) + one CMaterialInstance per entry in SourceMaterials, and
        // returns the instances at matching indices (null where generation failed). One master is created
        // per distinct render state (blend mode / shading model / two-sided / cutoff), since instances can
        // only diverge in parameters. Texture parameters resolve through ImageAssets, indexed by the source
        // image indices the materials carry. MaterialsDir is the package directory every generated asset is
        // created under. Every created CObject is appended to OutCreated for the caller to save, register,
        // and tear down alongside the rest of the import.
        //
        // bSourceHasVertexColors (FMeshImportData::bHasVertexColors) adds the COLOR_0 multiply to every
        // master's base-colour chain. It is per IMPORT rather than per material because a material is shared
        // across meshes and cannot know which of them are coloured; the importers fill opaque white where a
        // mesh has no attribute, so the multiply is a no-op there.
        EDITOR_API TVector<CMaterialInstance*> GenerateMaterials(
            TSpan<const Import::Mesh::FMeshImportMaterial>  SourceMaterials,
            TSpan<CTexture* const>                          ImageAssets,
            const FFixedString&                             MaterialsDir,
            const FFixedString&                             BaseName,
            TVector<CObject*>&                              OutCreated,
            bool                                            bSourceHasVertexColors);
    }
}
