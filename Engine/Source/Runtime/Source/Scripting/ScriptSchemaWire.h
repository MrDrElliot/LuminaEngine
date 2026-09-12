#pragma once

#include "Platform/GenericPlatform.h"

// The one definition of the script schema wire, shared by the managed writer (LuminaSharp.Serializer) and the
// native reader. Both sides are hand-written, so what keeps them honest is that a mismatch is LOUD: the header
// is checked before a byte of payload is read, and every record is length-delimited so a reader that falls
// behind resyncs at the next boundary instead of misreading the rest of the buffer as garbage.
namespace Lumina::Scripting
{
    //~ A VALUE node is deliberately unframed. It is the one part of this wire both sides read AND write
    //~ (native pushes instance overrides into managed, managed writes schema defaults out), so framing it
    //~ would double the surface for a taxonomy that is closed and does not evolve. A value always sits
    //~ inside a framed field, which is what still resyncs the reader if one ever does change.

    /** 'LSCH'. Identifies a schema buffer, so a stale or foreign one is refused rather than parsed. */
    inline constexpr uint32 kScriptSchemaMagic = 0x4843534Cu;

    /**
     * Bumped only when an existing field changes meaning, type or position.
     *
     * Appending a field to the END of a record does NOT need a bump: records carry their byte length, so an
     * older reader skips what it does not recognise and continues. That is the whole point of the framing --
     * the format used to be positional, which made every addition a lockstep edit on both sides and a silent
     * cursor desync when they drifted.
     */
    inline constexpr uint16 kScriptSchemaVersion = 1;

    /**
     * Records, each written as [u8 Tag][u32 ByteLength][payload].
     *
     * The tag is what turns a drift into an immediate, named failure: a reader that expected a Type and finds
     * a Value says so, instead of reading the next four bytes as a length and walking off into the buffer.
     */
    enum class EScriptSchemaRecord : uint8
    {
        Field = 0,      ///< one [Property] member: name, aliases, metadata, type, default
        Meta,           ///< the editor-facing hints on a field, which is the bag that grows most
        Type,           ///< one FScriptExportType node, recursive
        Candidate,      ///< one instanced-struct candidate
    };
}
