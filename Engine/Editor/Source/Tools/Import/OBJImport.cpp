#include "PCH.h"
#include "Core/Math/Math.h"
#include <tinyobjloader/tiny_obj_loader.h>
#include "MeshFormatImport.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Progress/SlowTask.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Renderer/MeshData.h"


namespace Lumina::Import::Mesh::OBJ
{
    TExpected<FMeshImportData, FString> ImportOBJ(const FMeshImportOptions& ImportOptions, FStringView FilePath, FScopedSlowTask* Progress)
    {
        tinyobj::ObjReaderConfig ReaderConfig;

        tinyobj::ObjReader Reader;

        if (!Reader.ParseFromFile(FilePath.data(), ReaderConfig))
        {
            if (!Reader.Error().empty())
            {
                LOG_ERROR("TinyObjReader Error: {}", Reader.Error());
            }

            return TUnexpected(Reader.Error().c_str());
        }

        if (!Reader.Warning().empty())
        {
            LOG_WARN("TinyObjReader Warning: {}", Reader.Warning());
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.45f, "Reading materials...");
        }

    
        const tinyobj::attrib_t& Attribute                  = Reader.GetAttrib();
        const std::vector<tinyobj::shape_t>& Shapes         = Reader.GetShapes();
        const std::vector<tinyobj::material_t>& Materials   = Reader.GetMaterials();

        FMeshImportData ImportData;
        
        TUniquePtr<FMeshResource> MeshResource = MakeUnique<FMeshResource>();
        MeshResource->Name = VFS::FileName(FilePath, true);

        if (ImportOptions.bImportTextures)
        {
            auto EmitImage = [&](const std::string& TexName, ETextureColorSpace Role)
            {
                if (TexName.empty())
                {
                    return;
                }
                FMeshImportImage Image;
                Image.RelativePath = TexName.c_str();
                Image.IntendedColorSpace = Role;
                ImportData.Textures.emplace(Image);
            };

            for (const tinyobj::material_t& Material : Materials)
            {
                EmitImage(Material.diffuse_texname,            ETextureColorSpace::SRGB);
                // Linear (BC7 RGB), not NormalMap: the BC5-packed normal path is currently broken.
                EmitImage(Material.bump_texname,               ETextureColorSpace::Linear);
                EmitImage(Material.normal_texname,             ETextureColorSpace::Linear);
                EmitImage(Material.specular_texname,           ETextureColorSpace::Linear);
                EmitImage(Material.ambient_texname,            ETextureColorSpace::Linear);
                EmitImage(Material.specular_highlight_texname, ETextureColorSpace::Linear);
                EmitImage(Material.metallic_texname,           ETextureColorSpace::Linear);
                EmitImage(Material.roughness_texname,          ETextureColorSpace::Linear);
                EmitImage(Material.emissive_texname,           ETextureColorSpace::SRGB);
            }
        }

        // Material definitions (indexed by tinyobj material_id, matching FGeometrySurface::MaterialIndex).
        if (ImportOptions.bImportMaterials)
        {
            ImportData.Materials.reserve(Materials.size());
            for (const tinyobj::material_t& Material : Materials)
            {
                FMeshImportMaterial Out;
                Out.Name = Material.name.empty() ? FString("Material") : FString(Material.name.c_str());

                Out.BaseColorFactor = FVector4(Material.diffuse[0], Material.diffuse[1], Material.diffuse[2], Material.dissolve);
                Out.MetallicFactor  = Material.metallic;

                // Classic OBJ has no roughness; derive it from the Phong shininess exponent so non-PBR
                // .mtl files don't import as mirror-smooth. PBR .mtl roughness (when authored) wins.
                if (Material.roughness > 0.0f)
                {
                    Out.RoughnessFactor = Material.roughness;
                }
                else
                {
                    Out.RoughnessFactor = Math::Sqrt(2.0f / (Math::Max(Material.shininess, 0.0f) + 2.0f));
                }

                Out.EmissiveColor = FVector3(Material.emission[0], Material.emission[1], Material.emission[2]);

                Out.AlphaMode = (Material.dissolve < 0.999f) ? EImportAlphaMode::Blend : EImportAlphaMode::Opaque;

                Out.BaseColorTexture = Material.diffuse_texname.c_str();
                Out.NormalTexture    = !Material.normal_texname.empty() ? Material.normal_texname.c_str() : Material.bump_texname.c_str();
                Out.EmissiveTexture  = Material.emissive_texname.c_str();

                ImportData.Materials.push_back(Move(Out));
            }
        }
        
        bool bIsSkinned = !Attribute.skin_weights.empty();
        MeshResource->bSkinnedMesh = bIsSkinned;

        // The geometry phase owns the final 0.55 of the parse budget, spread per shape.
        const float ShapeStep = 0.55f / (float)eastl::max<size_t>((size_t)1, Shapes.size());
        uint32 ShapesDone = 0;

        for (const tinyobj::shape_t& Shape : Shapes)
        {
            const size_t NumFaces = Shape.mesh.num_face_vertices.size();
            
            TVector<size_t> FaceIndexOffsets;
            FaceIndexOffsets.resize(NumFaces);
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
                const int MaterialID = Shape.mesh.material_ids.empty()
                    ? -1
                    : Shape.mesh.material_ids[Face];
                FacesByMaterial[MaterialID].push_back(Face);
            }

            for (auto& [MaterialID, FaceList] : FacesByMaterial)
            {
                FGeometrySurface& Surface = MeshResource->GeometrySurfaces.emplace_back();
                if (FacesByMaterial.size() > 1)
                {
                    FFixedString SurfaceID;
                    SurfaceID.append_convert(Shape.name).append("_Mat").append_convert(eastl::to_string(MaterialID));
                    Surface.ID = SurfaceID;
                }
                else
                {
                    Surface.ID = Shape.name.c_str();
                }
                Surface.MaterialIndex = (int16)MaterialID;
                Surface.IndexCount    = 0;
                Surface.StartIndex    = static_cast<uint32>(MeshResource->Indices.size());

                for (size_t Face : FaceList)
                {
                    const size_t NumFaceVerts = Shape.mesh.num_face_vertices[Face];
                    const size_t IndexOffset  = FaceIndexOffsets[Face];

                    for (size_t V = 0; V < NumFaceVerts; ++V)
                    {
                        tinyobj::index_t Index = Shape.mesh.indices[IndexOffset + V];

                        MeshResource->Indices.push_back(static_cast<uint32>(MeshResource->GetNumVertices()));
                        Surface.IndexCount++;

                        FSkinnedVertex Vertex;
                        Vertex.Normal   = 0;
                        Vertex.Tangent  = 0;  // Filled by MikkTSpace in GenerateMeshlets.
                        Vertex.UV       = 0;
                        Vertex.Color    = 0xFFFFFFFF;
                        Vertex.JointIndices = FU8Vector4(0);
                        Vertex.JointWeights = FU8Vector4(0);
                        Vertex.Position.x = Attribute.vertices[3 * Index.vertex_index + 0];
                        Vertex.Position.y = Attribute.vertices[3 * Index.vertex_index + 1];
                        Vertex.Position.z = Attribute.vertices[3 * Index.vertex_index + 2];

                        if (Index.normal_index >= 0)
                        {
                            FVector3 Normal;
                            Normal.x = Attribute.normals[3 * Index.normal_index + 0];
                            Normal.y = Attribute.normals[3 * Index.normal_index + 1];
                            Normal.z = Attribute.normals[3 * Index.normal_index + 2];
                            Vertex.Normal = PackNormal(Math::Normalize(Normal));
                        }

                        if (Index.texcoord_index >= 0)
                        {
                            Vertex.UV = Math::PackHalf2x16(FVector2(Attribute.texcoords[2 * Index.texcoord_index + 0], Attribute.texcoords[2 * Index.texcoord_index + 1]));
                        }

                        if (bIsSkinned)
                        {
                            MeshResource->AppendVertex(Vertex);
                        }
                        else
                        {
                            MeshResource->AppendVertex(static_cast<const FVertex&>(Vertex));
                        }
                    }
                }
            }

            if (Progress)
            {
                ++ShapesDone;
                FFixedString Msg(FFixedString::CtorSprintf(), "Reading geometry (%u/%u shapes)...", ShapesDone, (uint32)Shapes.size());
                Progress->EnterProgressFrame(ShapeStep, Msg);
            }
        }

        // Skip the heavy passes when the dialog has asked for a raw preview
        // parse; FinalizeMeshImportData runs them at commit time.
        if (!ImportOptions.bSkipFinalization)
        {
            if (ImportOptions.bOptimize)
            {
                OptimizeNewlyImportedMesh(*MeshResource);
            }
            GenerateMeshlets(*MeshResource);
        }
        AnalyzeMeshStatistics(*MeshResource, ImportData.MeshStatistics);

        ImportData.Resources.push_back(Move(MeshResource));
        
        return Move(ImportData);
    }
}

