#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wparentheses"

  /* placing code in section(text) does not mark it executable with Clang. */
  #undef  LIBCO_MPROTECT
  #define LIBCO_MPROTECT
#endif

#if defined(__clang__) || defined(__GNUC__)
  /* Pick fiber.c on Windows BEFORE the arch-specific raw-asm options.
   * libco's amd64.c does save Win64 non-volatiles, but it doesn't update
   * the NT_TIB stack-base/stack-limit fields a fiber-switched stack
   * needs. Win32 SEH unwind reads those fields, and the MSVCRT abort
   * path goes through SEH — meaning any abort from inside a libco fiber
   * silently terminates the process with no stderr output. fiber.c uses
   * CreateFiber/SwitchToFiber, which Windows updates correctly. */
  #if defined(_WIN32)
    #include "fiber.c"
  #elif defined(__i386__)
    #include "x86.c"
  #elif defined(__amd64__)
    #include "amd64.c"
  #elif defined(__arm__)
    #include "arm.c"
  #elif defined(__aarch64__)
    #include "aarch64.c"
  #elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
    #include "ppc64v2.c"
  #elif defined(_ARCH_PPC) && !defined(__LITTLE_ENDIAN__)
    #include "ppc.c"
  #else
    #include "sjlj.c"
  #endif
#elif defined(_MSC_VER)
  #if defined(_M_IX86)
    #include "x86.c"
  #elif defined(_M_AMD64)
    #include "amd64.c"
  #else
    #include "fiber.c"
  #endif
#else
  #error "libco: unsupported processor, compiler or operating system"
#endif
