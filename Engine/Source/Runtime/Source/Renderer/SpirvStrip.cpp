#include "RuntimePCH.h"
#include "SpirvStrip.h"

#include "Containers/HashTable.h"

namespace Lumina::Spirv
{
    namespace
    {
        constexpr uint32 kHeaderWords = 5;

        enum EOp : uint32
        {
            OpSourceContinued     = 2,
            OpSource              = 3,
            OpSourceExtension     = 4,
            OpName                = 5,
            OpMemberName          = 6,
            OpString              = 7,
            OpLine                = 8,
            OpExtension           = 10,
            OpExtInstImport       = 11,
            OpExtInst             = 12,
            OpDecorate            = 71,
            OpMemberDecorate      = 72,
            OpNoLine              = 317,
            OpModuleProcessed     = 330,
            OpDecorateId          = 332,
            OpDecorateString      = 5632,
            OpMemberDecorateString = 5633,
        };

        struct FInstruction
        {
            uint32 Op;
            uint32 Offset;
            uint32 Count;
        };

        FStringView LiteralAt(const TVector<uint32>& Words, uint32 Offset, uint32 End)
        {
            const char* Begin = reinterpret_cast<const char*>(Words.data() + Offset);
            const size_t MaxLength = (End - Offset) * sizeof(uint32);
            size_t Length = 0;
            while (Length < MaxLength && Begin[Length] != '\0')
            {
                ++Length;
            }
            return FStringView(Begin, Length);
        }

        bool IsDecoration(uint32 Op)
        {
            return Op == OpName || Op == OpMemberName || Op == OpDecorate || Op == OpMemberDecorate
                || Op == OpDecorateId || Op == OpDecorateString || Op == OpMemberDecorateString;
        }
    }

    void StripDebugInfo(TVector<uint32>& Words)
    {
        if (Words.size() <= kHeaderWords || Words[0] != 0x07230203u)
        {
            return;
        }

        TVector<FInstruction> Instructions;
        for (uint32 Offset = kHeaderWords; Offset < (uint32)Words.size();)
        {
            const uint32 Count = Words[Offset] >> 16;
            if (Count == 0 || Offset + Count > (uint32)Words.size())
            {
                // A malformed stream is shipped as it came rather than guessed at.
                return;
            }
            Instructions.push_back({ Words[Offset] & 0xFFFFu, Offset, Count });
            Offset += Count;
        }

        THashSet<uint32> DebugSets;
        for (const FInstruction& Inst : Instructions)
        {
            if (Inst.Op == OpExtInstImport && Inst.Count > 2
                && LiteralAt(Words, Inst.Offset + 2, Inst.Offset + Inst.Count).starts_with("NonSemantic.Shader.DebugInfo"))
            {
                DebugSets.insert(Words[Inst.Offset + 1]);
            }
        }

        THashSet<uint32> RemovedIds;
        TVector<FInstruction> Kept;
        Kept.reserve(Instructions.size());
        for (const FInstruction& Inst : Instructions)
        {
            switch (Inst.Op)
            {
                case OpSourceContinued:
                case OpSource:
                case OpSourceExtension:
                case OpLine:
                case OpNoLine:
                case OpModuleProcessed:
                    continue;
                case OpExtInstImport:
                    if (DebugSets.contains(Words[Inst.Offset + 1]))
                    {
                        RemovedIds.insert(Words[Inst.Offset + 1]);
                        continue;
                    }
                    break;
                case OpExtInst:
                    if (Inst.Count > 3 && DebugSets.contains(Words[Inst.Offset + 3]))
                    {
                        RemovedIds.insert(Words[Inst.Offset + 2]);
                        continue;
                    }
                    break;
                default:
                    break;
            }
            Kept.push_back(Inst);
        }

        // A string survives only while something other than a name still points at it, such as a printf format.
        THashSet<uint32> Referenced;
        for (const FInstruction& Inst : Kept)
        {
            if (Inst.Op != OpString && Inst.Op != OpName && Inst.Op != OpMemberName)
            {
                for (uint32 Word = 1; Word < Inst.Count; ++Word)
                {
                    Referenced.insert(Words[Inst.Offset + Word]);
                }
            }
        }

        bool bAnyNonSemantic = false;
        for (const FInstruction& Inst : Kept)
        {
            if (Inst.Op == OpExtInstImport && LiteralAt(Words, Inst.Offset + 2, Inst.Offset + Inst.Count).starts_with("NonSemantic"))
            {
                bAnyNonSemantic = true;
            }
        }

        TVector<uint32> Out;
        Out.reserve(Words.size());
        Out.insert(Out.end(), Words.begin(), Words.begin() + kHeaderWords);
        for (const FInstruction& Inst : Kept)
        {
            if (Inst.Op == OpString && !Referenced.contains(Words[Inst.Offset + 1]))
            {
                continue;
            }
            if (IsDecoration(Inst.Op) && RemovedIds.contains(Words[Inst.Offset + 1]))
            {
                continue;
            }
            if (Inst.Op == OpExtension && !bAnyNonSemantic
                && LiteralAt(Words, Inst.Offset + 1, Inst.Offset + Inst.Count) == FStringView("SPV_KHR_non_semantic_info"))
            {
                continue;
            }
            Out.insert(Out.end(), Words.begin() + Inst.Offset, Words.begin() + Inst.Offset + Inst.Count);
        }

        Words = Move(Out);
    }
}
