#include "Scripting/ScriptReturn.h"
#include "Scripting/DotNet/DotNetExport.h"
#include "Scripting/DotNet/ExportSignature.h"
#include "Containers/Vector.h"
#include "Memory/Memcpy.h"

using namespace Lumina;

namespace
{
    // Keyed by the caller's scratch buffer, which is unique to the one managed call that overflowed it.
    struct FCallOverflow
    {
        const void*    Scratch = nullptr;
        TVector<uint8> Bytes;
    };

    thread_local FCallOverflow GCallOverflow;
}

void Lumina::Scripting::StashCallOverflow(const void* CallerScratch, const void* Data, size_t Bytes)
{
    GCallOverflow.Scratch = CallerScratch;
    GCallOverflow.Bytes.SetNum(IntCastChecked<int32>(Bytes));
    if (Bytes > 0)
    {
        Memory::Memcpy(GCallOverflow.Bytes.GetData(), Data, Bytes);
    }
}

// Returns the bytes copied, or -1 when the call that overflowed this scratch left nothing, so the caller calls again.
LUMINA_DOTNET_EXPORT(int32, TakeCallOverflow)(const void* CallerScratch, void* Dest, int32 Bytes)
{
    if (CallerScratch == nullptr || GCallOverflow.Scratch != CallerScratch || Dest == nullptr || Bytes < 0)
    {
        return -1;
    }

    const int32 Stashed = GCallOverflow.Bytes.Num();
    const int32 Copied = Bytes < Stashed ? Bytes : Stashed;
    if (Copied > 0)
    {
        Memory::Memcpy(Dest, GCallOverflow.Bytes.GetData(), (size_t)Copied);
    }

    GCallOverflow.Scratch = nullptr;
    GCallOverflow.Bytes.Reset();
    return Copied;
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(TakeCallOverflow)
);
