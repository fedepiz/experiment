#pragma once

#include <stdint.h>
#include <string.h>

////////////////////////////////
//~ fp: Context Cracking
//
// Normalizes the compiler's predefined macro soup into one fixed vocabulary.
// Every macro is defined to 1 or 0 by the end of this section -- never left
// undefined -- so downstream code writes `#if OS_LINUX` and a typo becomes a
// -Wundef warning instead of a silently-false branch.

#if defined(__clang__)
# define COMPILER_CLANG 1
#elif defined(__GNUC__)
# define COMPILER_GCC 1
#elif defined(_MSC_VER)
# define COMPILER_CL 1
#else
# error This compiler is not supported yet.
#endif

#if defined(__gnu_linux__)
# define OS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
# define OS_MAC 1
#elif defined(_WIN32)
# define OS_WINDOWS 1
#else
# error This OS is not supported yet.
#endif

#if defined(__amd64__) || defined(_M_AMD64)
# define ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
# define ARCH_ARM64 1
#else
# error This architecture is not supported yet.
#endif

//- fp: zero-fill missing context macros

#if !defined(COMPILER_CLANG)
# define COMPILER_CLANG 0
#endif
#if !defined(COMPILER_GCC)
# define COMPILER_GCC 0
#endif
#if !defined(COMPILER_CL)
# define COMPILER_CL 0
#endif
#if !defined(OS_LINUX)
# define OS_LINUX 0
#endif
#if !defined(OS_MAC)
# define OS_MAC 0
#endif
#if !defined(OS_WINDOWS)
# define OS_WINDOWS 0
#endif
#if !defined(ARCH_X64)
# define ARCH_X64 0
#endif
#if !defined(ARCH_ARM64)
# define ARCH_ARM64 0
#endif

// Normally set by build.sh; default to debug when compiled some other way.
#if !defined(BUILD_DEBUG)
# define BUILD_DEBUG 1
#endif

////////////////////////////////
//~ fp: Codebase Keywords
//
// In a unity build every symbol lives in one translation unit, so `static` no
// longer says anything about linkage -- these spell out intent instead.

#define internal      static
#define global        static
#define local_persist static

////////////////////////////////
//~ fp: Base Types

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

typedef int8_t  I8;
typedef int16_t I16;
typedef int32_t I32;
typedef int64_t I64;

typedef int8_t  B8;
typedef int16_t B16;
typedef int32_t B32;
typedef int64_t B64;

typedef float  F32;
typedef double F64;

////////////////////////////////
//~ fp: Boolean constants
#define false 0
#define true 1

////////////////////////////////
//~ fp: Units

#define KB(n) (((U64)(n)) << 10)
#define MB(n) (((U64)(n)) << 20)
#define GB(n) (((U64)(n)) << 30)
#define TB(n) (((U64)(n)) << 40)

#define Thousand(n) (((U64)(n)) * 1000)
#define Million(n)  (((U64)(n)) * 1000000)
#define Billion(n)  (((U64)(n)) * 1000000000)

////////////////////////////////
//~ fp: Token Pasting / Stringification

#define Glue_(a, b) a##b
#define Glue(a, b)  Glue_(a, b)

#define Stringify_(s) #s
#define Stringify(s)  Stringify_(s)

////////////////////////////////
//~ fp: Helper Macros

// Wraps multi-statement macros so they behave as one statement after an `if`.
#define Stmnt(s) do { s } while(0)

#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))
#define AlignOf(T)    _Alignof(T)

#define Min(a, b)      (((a) < (b)) ? (a) : (b))
#define Max(a, b)      (((a) > (b)) ? (a) : (b))
#define ClampTop(a, x) Min(a, x)
#define ClampBot(x, b) Max(x, b)
#define Clamp(a, x, b) (((x) < (a)) ? (a) : ((x) > (b)) ? (b) : (x))

// `b` must be a power of two.
#define AlignPow2(x, b)     (((x) + (b) - 1) & (~((b) - 1)))
#define AlignDownPow2(x, b) ((x) & (~((b) - 1)))
#define IsPow2(x)           (((x) != 0) && (((x) & ((x) - 1)) == 0))

#define Swap(T, a, b) Stmnt(T swap__t = (a); (a) = (b); (b) = swap__t;)

#define Unused(x) ((void)(x))

// Runs `begin` before the body and `end` after it; the body runs exactly
// once. A `break` inside the body skips `end`.
#define DeferLoop(begin, end) for(int Glue(defer__, __LINE__) = ((begin), 0); \
                                  !Glue(defer__, __LINE__);                   \
                                  Glue(defer__, __LINE__) += 1, (end))

////////////////////////////////
//~ fp: Asserts

#if COMPILER_CL
# define AssertBreak() __debugbreak()
#else
# define AssertBreak() __builtin_trap()
#endif

// Assert compiles away in release builds; AssertAlways survives them.
#define AssertAlways(c) Stmnt(if(!(c)) { AssertBreak(); })
#if BUILD_DEBUG
# define Assert(c) AssertAlways(c)
#else
# define Assert(c) ((void)0)
#endif

#define InvalidPath    Assert(!"Invalid path!")
#define NotImplemented Assert(!"Not implemented!")

#define StaticAssert(c, id) _Static_assert((c), Stringify(id))

#define MemoryZero(p, z)       memset((p), 0, (z))
#define MemoryZeroStruct(p)    MemoryZero((p), sizeof(*(p)))
#define MemoryZeroArray(a)     MemoryZero((a), sizeof(a))
#define MemoryCopy(d, s, z)    memmove((d), (s), (z))
#define MemoryCopyStruct(d, s) MemoryCopy((d), (s), sizeof(*(d)))
#define MemoryMatch(a, b, z)   (memcmp((a), (b), (z)) == 0)

////////////////////////////////
//~ fp: Linked List Macros
//
// f = first, l = last, n = node, p = previous. Nodes are expected to have
// `next` (and `prev` for DLL) members. Zero is the nil node.

//- fp: singly-linked stack
#define SLLStackPush(f, n) ((n)->next = (f), (f) = (n))
#define SLLStackPop(f)     ((f) = (f)->next)

//- fp: singly-linked queue
#define SLLQueuePush(f, l, n) ((f) == 0 ?                        \
                               ((f) = (l) = (n)) :               \
                               ((l)->next = (n), (l) = (n)),     \
                               (n)->next = 0)
#define SLLQueuePushFront(f, l, n) ((f) == 0 ?                            \
                                    ((f) = (l) = (n), (n)->next = 0) :    \
                                    ((n)->next = (f), (f) = (n)))
#define SLLQueuePop(f, l) ((f) == (l) ?             \
                           ((f) = (l) = 0) :        \
                           ((f) = (f)->next))

//- fp: doubly-linked list
#define DLLPushBack(f, l, n) Stmnt(                                        \
  if((f) == 0) { (f) = (l) = (n); (n)->next = (n)->prev = 0; }             \
  else { (n)->prev = (l); (n)->next = 0; (l)->next = (n); (l) = (n); })
#define DLLPushFront(f, l, n) Stmnt(                                       \
  if((f) == 0) { (f) = (l) = (n); (n)->next = (n)->prev = 0; }             \
  else { (n)->next = (f); (n)->prev = 0; (f)->prev = (n); (f) = (n); })
#define DLLInsert(f, l, p, n) Stmnt(                                       \
  if((p) == 0) { DLLPushFront(f, l, n); }                                  \
  else if((p) == (l)) { DLLPushBack(f, l, n); }                            \
  else { (n)->next = (p)->next; (n)->prev = (p);                           \
         (p)->next->prev = (n); (p)->next = (n); })
#define DLLRemove(f, l, n) Stmnt(                                          \
  if((n)->prev) { (n)->prev->next = (n)->next; } else { (f) = (n)->next; } \
  if((n)->next) { (n)->next->prev = (n)->prev; } else { (l) = (n)->prev; })
