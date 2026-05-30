#ifndef BX_LIB_COMPILER_H
#define BX_LIB_COMPILER_H

#ifndef __has_builtin
#define __has_builtin(name) 0
#endif

#ifndef __has_attribute
#define __has_attribute(name) 0
#endif

#if __has_builtin(__builtin_expect) || defined(__GNUC__)
#define BX_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define BX_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#else
#define BX_LIKELY(expr) (expr)
#define BX_UNLIKELY(expr) (expr)
#endif

#if __has_attribute(cold) || defined(__GNUC__)
#define BX_COLD __attribute__((cold))
#else
#define BX_COLD
#endif

#endif /* BX_LIB_COMPILER_H */
