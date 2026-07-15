// Ubuntu 24.04's static libstdc++ (gcc-13-aarch64-linux-gnu) was compiled
// against glibc 2.39 headers where <stdlib.h> redirects strtoul() to
// __isoc23_strtoul() in C23/C++23 translation units.  That symbol is only
// present in glibc ≥ 2.38, while the pistomp (Debian Bookworm) ships glibc
// 2.36.
//
// By defining __isoc23_strtoul here with hidden visibility the linker
// satisfies the static libstdc++'s undefined reference locally at link time,
// so no GLIBC_2.38 entry ends up in the .so's version-need table.
// The behaviour is identical to strtoul(); the difference only concerns
// the C23 "0o755" octal syntax that libstdc++ never passes to strtoul.
#include <cstdlib>

extern "C"
__attribute__((visibility("hidden")))
unsigned long __isoc23_strtoul(const char* nptr, char** endptr, int base) noexcept
{
    return std::strtoul(nptr, endptr, base);
}
