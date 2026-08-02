#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Math/AABB.h"
#include "Containers/Array.h"
#include "Renderer/Vertex.h"
#include "GeometryCollection.generated.h"

namespace Lumina
{
    class CMesh;
    class CStaticMesh;
    class CMaterialInterface;

    /** One convex chunk of a fractured mesh: render geometry (also drives convex collision) + a local centroid. */
    struct FFracturePiece
    {
        TVector<FVertex> Vertices;
        TVector<uint32>  Indices;
        FVector3        Center = FVector3(0.0f);   // local-space centroid, used as the launch pivot
        FAABB            Bounds;

        friend FArchive& operator<<(FArchive& Ar, FFracturePiece& P)
        {
            Ar << P.Vertices;
            Ar << P.Indices;
            Ar << P.Center;
            Ar << P.Bounds.Min;
            Ar << P.Bounds.Max;
            return Ar;
        }
    };

    /** Baked fracture: the full set of pieces plus the bounds they were generated from. */
    struct FFractureData
    {
        TVector<FFracturePiece> Pieces;
        FAABB                   SourceBounds;

        FORCEINLINE bool IsEmpty() const { return Pieces.empty(); }

        friend FArchive& operator<<(FArchive& Ar, FFractureData& D)
        {
            Ar << D.Pieces;
            Ar << D.SourceBounds.Min;
            Ar << D.SourceBounds.Max;
            return Ar;
        }
    };

    struct FFractureSettings
    {
        /** Number of convex chunks to generate. */
        int32  NumPieces = 16;
        /** Deterministic seed for the seed-point scatter (same seed → same fracture). */
        uint32 Seed      = 1337u;
    };

    // Pre-fractured geometry asset: baked convex pieces of a source mesh, shattered into chunks at runtime.
    // Pure data; piece meshes are built on demand (Fracture::BuildPieceMesh), so it holds no GPU resources.
    REFLECT()
    class RUNTIME_API CGeometryCollection : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }
        void Serialize(FArchive& Ar) override;

        /** Pre-build the shared piece meshes so the first runtime fracture doesn't hitch. */
        void PostLoad() override;

        FORCEINLINE int32 GetNumPieces() const { return static_cast<int32>(Data.Pieces.size()); }
        FORCEINLINE const FFractureData& GetFractureData() const { return Data; }
        FORCEINLINE const FFracturePiece& GetPiece(int32 Index) const { return Data.Pieces[Index]; }

        /** Per-piece CStaticMesh built once and shared across every fracture (indices match Pieces);
         *  lets runtime fracture spawn fragments with zero mesh-build cost. */
        const TVector<TObjectPtr<CStaticMesh>>& GetPieceMeshes();

        /** Re-bake the pieces from SourceMesh using NumPieces/Seed and copy its materials. Returns the piece count. */
        int32 Rebuild();

        /** Bake convex Voronoi pieces from a source mesh's bounds. Returns a new collection owned by Outer. */
        static CGeometryCollection* GenerateFromMesh(CStaticMesh* Source, const FFractureSettings& Settings, CObject* Outer = nullptr);

        /** Source mesh whose bounds are fractured. Pick it in the editor, then Generate. */
        PROPERTY(Editable, Category = "Fracture")
        TObjectPtr<CStaticMesh> SourceMesh;

        /** Number of convex chunks produced by the next bake. */
        PROPERTY(Editable, ClampMin = 2, ClampMax = 512, Category = "Fracture")
        int32 NumPieces = 16;

        /** Deterministic fracture seed (same value reproduces the same break). */
        PROPERTY(Editable, Category = "Fracture")
        int32 Seed = 1337;

        /** Shared materials applied to each piece (copied from the source mesh at bake time). */
        PROPERTY(Editable, NoResize, Category = "Materials")
        TVector<TObjectPtr<CMaterialInterface>> Materials;

    private:

        /** Build PieceMeshes from Data.Pieces (no-op if already built). */
        void BuildPieceMeshes();

        FFractureData Data;   // serialized geometry blob (not a reflected property)

        /** Shared per-piece meshes, built once. Strong refs keep them alive; cleared on Rebuild. Not serialized. */
        TVector<TObjectPtr<CStaticMesh>> PieceMeshes;
    };

    namespace Fracture
    {
        // Convex Voronoi fracture: clip the mesh's convex hull by perpendicular bisectors between seed
        // points, so pieces follow the silhouette. Falls back to the AABB for degenerate/dense meshes.
        RUNTIME_API void GenerateConvexFracture(const CMesh* SourceMesh, const FFractureSettings& Settings, TVector<FFracturePiece>& OutPieces);

        /** Build a transient, render- and collision-ready CStaticMesh from one piece. */
        RUNTIME_API CStaticMesh* BuildPieceMesh(const FFracturePiece& Piece, const TVector<TObjectPtr<CMaterialInterface>>& Materials, const char* DebugName);
    }
}
