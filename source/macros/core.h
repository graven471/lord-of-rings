#ifndef MACROS_COMPILER_H
#define MACROS_COMPILER_H

#include <stddef.h>

/* Branch prediction */
#if defined(__GNUC__) || defined(__clang__)
    #define likely(x)   __builtin_expect(!!(x), 1)
    #define unlikely(x) __builtin_expect(!!(x), 0)
#else
    #define likely(x)   (!!(x))
    #define unlikely(x) (!!(x))
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CPU_RELAX() _mm_pause()

#elif defined(__aarch64__) || defined(__arm__)
#define CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")

#elif defined(__riscv)
#define CPU_RELAX() __asm__ __volatile__("" ::: "memory")

#else
#define CPU_RELAX() __asm__ __volatile__("" ::: "memory")
#endif

#define RING_PTR(base, offset) ((uint32_t *)((char *)(base) + (offset)))

/* Attributes */
#if defined(__GNUC__) || defined(__clang__)
    #define NORETURN       __attribute__((noreturn))
    #define UNUSED         __attribute__((unused))
    #define WARN_UNUSED    __attribute__((warn_unused_result))
    #define ALWAYS_INLINE  __attribute__((always_inline)) inline
    #define NOINLINE       __attribute__((noinline))
    #define COLD           __attribute__((cold))
    #define HOT            __attribute__((hot))
    #define PACKED         __attribute__((packed))
    #define PRINTF_LIKE(fmt, args) \
        __attribute__((format(printf, fmt, args)))
#else
    #define NORETURN
    #define UNUSED
    #define WARN_UNUSED
    #define ALWAYS_INLINE inline
    #define NOINLINE
    #define COLD
    #define HOT
    #define PACKED
    #define PRINTF_LIKE(fmt, args)
#endif

/* Array utilities */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Alignment */
#define ALIGN_UP(value, alignment) \
    (((value) + (alignment) - 1) & ~((alignment) - 1))

#define ALIGN_DOWN(value, alignment) \
    ((value) & ~((alignment) - 1))

/* Compile-time checks */
#define STATIC_ASSERT(condition, message) \
    _Static_assert(condition, message)

#endif /* MACROS_COMPILER_H */
