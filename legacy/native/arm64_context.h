#ifndef CF_ARM64_CONTEXT_H
#define CF_ARM64_CONTEXT_H

#define CF_CONTEXT_SIZE 800
#define CF_CONTEXT_SP 248
#define CF_CONTEXT_Q 256
#define CF_CONTEXT_NZCV 768
#define CF_CONTEXT_FPCR 776
#define CF_CONTEXT_FPSR 784

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
typedef struct {
  uint64_t x[31];
  uintptr_t sp;
  _Alignas(16) float q[32][4];
  uint64_t nzcv;
  uint64_t fpcr;
  uint64_t fpsr;
  uint64_t reserved;
} Arm64Context;
_Static_assert(sizeof(Arm64Context) == CF_CONTEXT_SIZE, "assembly frame size");
_Static_assert(offsetof(Arm64Context, sp) == CF_CONTEXT_SP, "assembly SP offset");
_Static_assert(offsetof(Arm64Context, q) == CF_CONTEXT_Q, "assembly SIMD offset");
_Static_assert(offsetof(Arm64Context, nzcv) == CF_CONTEXT_NZCV, "assembly NZCV offset");
_Static_assert(offsetof(Arm64Context, fpcr) == CF_CONTEXT_FPCR, "assembly FPCR offset");
_Static_assert(offsetof(Arm64Context, fpsr) == CF_CONTEXT_FPSR, "assembly FPSR offset");
#endif
#endif
