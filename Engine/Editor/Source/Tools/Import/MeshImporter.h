#pragma once

#include "Tools/Import/Importer.h"
#include "Tools/Import/ImportHelpers.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/MeshDistanceField.h"
#include "MeshImporter.generated.h"

namespace Lumina
{
    class CMesh;
    class CTexture;
    class CSkeleton;
    class CMaterialInstance;
    class CPrefab;

    /** How a source file's light intensities are interpreted when they become light components. */
    REFLECT()
    enum class ELightImportUnits : uint8
    {
        /** Convert from what the format authored in: glTF's lux and candela, or FBX's ratios anchored to its own brightest light. */
        Photometric,

        /** Use the source intensity verbatim. Only useful for files authored against engine units. */
        Raw,
    };

    /**
     * Shared stages of every mesh-format import. A format importer implements ParseMeshSource (parse ->
     * discovery -> dedup) and inherits processing, asset creation and serialization from here, so the
     * per-format code is only ever the part that reads the file.
     */
    REFLECT()
    class EDITOR_API CMeshImporter : public CImporter
    {
        GENERATED_BODY()
    public:

        /** Import static and skeletal mesh geometry. */
        PROPERTY(Editable, Category = "Import")
        bool bImportMeshes = true;

        /** Import skeleton hierarchies and bone data. */
        PROPERTY(Editable, Category = "Import")
        bool bImportSkeleton = true;

        /** Import skeletal animation clips. */
        PROPERTY(Editable, Category = "Import")
        bool bImportAnimations = true;

        // Bind imported clips to an existing skeleton instead of one from this file. Safe whenever the
        // bone NAMES match, which is what channels resolve against; the dialogue reports the match.
        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CSkeleton> TargetSkeleton;

        /** Import material definitions and generate material assets for them. */
        PROPERTY(Editable, Category = "Import")
        bool bImportMaterials = true;

        /** Import the texture files the source references. Implied by Import Materials. */
        PROPERTY(Editable, Category = "Import")
        bool bImportTextures = true;

        /** Uniform scale applied to all imported geometry, skeletons and animation translations. */
        PROPERTY(Editable, Category = "Transform", ClampMin = "0.001", ClampMax = "1000.0")
        float Scale = 1.0f;

        /** Flip UVs vertically (1 - V). */
        PROPERTY(Editable, Category = "Transform")
        bool bFlipUVs = false;

        /** Flip UVs horizontally (1 - U); for sources whose UVs are mirrored (backwards text). */
        PROPERTY(Editable, Category = "Transform")
        bool bFlipU = false;

        /** Invert mesh normals, for inside-out geometry. */
        PROPERTY(Editable, Category = "Transform")
        bool bFlipNormals = false;

        /** Optimize vertex cache locality and overdraw before meshlets are built. */
        PROPERTY(Editable, Category = "Geometry")
        bool bOptimize = true;

        /**
         * Flatten every mesh instance in the source into a single asset, baking each node's world transform
         * into its vertices. Off imports one asset per unique mesh in object space, which is what an
         * instanced scene wants: 10,000 nodes sharing a mesh cost one mesh, not 10,000.
         */
        PROPERTY(Editable, Category = "Geometry")
        bool bMergeMeshes = false;

        /**
         * Also create a prefab that reproduces the source scene: one entity per node, parented as the
         * source parents them, carrying the imported meshes and lights. Placing the prefab reproduces the
         * authored scene, while each mesh stays a single shared asset. Ignored when merging.
         *
         * Off by default: it costs one entity per source node, so a scene with a large node count produces
         * a correspondingly large asset (a 187k-node source is a ~190MB prefab), which is worth opting
         * into rather than paying for on every import.
         */
        PROPERTY(Editable, Category = "Scene")
        bool bImportAsPrefab = false;

        /**
         * Also import the source's cameras as entities carrying SCameraComponent. The first one found
         * auto-activates, so instantiating the prefab reproduces the viewpoint the scene was framed from --
         * which is what you want when comparing the import against a DCC render.
         */
        PROPERTY(Editable, Category = "Scene")
        bool bImportCameras = true;

        /** Also import the source's lights, so a placed prefab lights itself instead of borrowing the host world's lighting. */
        PROPERTY(Editable, Category = "Scene")
        bool bImportLights = true;

        /**
         * Author an environment onto the prefab root: a flat-color sky, a matching skylight and a neutral
         * post-process volume.
         *
         * glTF carries no world/environment of its own, so without this an imported scene is lit by whatever
         * the host world happens to provide -- in a default Lumina world that is a procedural sky, a 3.2
         * directional light and an art-directed grade the source never asked for. Supplying its own makes
         * the prefab reproduce the same look wherever it is placed.
         */
        PROPERTY(Editable, Category = "Scene")
        bool bCreateSceneEnvironment = true;

        /**
         * The world background the source was authored against, which glTF cannot express. Drives both the
         * flat sky color and the skylight ambient when Create Scene Environment is on. The default is
         * Blender's own default world grey.
         */
        PROPERTY(Editable, Color, Category = "Scene")
        FVector3 WorldColor = FVector3(0.05f, 0.05f, 0.05f);

        /** How to interpret the source's light intensities. See ELightImportUnits. */
        PROPERTY(Editable, Category = "Scene|Light Units")
        ELightImportUnits LightUnits = ELightImportUnits::Photometric;

        /**
         * Engine directional intensity produced by one watt per square metre of authored sun strength.
         *
         * Photometric conversion runs in two steps. The first is exact: glTF directional intensity is lux,
         * and dividing by 683 lm/W (luminous efficacy at 555nm, the constant DCC exporters use going the
         * other way) recovers the radiometric value the artist authored. The second is this calibration,
         * because the engine's intensity is an arbitrary multiplier rather than a physical unit. The default
         * is the engine's own SDirectionalLightComponent default, so a Blender sun left at its default
         * strength of 1.0 imports as an engine sun left at its default.
         */
        PROPERTY(Editable, Category = "Scene|Light Units", ClampMin = "0.0")
        float DirectionalLightCalibration = 1.5f;

        /**
         * Engine point/spot intensity produced by one watt of authored lamp power.
         *
         * Same two steps as the directional case: glTF point/spot intensity is candela, so multiplying by
         * 4pi steradians and dividing by 683 lm/W recovers the authored wattage. The default calibration
         * maps Blender's default 1000W lamp onto the engine's default point intensity of 10.
         */
        PROPERTY(Editable, Category = "Scene|Light Units", ClampMin = "0.0")
        float PunctualLightCalibration = 0.01f;

        /** Trim applied on top of the converted intensity of every imported light. */
        PROPERTY(Editable, Category = "Scene|Light Units", ClampMin = "0.0")
        float LightIntensityScale = 1.0f;

        /**
         * Bakes a signed distance field volume per mesh for the Distance Field material nodes. The single
         * most expensive step of an import; can be added later from the mesh editor.
         */
        PROPERTY(Editable, Category = "Distance Field")
        SDistanceFieldBuildSettings DistanceField;

        bool HasSettingsDialogue() const override { return true; }

        bool ParseSource(const FImportRequest& Request, FString& OutError, FScopedSlowTask* Progress) override;
        void PrepareSettingsPreview() override;
        void BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress) override;
        void DrawSourcePreview() override;
        void ReleaseSourceData() override;

        bool CanReimport(const CStruct* AssetClass) const override;
        bool ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;

    protected:

        /**
         * The one stage a format importer supplies: read SourcePath into OutData, already deduplicated.
         * Options.bSkipFinalization is set for the settings-preview parse, where the heavy passes are
         * deliberately deferred to commit time.
         */
        virtual bool ParseMeshSource(const FImportRequest& Request,
                                     const Import::Mesh::FMeshImportOptions& Options,
                                     Import::Mesh::FMeshImportData& OutData,
                                     FString& OutError,
                                     FScopedSlowTask* Progress) { return false; }

        Import::Mesh::FMeshImportOptions BuildOptions(bool bSkipFinalization) const;

        /**
         * Builds the scene prefab from SourceData.SceneNodes and the meshes just created. ResourceToMesh is
         * indexed by FMeshImportData::Resources index. Returns null when the source has no usable hierarchy.
         */
        CPrefab* BuildScenePrefab(const FFixedString& PackagePath, FStringView PrefabName,
                                  const TVector<CMesh*>& ResourceToMesh) const;

        /** Source directional intensity -> engine directional intensity, including the LightIntensityScale trim. */
        float ConvertDirectionalIntensity(const Import::Mesh::FSourceLight& Light, float BrightestOfKind) const;

        /** Source point/spot intensity -> engine intensity, trim included. */
        float ConvertPunctualIntensity(const Import::Mesh::FSourceLight& Light, float BrightestOfKind) const;

        Import::Mesh::FMeshImportData SourceData;
    };
}
