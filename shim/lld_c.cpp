// The C interface to lld, which ships none of its own.
//
// Everything here is one call into lld with the C++ taken off it. Nothing is
// combined, nothing is given a meaning lld has not got, and the file is as
// short as that discipline allows - what it is worth is that a reader can see
// it is a wrapper and not a layer.
//
// Which drivers are compiled in is settled by CMake, which looks for the lld
// libraries the LLVM beside it actually has. A build configured without the
// WebAssembly driver gets a library that says so through LLDHasFlavor rather
// than one that fails to link.

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

#include "lld-c/Driver.h"

// A driver named here has to be linked in, which is what the lld libraries
// CMake found are for. These are guarded because a driver LLVM was built
// without has no library to link and no symbol to declare.
#if LLD_C_HAS_COFF
LLD_HAS_DRIVER(coff)
#endif
#if LLD_C_HAS_ELF
LLD_HAS_DRIVER(elf)
#endif
#if LLD_C_HAS_MINGW
LLD_HAS_DRIVER(mingw)
#endif
#if LLD_C_HAS_MACHO
LLD_HAS_DRIVER(macho)
#endif
#if LLD_C_HAS_WASM
LLD_HAS_DRIVER(wasm)
#endif

namespace
{
    /// One link at a time, however many callers there are.
    ///
    /// lld keeps state for the duration of a link that is not per call - the
    /// configuration it is reading, the symbol table it is filling - and two
    /// links running at once in one process write over each other's and take
    /// the process down with them. It is safe to call again, which is not the
    /// same as safe to call twice at once, and the difference is not visible
    /// from the outside until something crashes.
    ///
    /// Serialising here rather than leaving it to whoever calls means every
    /// caller gets the guarantee, including the ones that never thought to ask
    /// for it: a build running its projects in parallel, or an editor building
    /// while it answers questions about something else.
    std::mutex oneLinkAtATime;

    /// Every driver this library was built with, in the order they are offered.
    const std::vector<lld::DriverDef>& driversBuiltIn()
    {
        static const std::vector<lld::DriverDef> drivers = {
#if LLD_C_HAS_COFF
            { lld::WinLink, &lld::coff::link },
#endif
#if LLD_C_HAS_ELF
            { lld::Gnu, &lld::elf::link },
#endif
#if LLD_C_HAS_MINGW
            { lld::MinGW, &lld::mingw::link },
#endif
#if LLD_C_HAS_MACHO
            { lld::Darwin, &lld::macho::link },
#endif
#if LLD_C_HAS_WASM
            { lld::Wasm, &lld::wasm::link },
#endif
        };

        return drivers;
    }

    /// The one driver of that flavour, or null where this build has none.
    lld::Driver driverFor(LLDFlavor flavor)
    {
        for (const auto& one : driversBuiltIn())
        {
            if (static_cast<int>(one.f) == static_cast<int>(flavor))
            {
                return one.d;
            }
        }

        return nullptr;
    }

    /// A copy of what lld wrote, in memory the caller hands back to
    /// LLDDisposeMessage.
    ///
    /// Copied rather than handed over because the string it came from belongs
    /// to the call that is about to end. Nothing written means nothing to hand
    /// back, which is what a link with nothing to say gives.
    char* copyOf(const std::string& text)
    {
        if (text.empty())
        {
            return nullptr;
        }

        auto size = text.size() + 1;
        auto copy = static_cast<char*>(std::malloc(size));

        if (copy == nullptr)
        {
            return nullptr;
        }

        std::memcpy(copy, text.c_str(), size);

        return copy;
    }

    /// Where a caller asked for one stream, both are gathered into it; where it
    /// asked for two, they are kept apart.
    ///
    /// One string is the order the linker wrote in, which is what explains a
    /// failure. Two are what a caller wants that means to tell a diagnostic
    /// from ordinary output.
    struct Written
    {
        std::string out;
        std::string err;
        bool together;

        explicit Written(char** standardError) : together(standardError == nullptr)
        {
        }

        std::string& errorsGoTo()
        {
            return together ? out : err;
        }

        void handOver(char** standardOutput, char** standardError)
        {
            if (standardOutput != nullptr)
            {
                *standardOutput = copyOf(out);
            }

            if (standardError != nullptr)
            {
                *standardError = copyOf(err);
            }
        }
    };

    LLDResult nothingToLink(char** standardOutput, char** standardError, const char* why)
    {
        Written written(standardError);

        written.errorsGoTo() = why;
        written.handOver(standardOutput, standardError);

        return { 1, 1 };
    }
}

extern "C"
{
    unsigned LLDCountFlavors(void)
    {
        return static_cast<unsigned>(driversBuiltIn().size());
    }

    LLDFlavor LLDFlavorAtIndex(unsigned Index)
    {
        const auto& drivers = driversBuiltIn();

        if (Index >= drivers.size())
        {
            return LLDFlavorInvalid;
        }

        return static_cast<LLDFlavor>(drivers[Index].f);
    }

    int LLDHasFlavor(LLDFlavor Flavor)
    {
        return driverFor(Flavor) != nullptr ? 1 : 0;
    }

    const char* LLDNameOfFlavor(LLDFlavor Flavor)
    {
        // The names '-flavor' takes, which is what makes them worth handing
        // out: a caller can put one straight back on a command line.
        switch (Flavor)
        {
            case LLDFlavorGnu: return "gnu";
            case LLDFlavorMinGW: return "mingw";
            case LLDFlavorWinLink: return "link";
            case LLDFlavorDarwin: return "darwin";
            case LLDFlavorWasm: return "wasm";
            default: return "invalid";
        }
    }

    const char* LLDVersion(void)
    {
        // Handed in by CMake rather than read from lld, and not for want of
        // trying: lld::getLLDVersion() lives behind lld/Common/Version.inc,
        // which is generated into the build tree and **never installed**, so an
        // installed prefix cannot reach it. What is handed in is the version of
        // the LLVM this was built against, which is the same number - lld is
        // released with LLVM and shares its version.
        return LLD_C_VERSION;
    }

    LLDResult LLDLinkAsProgram(int ArgumentCount, const char* const* Arguments,
                               char** StandardOutput, char** StandardError)
    {
        if (StandardOutput != nullptr) { *StandardOutput = nullptr; }
        if (StandardError != nullptr) { *StandardError = nullptr; }

        if (ArgumentCount <= 0 || Arguments == nullptr)
        {
            return nothingToLink(StandardOutput, StandardError, "the linker was given no arguments");
        }

        std::lock_guard<std::mutex> onlyOne(oneLinkAtATime);

        Written written(StandardError);

        llvm::raw_string_ostream toOutput(written.out);
        llvm::raw_string_ostream toErrors(written.errorsGoTo());

        auto result = lld::lldMain(
            llvm::ArrayRef<const char*>(Arguments, static_cast<size_t>(ArgumentCount)),
            toOutput, toErrors, driversBuiltIn());

        toOutput.flush();
        toErrors.flush();

        written.handOver(StandardOutput, StandardError);

        return { result.retCode, result.canRunAgain ? 1 : 0 };
    }

    LLDResult LLDLinkWithFlavor(LLDFlavor Flavor, int ArgumentCount,
                                const char* const* Arguments, int ExitEarly,
                                int DisableOutput, char** StandardOutput,
                                char** StandardError)
    {
        if (StandardOutput != nullptr) { *StandardOutput = nullptr; }
        if (StandardError != nullptr) { *StandardError = nullptr; }

        if (ArgumentCount <= 0 || Arguments == nullptr)
        {
            return nothingToLink(StandardOutput, StandardError, "the linker was given no arguments");
        }

        auto driver = driverFor(Flavor);

        if (driver == nullptr)
        {
            return nothingToLink(StandardOutput, StandardError, "this lld was built without that driver");
        }

        std::lock_guard<std::mutex> onlyOne(oneLinkAtATime);

        Written written(StandardError);

        llvm::raw_string_ostream toOutput(written.out);
        llvm::raw_string_ostream toErrors(written.errorsGoTo());

        auto worked = driver(
            llvm::ArrayRef<const char*>(Arguments, static_cast<size_t>(ArgumentCount)),
            toOutput, toErrors, ExitEarly != 0, DisableOutput != 0);

        toOutput.flush();
        toErrors.flush();

        written.handOver(StandardOutput, StandardError);

        // A driver called this way hands back whether it worked and nothing
        // else. There is no crash recovery on this route, so there is nothing
        // that could say it cannot be run again.
        return { worked ? 0 : 1, 1 };
    }

    void LLDDisposeMessage(char* Message)
    {
        std::free(Message);
    }

    int LLDHasContext(void)
    {
        return lld::hasContext() ? 1 : 0;
    }

    void LLDDestroyContext(void)
    {
        if (lld::hasContext())
        {
            lld::CommonLinkerContext::destroy();
        }
    }
}
