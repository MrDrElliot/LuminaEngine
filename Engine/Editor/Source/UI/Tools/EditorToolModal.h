#pragma once
#include "imgui.h"
#include "Containers/String.h"
#include "Core/UpdateContext.h"
#include "Containers/Function.h"
#include <Memory/SmartPtr.h>

namespace Lumina
{
    class FEditorToolModal;
    class FUpdateContext;
}

namespace Lumina
{

    class FEditorModalManager
    {
    public:
        
        void CreateDialogue(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction, bool bBlocking = true, bool bCloseable = true);

        FORCEINLINE bool HasModal() const { return ActiveModal.get() != nullptr; }

        void DrawDialogue();

    private:
        
        TUniquePtr<FEditorToolModal>       ActiveModal;
        
    };
    
    class FEditorToolModal
    {
    public:

        friend class FEditorModalManager;
        
        virtual ~FEditorToolModal() = default;

        FEditorToolModal(const FString& InTitle, ImVec2 InSize, bool bInClosable)
            : Title(InTitle)
            , Size(InSize)
            , bBlocking(false)
            , bCloseable(bInClosable)
        {
        }

        /** Return true to indicate the modal is ready to close */
        bool DrawModal()
        {
            return DrawFunction();
        }
        
    protected:

        TMoveOnlyFunction<bool()>   DrawFunction;
        FString                     Title;
        ImVec2                      Size;
        ImVec2                      OpenPos = ImVec2(-FLT_MAX, -FLT_MAX);
        bool                        bBlocking;
        bool                        bCloseable = true;
        bool                        bOpen = true;
    };
}
