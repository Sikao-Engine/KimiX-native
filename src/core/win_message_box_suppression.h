#pragma once

// ---------------------------------------------------------------------------
// Win32 message-box suppression
//
// When KIMIX_DISABLE_WIN_MESSAGE_BOX is defined, this header installs a
// global-initializer that redirects CRT assertions, runtime errors, and
// critical-error dialogs to stderr instead of showing a pop-up message box.
//
// The global is declared as an inline variable (C++17) so that every
// translation unit that includes this header provides a definition; the
// linker picks one and the constructor runs exactly once before main().
// This ensures the suppression is active regardless of which object files
// the linker pulls in from the kimix-core static library.
// ---------------------------------------------------------------------------

#ifdef KIMIX_DISABLE_WIN_MESSAGE_BOX
#ifdef KIMIX_PLATFORM_WINDOWS

#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>

namespace kimix {
namespace detail {

struct DisableMessageBoxInit {
    DisableMessageBoxInit() noexcept {
#ifndef NDEBUG
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
        _set_error_mode(_OUT_TO_STDERR);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
};

/// Inline variable — one definition selected by linker, ctor runs once before main().
inline DisableMessageBoxInit g_disable_message_box;

} // namespace detail
} // namespace kimix

#endif // KIMIX_PLATFORM_WINDOWS
#endif // KIMIX_DISABLE_WIN_MESSAGE_BOX
