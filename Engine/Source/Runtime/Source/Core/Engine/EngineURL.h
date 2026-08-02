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

        bool IsClient() const { return !Host.empty(); }

        // Parse "192.168.1.5:7777" | "/Game/Maps/Foo?listen?port=7777". A token starting with '/' is the Map;
        // otherwise it's host[:port]. Options after '?': "listen", "port=N".
        static FURL Parse(FStringView Str)
        {
            FURL URL;
            FString S(Str.data(), Str.size());

            // Split off options on the first '?'.
            FString Main = S;
            FString Options;
            const SIZE_T Q = S.find('?');
            if (Q != FString::npos)
            {
                Main    = S.substr(0, Q);
                Options = S.substr(Q + 1);
            }

            if (!Main.empty() && Main[0] == '/')
            {
                URL.Map = Main;
            }
            else if (!Main.empty())
            {
                const SIZE_T Colon = Main.find(':');
                if (Colon != FString::npos)
                {
                    URL.Host = Main.substr(0, Colon);
                    URL.Port = static_cast<uint16>(atoi(Main.substr(Colon + 1).c_str()));
                }
                else
                {
                    URL.Host = Main;
                }
            }

            // Walk '?'-separated options.
            SIZE_T Cursor = 0;
            while (Cursor < Options.size())
            {
                SIZE_T Next = Options.find('?', Cursor);
                if (Next == FString::npos) { Next = Options.size(); }
                const FString Opt = Options.substr(Cursor, Next - Cursor);
                Cursor = Next + 1;

                if (Opt == "listen")
                {
                    URL.bListen = true;
                }
                else if (Opt.size() > 5 && Opt.substr(0, 5) == "port=")
                {
                    URL.Port = static_cast<uint16>(atoi(Opt.substr(5).c_str()));
                }
            }

            return URL;
        }
    };
}
