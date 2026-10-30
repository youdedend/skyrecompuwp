// <dlfcn.h> for UWP: nothing, on purpose.
//
// runtime/gamelib.c includes this header unconditionally, but every call through it (dlopen,
// dlsym, dlerror) sits in the non-Windows branch of that file. On the console the game code is
// opened with LoadPackagedLibrary instead - an AppContainer loads only DLLs from its own package,
// by name from the package root - which patch 0001-gamelib-load-packaged-library.patch selects.
//
// So the header has to exist and must not declare anything: a translation unit that reaches for
// dlopen on this platform gets an undeclared identifier at the call site rather than a link error
// three build steps later, and nothing can accidentally resolve to something that half works.
#pragma once
