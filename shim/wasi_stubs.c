// What LLVM mentions that WASI has not got, answered the way a machine without
// it would answer.
//
// LLVM's support library can load shared libraries and advise the system about
// memory it has mapped. WASI has neither, and wasi-libc declares neither, so a
// program linking LLVM for WASI is left with these names undefined even though
// nothing it does calls them. Each says "not here", which is what a caller
// finding no plugin or no advice to give already handles.

#include <stddef.h>

void* dlopen(const char* file, int mode)
{
    (void)file;
    (void)mode;
    return NULL;
}

void* dlsym(void* handle, const char* name)
{
    (void)handle;
    (void)name;
    return NULL;
}

int dlclose(void* handle)
{
    (void)handle;
    return 0;
}

char* dlerror(void)
{
    return (char*)"WASI cannot load a library at run time";
}

int posix_madvise(void* address, size_t length, int advice)
{
    (void)address;
    (void)length;
    (void)advice;
    return 0;
}
