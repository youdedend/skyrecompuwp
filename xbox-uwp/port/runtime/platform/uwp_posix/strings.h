// <strings.h> for UWP: the case-insensitive comparisons from POSIX that UCRT spells differently.
//
// One file (runtime/hle/pl.c) includes this header. UCRT has _stricmp/_strnicmp; ../uwp_compat.h
// already maps strcasecmp/strncasecmp onto them for MSVC, and strnlen is a real UCRT function
// declared in <string.h>. So this is the thin include, kept separate because a translation unit
// that includes <strings.h> and never sees <string.h> still has to compile.
#pragma once

#include "../uwp_compat.h"

#include <string.h>

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
