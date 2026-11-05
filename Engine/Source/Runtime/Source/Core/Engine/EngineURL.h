#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // A connection / level-open target.
    struct FURL
    {
        FString Map;            // world asset path, e.g. "/Game/Maps/NewWorld"
        FString Host;           // remote host; empty => local (host / standalone)
        uint16  Port      = 7777;
        bool    bListen    = false;
        bool    bDedicated = false; // listen as a clientless, non-rendered dedicated server

        bool IsClient() const { return !Host.IsEmpty(); }

        // Parse "192.168.1.5:7777" | "/Game/Maps/Foo?listen?port=7777". A token starting with '/' is the Map;
        // otherwise it's host[:port]. Options after '?': "listen", "port=N".
        static FURL Parse(FStringView Str)
        {
            FURL URL;
            FString S(Str.GetData(), Str.Len());

            // Split off options on the first '?'.
            FString Main = S;
            FString Options;
            const int32 Q = S.Find('?');
            if (Q != Constants::kIndexNone)
            {
                Main    = S.Mid(0, Q);
                Options = S.Mid(Q + 1);
            }

            if (!Main.IsEmpty() && Main[0] == '/')
            {
                URL.Map = Main;
            }
            else if (!Main.IsEmpty())
            {
                const int32 Colon = Main.Find(':');
                if (Colon != Constants::kIndexNone)
                {
                    URL.Host = Main.Mid(0, Colon);
                    URL.Port = static_cast<uint16>(atoi(*Main.Mid(Colon + 1)));
                }
                else
                {
                    URL.Host = Main;
                }
            }

            // Walk '?'-separated options.
            int32 Cursor = 0;
            while (Cursor < Options.Len())
            {
                int32 Next = Options.Find('?', Cursor);
                if (Next == Constants::kIndexNone) { Next = Options.Len(); }
                const FString Opt = Options.Mid(Cursor, Next - Cursor);
                Cursor = Next + 1;

                if (Opt == "listen")
                {
                    URL.bListen = true;
                }
                else if (Opt.Len() > 5 && Opt.Mid(0, 5) == "port=")
                {
                    URL.Port = static_cast<uint16>(atoi(*Opt.Mid(5)));
                }
            }

            return URL;
        }
    };
}
