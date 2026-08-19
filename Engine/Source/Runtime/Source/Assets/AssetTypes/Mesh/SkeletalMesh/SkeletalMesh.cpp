#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "SkeletalMesh.h"

namespace Lumina
{
    void CSkeletalMesh::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Meshes");
        CMesh::Serialize(Ar);
    }
}
