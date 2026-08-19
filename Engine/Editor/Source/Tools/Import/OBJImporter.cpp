#include "EditorPCH.h"
#include "OBJImporter.h"

#include <tinyobjloader/tiny_obj_loader.h>

#include "Core/Math/Math.h"
#include "Core/Progress/SlowTask.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    using namespace Import::Mesh;

    bool COBJImporter::ParseMeshSource(const FImportRequest& Request,
                                       const FMeshImportOptions& Options,
                                       FMeshImportData& OutData,
                                       FString& OutError,
                                       FScopedSlowTask* Progress)
    {
        tinyobj::ObjReaderConfig ReaderConfig;
        tinyobj::ObjReader Reader;

        if (!Reader.ParseFromFile(Request.SourcePath.c_str(), ReaderConfig))
        {
            OutError = FString(Reader.Error().c_str());
            return false;
        }

        if (!Reader.Warning().empty())
        {
            LOG_WARN("TinyObjReader Warning: {}", Reader.Warning());
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.45f, "Reading materials...");
        }

        const tinyobj::attrib_t& Attribute                = Reader.GetAttrib();
        const std::vector<tinyobj::shape_t>& Shapes       = Reader.GetShapes();
        const std::vector<tinyobj::material_t>& Materials = Reader.GetMaterials();

        const FStringView SourceDir = VFS::Parent(Request.SourcePath);

        THashMap<FFixedString, int32> ImageKeyToIndex;
        auto EmitImage = [&](const std::string& TexName, ETextureColorSpace Role) -> int32
        {
            if (TexName.empty() || !Options.bImportTextures)
            {
                return INDEX_NONE;
            }

            FFixedString Key(TexName.c_str());
            auto Existing = ImageKeyToIndex.find(Key);
            if (Existing != ImageKeyToIndex.end())
            {
                return Existing->second;
            }

            FSourceImage Image;
            Image.Key                = Key;
            Image.ResolvedPath       = Paths::Combine(SourceDir, Key);
            Image.IntendedColorSpace = Role;

            const int32 Index = (int32)OutData.Images.size();
            ImageKeyToIndex.emplace(Move(Key), Index);
            OutData.Images.push_back(Move(Image));
            return Index;
        };

        if (Options.bImportMaterials || Options.bImportTextures)
        {
            OutData.Materials.reserve(Materials.size());
            for (const tinyobj::material_t& Material : Materials)
            {
                // Emitted for every channel the cook can use, even when the material itself is not
                // imported, so "textures only" still brings the referenced files in.
                const int32 BaseColor = EmitImage(Material.diffuse_texname, ETextureColorSpace::SRGB);
                // Linear (BC7 RGB), not NormalMap: the BC5-packed normal path is currently broken.
                const int32 Normal    = !Material.normal_texname.empty()
                    ? EmitImage(Material.normal_texname, ETextureColorSpace::Linear)
                    : EmitImage(Material.bump_texname,   ETextureColorSpace::Linear);
                const int32 Emissive  = EmitImage(Material.emissive_texname, ETextureColorSpace::SRGB);
                const int32 Occlusion = EmitImage(Material.ambient_texname,  ETextureColorSpace::Linear);
                EmitImage(Material.specular_texname,           ETextureColorSpace::Linear);
                EmitImage(Material.specular_highlight_texname, ETextureColorSpace::Linear);
                EmitImage(Material.metallic_texname,           ETextureColorSpace::Linear);
                EmitImage(Material.roughness_texname,          ETextureColorSpace::Linear);

                if (!Options.bImportMaterials)
                {
                    continue;
                }

                FMeshImportMaterial Out;
                Out.Name = Material.name.empty() ? FString("Material") : FString(Material.name.c_str());

                Out.BaseColorFactor = FVector4(Material.diffuse[0], Material.diffuse[1], Material.diffuse[2], Material.dissolve);
                Out.MetallicFactor  = Material.metallic;

                // Classic OBJ has no roughness; derive it from the Phong shininess exponent so non-PBR
                // .mtl files don't import as mirror-smooth. PBR .mtl roughness (when authored) wins.
                Out.RoughnessFactor = (Material.roughness > 0.0f)
                    ? Material.roughness
                    : Math::Sqrt(2.0f / (Math::Max(Material.shininess, 0.0f) + 2.0f));

                Out.EmissiveColor = FVector3(Material.emission[0], Material.emission[1], Material.emission[2]);
                Out.AlphaMode     = (Material.dissolve < 0.999f) ? EImportAlphaMode::Blend : EImportAlphaMode::Opaque;

                Out.BaseColorImage = BaseColor;
                Out.NormalImage    = Normal;
                Out.EmissiveImage  = Emissive;
                Out.OcclusionImage = Occlusion;

                OutData.Materials.push_back(Move(Out));
            }
        }

        TUniquePtr<FMeshResource> MeshResource = MakeUnique<FMeshResource>();
        MeshResource->Name = VFS::FileName(Request.SourcePath, true);
        MeshResource->bSkinnedMesh = !Attribute.skin_weights.empty();

        const bool bIsSkinned = MeshResource->bSkinnedMesh;

        // A face-vertex is emitted per corner, so both streams are known exactly up front.
        {
            size_t TotalCorners = 0;
            for (const tinyobj::shape_t& Shape : Shapes)
            {
                TotalCorners += Shape.mesh.indices.size();
            }
            MeshResource->ReserveVertices(TotalCorners);
            MeshResource->Indices.reserve(TotalCorners);
        }

        const float ShapeStep = 0.55f / (float)Math::Max<size_t>(1, Shapes.size());
        uint32 ShapesDone = 0;

        for (const tinyobj::shape_t& Shape : Shapes)
        {
            const size_t NumFaces = Shape.mesh.num_face_vertices.size();

            TVector<size_t> FaceIndexOffsets(NumFaces);
            {
                size_t Running = 0;
                for (size_t Face = 0; Face < NumFaces; ++Face)
                {
                    FaceIndexOffsets[Face] = Running;
                    Running += Shape.mesh.num_face_vertices[Face];
                }
            }

            THashMap<int, TVector<size_t>> FacesByMaterial;
            for (size_t Face = 0; Face < NumFaces; ++Face)
            {
                const int MaterialID = Shape.mesh.material_ids.empty() ? -1 : Shape.mesh.material_ids[Face];
                FacesByMaterial[MaterialID].push_back(Face);
            }

            for (auto& [MaterialID, FaceList] : FacesByMaterial)
            {
                FGeometrySurface& Surface = MeshResource->GeometrySurfaces.emplace_back();
                if (FacesByMaterial.size() > 1)
                {
                    FFixedString SurfaceID;
                    SurfaceID.append(FStringView(Shape.name.data(), Shape.name.size())).append("_Mat").append(Format("{}", MaterialID));
                    Surface.ID = SurfaceID;
                }
                else
                {
                    Surface.ID = Shape.name.c_str();
                }
                Surface.MaterialIndex = (int16)MaterialID;
                Surface.IndexCount    = 0;
                Surface.StartIndex    = (uint32)MeshResource->Indices.size();

                for (size_t Face : FaceList)
                {
                    const size_t NumFaceVerts = Shape.mesh.num_face_vertices[Face];
                    const size_t IndexOffset  = FaceIndexOffsets[Face];

                    for (size_t V = 0; V < NumFaceVerts; ++V)
                    {
                        const tinyobj::index_t Index = Shape.mesh.indices[IndexOffset + V];

                        MeshResource->Indices.push_back((uint32)MeshResource->GetNumVertices());
                        Surface.IndexCount++;

                        FSourceSkinnedVertex Vertex;
                        Vertex.Normal       = 0;
                        Vertex.Tangent      = 0;   // Filled by MikkTSpace in GenerateMeshlets.
                        Vertex.UV           = 0;
                        Vertex.Color        = 0xFFFFFFFF;
                        Vertex.JointIndices = FU16Vector4(0);
                        Vertex.JointWeights = FU8Vector4(0);
                        Vertex.Position.x   = Attribute.vertices[3 * Index.vertex_index + 0];
                        Vertex.Position.y   = Attribute.vertices[3 * Index.vertex_index + 1];
                        Vertex.Position.z   = Attribute.vertices[3 * Index.vertex_index + 2];

                        if (Index.normal_index >= 0)
                        {
                            const FVector3 Normal(Attribute.normals[3 * Index.normal_index + 0],
                                                  Attribute.normals[3 * Index.normal_index + 1],
                                                  Attribute.normals[3 * Index.normal_index + 2]);
                            Vertex.Normal = PackNormal(Math::Normalize(Normal));
                        }

                        if (Index.texcoord_index >= 0)
                        {
                            Vertex.UV = Math::PackHalf2x16(FVector2(Attribute.texcoords[2 * Index.texcoord_index + 0],
                                                                    Attribute.texcoords[2 * Index.texcoord_index + 1]));
                        }

                        // OBJ carries a single UV channel; mirror it so a material sampling set 1 behaves.
                        Vertex.UV1 = Vertex.UV;

                        if (bIsSkinned)
                        {
                            MeshResource->AppendVertex(Vertex);
                        }
                        else
                        {
                            MeshResource->AppendVertex(static_cast<const FSourceVertex&>(Vertex));
                        }
                    }
                }
            }

            if (Progress)
            {
                ++ShapesDone;
                const FFixedString Message = FormatAs<FFixedString>("Reading geometry ({}/{} shapes)...", ShapesDone, (uint32)Shapes.size());
                Progress->EnterProgressFrame(ShapeStep, Message);
            }
        }

        if (!Options.bSkipFinalization)
        {
            if (Options.bOptimize)
            {
                OptimizeNewlyImportedMesh(*MeshResource);
            }
            GenerateMeshlets(*MeshResource);
        }

        OutData.SourceNodeCount = (uint32)Shapes.size();
        OutData.Resources.push_back(Move(MeshResource));
        return true;
    }
}
