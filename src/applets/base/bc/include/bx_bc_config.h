/*
 * bx native bc build configuration.
 *
 * Keep the bc applet self-configured so it can build as ordinary bx sources
 * instead of hiding behind a separate compatibility library target.
 */

#ifndef BX_BC_CONFIG_H
#define BX_BC_CONFIG_H

#ifndef BC_ENABLED
#define BC_ENABLED (1)
#endif // BC_ENABLED

#ifndef DC_ENABLED
#define DC_ENABLED (0)
#endif // DC_ENABLED

#ifndef BC_ENABLE_EXTRA_MATH
#define BC_ENABLE_EXTRA_MATH (1)
#endif // BC_ENABLE_EXTRA_MATH

#ifndef BC_ENABLE_LIBRARY
#define BC_ENABLE_LIBRARY (0)
#endif // BC_ENABLE_LIBRARY

#ifndef BC_ENABLE_HISTORY
#define BC_ENABLE_HISTORY (0)
#endif // BC_ENABLE_HISTORY

#ifndef BC_ENABLE_EDITLINE
#define BC_ENABLE_EDITLINE (0)
#endif // BC_ENABLE_EDITLINE

#ifndef BC_ENABLE_READLINE
#define BC_ENABLE_READLINE (0)
#endif // BC_ENABLE_READLINE

#ifndef BC_ENABLE_NLS
#define BC_ENABLE_NLS (0)
#endif // BC_ENABLE_NLS

#ifndef BC_ENABLE_MEMCHECK
#define BC_ENABLE_MEMCHECK (0)
#endif // BC_ENABLE_MEMCHECK

#ifndef BC_ENABLE_AFL
#define BC_ENABLE_AFL (0)
#endif // BC_ENABLE_AFL

#ifndef BC_ENABLE_OSSFUZZ
#define BC_ENABLE_OSSFUZZ (0)
#endif // BC_ENABLE_OSSFUZZ

#ifndef VERSION
#define VERSION 7.2.0
#endif // VERSION

#ifndef BUILD_TYPE
#define BUILD_TYPE release
#endif // BUILD_TYPE

#ifndef MAINEXEC
#define MAINEXEC bc
#endif // MAINEXEC

#endif // BX_BC_CONFIG_H
