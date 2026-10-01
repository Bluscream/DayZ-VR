#include "crash_report.hpp"

#include "logging.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace dayz::crash_report
{
    namespace
    {
        std::uintptr_t g_gameBase{};
        std::uint32_t g_gameSize{};
        std::uintptr_t g_selfBase{};
        std::uintptr_t g_selfEnd{};
        ContextDumpFn g_contextDump{};
        std::atomic_int g_reportsLeft{3};
        std::atomic_bool g_installed{};

        bool IsFatal(DWORD code) noexcept
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
            case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / fast-fail
                return true;
            default:
                return false;
            }
        }

        std::string Describe(std::uintptr_t address)
        {
            char text[160]{};
            if (address >= g_gameBase && address < g_gameBase + g_gameSize)
            {
                sprintf_s(text, "DayZ+0x%llx",
                    static_cast<unsigned long long>(address - g_gameBase));
                return text;
            }
            HMODULE module{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module) && module)
            {
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(module, path, MAX_PATH);
                const wchar_t* name = wcsrchr(path, L'\\');
                name = name ? name + 1 : path;
                char narrow[64]{};
                for (int i = 0; i < 63 && name[i]; ++i)
                    narrow[i] = name[i] < 128 ? static_cast<char>(name[i]) : '?';
                sprintf_s(text, "%s+0x%llx", narrow, static_cast<unsigned long long>(
                    address - reinterpret_cast<std::uintptr_t>(module)));
                return text;
            }
            sprintf_s(text, "0x%llx", static_cast<unsigned long long>(address));
            return text;
        }

        void LogBacktrace(const CONTEXT& faultContext)
        {
            CONTEXT context = faultContext;
            for (int frame = 0; frame < 24 && context.Rip; ++frame)
            {
                std::string line = "  frame " + std::to_string(frame) + ' ' +
                    Describe(static_cast<std::uintptr_t>(context.Rip));
                logging::Error(line);
                DWORD64 imageBase{};
                RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &imageBase,
                    nullptr);
                if (!function)
                {
                    // Leaf function: return address is at [rsp].
                    if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64)))
                        break;
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += sizeof(DWORD64);
                    continue;
                }
                void* handlerData{};
                DWORD64 establisherFrame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                    &handlerData, &establisherFrame, nullptr);
            }
        }

        LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* pointers) noexcept
        {
            if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
                return EXCEPTION_CONTINUE_SEARCH;
            const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
            if (!IsFatal(record.ExceptionCode))
                return EXCEPTION_CONTINUE_SEARCH;
            const auto rip = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
            // The runtime probe reads engine memory under __try; those faults are expected.
            if (rip >= g_selfBase && rip < g_selfEnd)
                return EXCEPTION_CONTINUE_SEARCH;
            if (g_reportsLeft.fetch_sub(1, std::memory_order_acq_rel) <= 0)
                return EXCEPTION_CONTINUE_SEARCH;

            char header[256]{};
            const unsigned long long info0 = record.NumberParameters > 0 ?
                record.ExceptionInformation[0] : 0;
            const unsigned long long info1 = record.NumberParameters > 1 ?
                record.ExceptionInformation[1] : 0;
            sprintf_s(header, "Fatal exception 0x%08lX at %s tid=%lu %s address 0x%llx",
                record.ExceptionCode, Describe(rip).c_str(), GetCurrentThreadId(),
                record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                    ? (info0 == 0 ? "reading" : info0 == 1 ? "writing" : "executing")
                    : "info", info1);
            logging::Error(header);
            const CONTEXT& c = *pointers->ContextRecord;
            char regs[512]{};
            sprintf_s(regs,
                "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx "
                "r8=%016llx r9=%016llx r10=%016llx r11=%016llx r12=%016llx r13=%016llx "
                "r14=%016llx r15=%016llx rsp=%016llx rbp=%016llx",
                c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.R10, c.R11, c.R12,
                c.R13, c.R14, c.R15, c.Rsp, c.Rbp);
            logging::Error(regs);
            LogBacktrace(c);
            if (g_contextDump)
                g_contextDump();
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void Install(std::uintptr_t gameModuleBase, std::uint32_t gameImageSize,
        ContextDumpFn contextDump) noexcept
    {
        if (g_installed.exchange(true))
            return;
        g_gameBase = gameModuleBase;
        g_gameSize = gameImageSize;
        g_contextDump = contextDump;
        HMODULE self{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Install), &self) && self)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(self);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            g_selfBase = base;
            g_selfEnd = base + nt->OptionalHeader.SizeOfImage;
        }
        if (AddVectoredExceptionHandler(1, VectoredHandler))
            logging::Info("Crash reporter installed: fatal exceptions are logged with a DayZ+RVA backtrace");
        else
            logging::Error("Crash reporter: AddVectoredExceptionHandler failed");
    }
}
