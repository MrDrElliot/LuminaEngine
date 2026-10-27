#pragma once

#include "Containers/Span.h"
#include "Containers/String.h"

namespace Lumina::RmlKnowledge
{
    struct FRmlElement
    {
        const char* Name = "";
        const char* Documentation = "";
        // The attributes this element reads beyond the global ones, separated by spaces.
        const char* Attributes = "";
        bool        bVoid = false;
        // Scrollbar and slider parts RmlUi generates, which RCSS styles but markup never writes.
        bool        bStyleOnly = false;
    };

    struct FRmlAttribute
    {
        const char* Name = "";
        // The element this definition belongs to, empty for one every element accepts.
        const char* Element = "";
        const char* Documentation = "";
        // The values it accepts, separated by spaces, empty when free form.
        const char* Values = "";
        bool        bBoolean = false;
    };

    namespace EValue
    {
        constexpr uint32 Length     = 1 << 0;
        constexpr uint32 Percent    = 1 << 1;
        constexpr uint32 Number     = 1 << 2;
        constexpr uint32 Color      = 1 << 3;
        constexpr uint32 String     = 1 << 4;
        constexpr uint32 Decorator  = 1 << 5;
        constexpr uint32 FontEffect = 1 << 6;
        constexpr uint32 Filter     = 1 << 7;
        constexpr uint32 Transform  = 1 << 8;
        constexpr uint32 Time       = 1 << 9;
        constexpr uint32 Shadow     = 1 << 10;
        constexpr uint32 Tween      = 1 << 11;
        constexpr uint32 Property   = 1 << 12;
        constexpr uint32 Angle      = 1 << 13;
    }

    struct FRmlProperty
    {
        const char* Name = "";
        // Keywords it accepts, separated by spaces.
        const char* Keywords = "";
        uint32      Values = 0;
        const char* Syntax = "";
        const char* Documentation = "";
    };

    struct FRmlFunction
    {
        const char* Name = "";
        const char* Signature = "";
        const char* Documentation = "";
        // The EValue kind this function produces, so only fitting functions are offered for a property.
        uint32      Kind = 0;
    };

    struct FRmlNamed
    {
        const char* Name = "";
        const char* Documentation = "";
        // What completion inserts in snippet syntax, empty for the name alone.
        const char* Insert = "";
    };

    struct FRmlNamedColor
    {
        const char* Name = "";
        uint8       R = 0;
        uint8       G = 0;
        uint8       B = 0;
        uint8       A = 255;
    };

    TSpan<const FRmlElement>    Elements();
    TSpan<const FRmlAttribute>  Attributes();
    TSpan<const FRmlNamed>      Events();
    TSpan<const FRmlProperty>   Properties();
    TSpan<const FRmlFunction>   Functions();
    TSpan<const FRmlNamed>      PseudoClasses();
    TSpan<const FRmlNamed>      AtRules();
    TSpan<const FRmlNamed>      Units();
    TSpan<const FRmlNamed>      Entities();
    TSpan<const FRmlNamedColor> Colors();
    TSpan<const char* const>    Tweens();
    TSpan<const FRmlNamed>      DocumentSnippets();
    TSpan<const FRmlNamed>      StyleSnippets();

    const FRmlElement*   FindElement(FStringView Name);
    // Prefers the definition specific to Element over the global one.
    const FRmlAttribute* FindAttribute(FStringView Element, FStringView Name);
    const FRmlNamed*     FindNamed(TSpan<const FRmlNamed> Table, FStringView Name);
    bool                 ContainsWord(const char* SpaceSeparated, FStringView Word);
}
