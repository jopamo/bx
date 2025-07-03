/* GNU cpio applet config wrapper for bx.
   Reuse bx's curated Linux GNU baseline, but override package identity
   macros so cpio reports itself correctly. */

#ifndef BX_GNU_CPIO_CONFIG_WRAPPER_H
#define BX_GNU_CPIO_CONFIG_WRAPPER_H

#include "../shared/config.h"

#ifndef HAVE_ATTRIB_PACKED
#define HAVE_ATTRIB_PACKED 1
#endif

#ifndef RETTYPE_MAJOR
#define RETTYPE_MAJOR int
#endif

#ifndef RETTYPE_MINOR
#define RETTYPE_MINOR int
#endif

#undef PACKAGE
#define PACKAGE "cpio"

#undef PACKAGE_BUGREPORT
#define PACKAGE_BUGREPORT "bug-cpio@gnu.org"

#undef PACKAGE_NAME
#define PACKAGE_NAME "GNU cpio"

#undef PACKAGE_STRING
#define PACKAGE_STRING "GNU cpio 2.15"

#undef PACKAGE_TARNAME
#define PACKAGE_TARNAME "cpio"

#undef PACKAGE_URL
#define PACKAGE_URL "http://www.gnu.org/software/cpio"

#undef PACKAGE_VERSION
#define PACKAGE_VERSION "2.15"

#undef VERSION
#define VERSION "2.15"

#endif
