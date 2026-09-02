#include "RuntimePCH.h"
#include "Memory/Construct.h"
#include "Fiber.h"

#ifdef LE_PLATFORM_LINUX

#include <cstdint>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#include "Core/Assertions/Assert.h"
#include "Memory/Memory.h"


extern "C"
{
    void LuminaFiberSwitch(void** SaveStackPointer, void* NewStackPointer);

    void LuminaFiberTrampoline();

    void LuminaFiberEnter();
}

asm(R"(
    .text

    /* void LuminaFiberSwitch(void** SaveStackPointer, void* NewStackPointer)
       rdi = SaveStackPointer, rsi = NewStackPointer                            */
    .globl  LuminaFiberSwitch
    .hidden LuminaFiberSwitch
    .type   LuminaFiberSwitch,@function
    .align  16
LuminaFiberSwitch:
    .cfi_startproc
    pushq   %rbp
    .cfi_adjust_cfa_offset 8
    pushq   %rbx
    .cfi_adjust_cfa_offset 8
    pushq   %r12
    .cfi_adjust_cfa_offset 8
    pushq   %r13
    .cfi_adjust_cfa_offset 8
    pushq   %r14
    .cfi_adjust_cfa_offset 8
    pushq   %r15
    .cfi_adjust_cfa_offset 8

    /* 8 bytes of FP control state: MXCSR at +0, x87 control word at +4. */
    subq    $8, %rsp
    .cfi_adjust_cfa_offset 8
    stmxcsr (%rsp)
    fnstcw  4(%rsp)

    movq    %rsp, (%rdi)        /* *SaveStackPointer = rsp   */
    movq    %rsi, %rsp          /* rsp = NewStackPointer     */

    ldmxcsr (%rsp)
    fldcw   4(%rsp)
    addq    $8, %rsp
    .cfi_adjust_cfa_offset -8

    popq    %r15
    .cfi_adjust_cfa_offset -8
    popq    %r14
    .cfi_adjust_cfa_offset -8
    popq    %r13
    .cfi_adjust_cfa_offset -8
    popq    %r12
    .cfi_adjust_cfa_offset -8
    popq    %rbx
    .cfi_adjust_cfa_offset -8
    popq    %rbp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
    .size   LuminaFiberSwitch,.-LuminaFiberSwitch

    /* Reached by the ret at the end of the first switch into a new fiber. */
    .globl  LuminaFiberTrampoline
    .hidden LuminaFiberTrampoline
    .type   LuminaFiberTrampoline,@function
    .align  16
LuminaFiberTrampoline:
    .cfi_startproc
    /* This frame has no caller. Telling the unwinder the return address is unrecoverable stops a
       backtrace cleanly at the base of the fiber instead of walking off into whatever the freshly
       mmap'd stack happens to contain. */
    .cfi_undefined %rip
    /* Align unconditionally rather than relying on how the stack was primed. Nothing here ever
       returns, so there is no saved rsp to preserve. */
    andq    $-16, %rsp
    call    LuminaFiberEnter
    /* LuminaFiberEnter must not return; trap loudly if it ever does. */
    ud2
    .cfi_endproc
    .size   LuminaFiberTrampoline,.-LuminaFiberTrampoline

    /* Mark the stack non-executable. Without this the linker sees a translation unit with no
       .note.GNU-stack and conservatively marks the whole image's stack executable.

       push/pop rather than a bare .section: a plain .section leaves the assembler sitting in
       .note.GNU-stack, so everything the compiler emits after this block lands there too. That
       fails the link with "defined in discarded section .note.GNU-stack" -- and only once the
       optimizer changes what gets emitted after the block, so an -O0 build hides it. */
    .pushsection .note.GNU-stack,"",@progbits
    .popsection
)");

namespace Lumina::Fibers
{
    namespace
    {
        struct FLinuxFiber
        {
            void*       StackPointer = nullptr;

            void*       Mapping      = nullptr;
            size_t      MappingSize  = 0;

            FFiberEntry Entry        = nullptr;
            void*       Arg          = nullptr;
        };

        thread_local FLinuxFiber* GCurrentFiber = nullptr;

        size_t PageSize()
        {
            static const size_t Size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
            return Size;
        }

        size_t RoundUpToPage(size_t Value)
        {
            const size_t Page = PageSize();
            return (Value + Page - 1) / Page * Page;
        }

        uint64 CaptureFloatingPointControl()
        {
            uint32 Mxcsr = 0;
            uint16 ControlWord = 0;

            asm volatile("stmxcsr %0" : "=m"(Mxcsr));
            asm volatile("fnstcw %0" : "=m"(ControlWord));

            return static_cast<uint64>(Mxcsr) | (static_cast<uint64>(ControlWord) << 32);
        }
    }

    FFiber ThreadToFiber()
    {
        if (GCurrentFiber != nullptr)
        {
            return GCurrentFiber;
        }

        FLinuxFiber* Fiber = static_cast<FLinuxFiber*>(
            Memory::Malloc(sizeof(FLinuxFiber), alignof(FLinuxFiber)));

        ASSERT(Fiber != nullptr);

        Memory::ConstructAt(Fiber);

        GCurrentFiber = Fiber;

        return Fiber;
    }

    void FiberToThread()
    {
        FLinuxFiber* Fiber = GCurrentFiber;

        if (Fiber == nullptr)
        {
            return;
        }

        ASSERT(Fiber->Mapping == nullptr);

        GCurrentFiber = nullptr;

        void* Memory = Fiber;
        Memory::Free(Memory);
    }

    FFiber Create(size_t StackSize, FFiberEntry Entry, void* Arg)
    {
        ASSERT(Entry != nullptr);

        const size_t Page = PageSize();
        const size_t UsableSize = RoundUpToPage(StackSize);

        const size_t MappingSize = UsableSize + Page;

        void* Mapping = ::mmap(nullptr, MappingSize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);

        if (Mapping == MAP_FAILED)
        {
            ASSERT(false);
            return nullptr;
        }

        if (::mprotect(Mapping, Page, PROT_NONE) != 0)
        {
            ::munmap(Mapping, MappingSize);
            ASSERT(false);
            return nullptr;
        }

        FLinuxFiber* Fiber = static_cast<FLinuxFiber*>(
            Memory::Malloc(sizeof(FLinuxFiber), alignof(FLinuxFiber)));

        ASSERT(Fiber != nullptr);

        Memory::ConstructAt(Fiber);
        Fiber->Mapping     = Mapping;
        Fiber->MappingSize = MappingSize;
        Fiber->Entry       = Entry;
        Fiber->Arg         = Arg;

        uint8* Top = static_cast<uint8*>(Mapping) + MappingSize;
        Top -= reinterpret_cast<uintptr_t>(Top) & 15u;   // 16-align downward

        uint64* Frame = reinterpret_cast<uint64*>(Top) - 8;

        Frame[0] = CaptureFloatingPointControl();
        Frame[1] = 0;   // r15
        Frame[2] = 0;   // r14
        Frame[3] = 0;   // r13
        Frame[4] = 0;   // r12
        Frame[5] = 0;   // rbx
        Frame[6] = 0;   // zero terminates a frame-pointer walk at the fiber base
        Frame[7] = reinterpret_cast<uint64>(&LuminaFiberTrampoline);

        Fiber->StackPointer = Frame;

        return Fiber;
    }

    void Destroy(FFiber Fiber)
    {
        if (Fiber == nullptr)
        {
            return;
        }

        FLinuxFiber* Target = static_cast<FLinuxFiber*>(Fiber);

        ASSERT(Target != GCurrentFiber);

        if (Target->Mapping != nullptr)
        {
            ::munmap(Target->Mapping, Target->MappingSize);
        }

        void* Memory = Target;
        Memory::Free(Memory);
    }

    void Switch(FFiber Fiber)
    {
        FLinuxFiber* Target = static_cast<FLinuxFiber*>(Fiber);
        FLinuxFiber* Current = GCurrentFiber;

        ASSERT(Target != nullptr);
        ASSERT(Current != nullptr);   // the thread must be a fiber already; see ThreadToFiber

        if (Target == Current)
        {
            return;
        }

        GCurrentFiber = Target;

        LuminaFiberSwitch(&Current->StackPointer, Target->StackPointer);

    }

    FFiber Current()
    {
        return GCurrentFiber;
    }

    extern "C" void LuminaFiberEnter()
    {
        FLinuxFiber* Self = GCurrentFiber;

        ASSERT(Self != nullptr);
        ASSERT(Self->Entry != nullptr);

        Self->Entry(Self->Arg);

        LUMINA_PANIC("Fiber entry point returned. Fiber entries must switch away, never return.");
    }
}

#endif
