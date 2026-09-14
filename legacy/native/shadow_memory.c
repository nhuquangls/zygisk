/*
 * shadow_memory.c — Shadow Memory engine for CRC-invisible code patching
 *
 * Technique: after writing a branch veneer into a libil2cpp.so .text page,
 * strip PROT_READ (leave PROT_EXEC only).  When libanort's CRC scanner
 * does LDR from that page the CPU fires SIGSEGV.  Our signal handler
 * emulates the load from a clean backup copy, writes the original value
 * into the faulting thread's register, and advances PC past the instruction.
 *
 * The CRC scanner sees original bytes → match.  The game CPU still
 * executes from the page via EXEC permission → branch veneer runs.
 */

#define _GNU_SOURCE
#include "shadow_memory.h"

#include <android/log.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ShadowMem", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "ShadowMem", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ShadowMem", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Shadow page database                                              */
/* ------------------------------------------------------------------ */

static ShadowPage g_pages[SHADOW_MAX_PAGES];
static int        g_count = 0;

/* Recursion guard — if the handler itself faults, don't loop. */
static volatile int g_in_handler = 0;

/* Alternate signal stack so the handler is reliable even when the
 * game thread's stack is deep or corrupted. */
static uint8_t g_sigstack[SIGSTKSZ];

/* Previous SIGSEGV action, for re-raising unknown faults. */
static struct sigaction g_prev_sa;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static long page_size(void) {
  static long ps = 0;
  if (ps == 0) ps = sysconf(_SC_PAGESIZE);
  return ps;
}

static uintptr_t page_align_down(uintptr_t addr) {
  return addr & ~(uintptr_t)(page_size() - 1);
}

static uintptr_t page_align_up(uintptr_t addr) {
  return (addr + page_size() - 1) & ~(uintptr_t)(page_size() - 1);
}

static ShadowPage *find_page(uintptr_t addr) {
  for (int i = 0; i < g_count; ++i) {
    if (g_pages[i].active &&
        addr >= g_pages[i].page_start &&
        addr <  g_pages[i].page_start + g_pages[i].page_size)
      return &g_pages[i];
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/*  ARM64 instruction decoder (async-signal-safe subset)              */
/*                                                                     */
/*  Emulates the load instruction that caused SIGSEGV and advances PC. */
/*  Only handles the patterns actually emitted by il2cpp / libanort.   */
/* ------------------------------------------------------------------ */

static void emulate_load(ucontext_t *uc, ShadowPage *page) {
  uint32_t pc   = (uint32_t)uc->uc_mcontext.pc;
  uint32_t inst = *(volatile uint32_t *)(uintptr_t)pc;

  /* --------------------------------------------------------------- */
  /* LDR Xt, [Xn, #imm]  (immediate, unsigned offset, 64-bit)       */
  /* Encoding: 11 111 0 01 01 imm12 Rn Rt                          */
  /* Mask matches bits [31:22] = 1111100101                         */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFC00000) == 0xF9400000) {
    int Rt    = inst & 0x1F;
    int Rn    = (inst >> 5) & 0x1F;
    int imm12 = (inst >> 10) & 0xFFF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)imm12 * 8;
    uintptr_t off  = addr - page->page_start;
    if (off + 8 <= page->page_size) {
      uint64_t val;
      memcpy(&val, page->clean_backup + off, 8);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Wt, [Xn, #imm]  (immediate, unsigned offset, 32-bit)       */
  /* Encoding: 10 111 0 01 01 imm12 Rn Rt                          */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFC00000) == 0xB9400000) {
    int Rt    = inst & 0x1F;
    int Rn    = (inst >> 5) & 0x1F;
    int imm12 = (inst >> 10) & 0xFFF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)imm12 * 4;
    uintptr_t off  = addr - page->page_start;
    if (off + 4 <= page->page_size) {
      uint32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Xt, [Xn, #imm]!  (pre-index, 64-bit)                       */
  /* Encoding: 11 111 0 00 01 imm9 11 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xF8400C00) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;   /* sign extend */
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm9;
    uintptr_t off  = addr - page->page_start;
    if (off + 8 <= page->page_size) {
      uint64_t val;
      memcpy(&val, page->clean_backup + off, 8);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = val;
      if (Rn != 31) uc->uc_mcontext.regs[Rn] = addr;  /* writeback */
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Wt, [Xn, #imm]!  (pre-index, 32-bit)                       */
  /* Encoding: 10 111 0 00 01 imm9 11 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xB8400C00) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm9;
    uintptr_t off  = addr - page->page_start;
    if (off + 4 <= page->page_size) {
      uint32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)val;
      if (Rn != 31) uc->uc_mcontext.regs[Rn] = addr;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Xt, [Xn], #imm  (post-index, 64-bit)                       */
  /* Encoding: 11 111 0 00 01 imm9 01 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xF8400400) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t off = base - page->page_start;
    if (off + 8 <= page->page_size) {
      uint64_t val;
      memcpy(&val, page->clean_backup + off, 8);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = val;
      if (Rn != 31) uc->uc_mcontext.regs[Rn] = base + (uintptr_t)(intptr_t)imm9;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Wt, [Xn], #imm  (post-index, 32-bit)                       */
  /* Encoding: 10 111 0 00 01 imm9 01 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xB8400400) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t off = base - page->page_start;
    if (off + 4 <= page->page_size) {
      uint32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)val;
      if (Rn != 31) uc->uc_mcontext.regs[Rn] = base + (uintptr_t)(intptr_t)imm9;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Xt, [PC, #imm]  (literal, 64-bit)                          */
  /* Encoding: 01 011 0 00 imm19 Rt                                 */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFF000000) == 0x58000000) {
    int Rt     = inst & 0x1F;
    int imm19  = (int)((inst >> 5) & 0x7FFFF);
    if (imm19 & 0x40000) imm19 |= ~0x7FFFF;
    uintptr_t addr = (uintptr_t)((int64_t)pc + (int64_t)imm19 * 4);
    uintptr_t off  = addr - page->page_start;
    if (off + 8 <= page->page_size) {
      uint64_t val;
      memcpy(&val, page->clean_backup + off, 8);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDR Wt, [PC, #imm]  (literal, 32-bit)                          */
  /* Encoding: 00 011 0 00 imm19 Rt                                 */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFF000000) == 0x18000000) {
    int Rt     = inst & 0x1F;
    int imm19  = (int)((inst >> 5) & 0x7FFFF);
    if (imm19 & 0x40000) imm19 |= ~0x7FFFF;
    uintptr_t addr = (uintptr_t)((int64_t)pc + (int64_t)imm19 * 4);
    uintptr_t off  = addr - page->page_start;
    if (off + 4 <= page->page_size) {
      uint32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDRSW Xt, [PC, #imm]  (literal, sign-extend 32→64)             */
  /* Encoding: 10 011 0 00 imm19 Rt                                 */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFF000000) == 0x98000000) {
    int Rt     = inst & 0x1F;
    int imm19  = (int)((inst >> 5) & 0x7FFFF);
    if (imm19 & 0x40000) imm19 |= ~0x7FFFF;
    uintptr_t addr = (uintptr_t)((int64_t)pc + (int64_t)imm19 * 4);
    uintptr_t off  = addr - page->page_start;
    if (off + 4 <= page->page_size) {
      int32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)(int64_t)val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDP X1, X2, [Xn, #imm]  (load pair, 64-bit, signed offset)    */
  /* Encoding: 10 101 0 011 1 imm7 Rt2 Rn Rt1                      */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFC00000) == 0xA9400000) {
    int Rt1  = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int Rt2  = (inst >> 10) & 0x1F;
    int imm7 = (int)((inst >> 15) & 0x7F);
    if (imm7 & 0x40) imm7 |= ~0x7F;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm7 * 8;
    uintptr_t off  = addr - page->page_start;
    if (off + 16 <= page->page_size) {
      uint64_t v1, v2;
      memcpy(&v1, page->clean_backup + off,     8);
      memcpy(&v2, page->clean_backup + off + 8, 8);
      if (Rt1 != 31) uc->uc_mcontext.regs[Rt1] = v1;
      if (Rt2 != 31) uc->uc_mcontext.regs[Rt2] = v2;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDP W1, W2, [Xn, #imm]  (load pair, 32-bit)                    */
  /* Encoding: 10 101 0 010 1 imm7 Rt2 Rn Rt1                      */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFC00000) == 0x29400000) {
    int Rt1  = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int Rt2  = (inst >> 10) & 0x1F;
    int imm7 = (int)((inst >> 15) & 0x7F);
    if (imm7 & 0x40) imm7 |= ~0x7F;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm7 * 4;
    uintptr_t off  = addr - page->page_start;
    if (off + 8 <= page->page_size) {
      uint32_t v1, v2;
      memcpy(&v1, page->clean_backup + off,     4);
      memcpy(&v2, page->clean_backup + off + 4, 4);
      if (Rt1 != 31) uc->uc_mcontext.regs[Rt1] = (uint64_t)v1;
      if (Rt2 != 31) uc->uc_mcontext.regs[Rt2] = (uint64_t)v2;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDUR Xt, [Xn, #imm]  (unscaled offset, 64-bit)                 */
  /* Encoding: 11 111 0 00 00 imm9 10 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xF8400000) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm9;
    uintptr_t off  = addr - page->page_start;
    if (off + 8 <= page->page_size) {
      uint64_t val;
      memcpy(&val, page->clean_backup + off, 8);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* --------------------------------------------------------------- */
  /* LDUR Wt, [Xn, #imm]  (unscaled offset, 32-bit)                 */
  /* Encoding: 10 111 0 00 00 imm9 10 Rn Rt                        */
  /* --------------------------------------------------------------- */
  if ((inst & 0xFFE00C00) == 0xB8400000) {
    int Rt   = inst & 0x1F;
    int Rn   = (inst >> 5) & 0x1F;
    int imm9 = (inst >> 12) & 0x1FF;
    if (imm9 & 0x100) imm9 |= ~0x1FF;
    uintptr_t base = (Rn == 31)
        ? (uintptr_t)uc->uc_mcontext.sp
        : uc->uc_mcontext.regs[Rn];
    uintptr_t addr = base + (uintptr_t)(intptr_t)imm9;
    uintptr_t off  = addr - page->page_start;
    if (off + 4 <= page->page_size) {
      uint32_t val;
      memcpy(&val, page->clean_backup + off, 4);
      if (Rt != 31) uc->uc_mcontext.regs[Rt] = (uint64_t)val;
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }

  /* Unrecognised instruction on a shadow page — shouldn't happen. */
  LOGE("unhandled instruction 0x%08x at pc=0x%x fault=0x%lx",
       inst, pc, (unsigned long)uc->uc_mcontext.regs[31]);
}

/* ------------------------------------------------------------------ */
/*  SIGSEGV handler                                                   */
/* ------------------------------------------------------------------ */

static void shadow_sigsegv(int sig, siginfo_t *info, void *ctx) {
  if (g_in_handler) goto re_raise;
  g_in_handler = 1;

  ucontext_t *uc = (ucontext_t *)ctx;
  uintptr_t fault = (uintptr_t)info->si_addr;
  ShadowPage *page = find_page(fault);

  if (page) {
    emulate_load(uc, page);
    g_in_handler = 0;
    return;   /* resumed with new PC and register value */
  }

re_raise:
  g_in_handler = 0;
  /* Restore default handler and re-raise so tombstone/debugger sees it. */
  sigaction(SIGSEGV, &g_prev_sa, NULL);
  raise(SIGSEGV);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool shadow_init(void) {
  /* Alternate signal stack. */
  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp   = g_sigstack;
  ss.ss_size = sizeof(g_sigstack);
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) != 0) {
    LOGE("sigaltstack failed: %m");
    return false;
  }

  /* Install handler. */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = shadow_sigsegv;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  if (sigaction(SIGSEGV, &sa, &g_prev_sa) != 0) {
    LOGE("sigaction failed: %m");
    return false;
  }

  LOGI("shadow memory engine initialized (page_size=%ld, max=%d)",
       page_size(), SHADOW_MAX_PAGES);
  return true;
}

bool shadow_page_save(uintptr_t page_start, size_t page_size) {
  if (g_count >= SHADOW_MAX_PAGES) {
    LOGE("shadow page table full (%d)", SHADOW_MAX_PAGES);
    return false;
  }

  /* Deduplicate. */
  for (int i = 0; i < g_count; ++i) {
    if (g_pages[i].page_start == page_start) return true;
  }

  uint8_t *backup = (uint8_t *)malloc(page_size);
  if (!backup) {
    LOGE("malloc failed for shadow backup (%zu bytes)", page_size);
    return false;
  }
  memcpy(backup, (const void *)page_start, page_size);

  ShadowPage *sp   = &g_pages[g_count++];
  sp->page_start   = page_start;
  sp->page_size    = page_size;
  sp->clean_backup = backup;
  sp->original_prot = PROT_READ | PROT_EXEC;  /* typical .text */
  sp->active       = false;   /* not protected yet */

  LOGI("saved clean page 0x%lx (%zu KB) in slot %d",
       (unsigned long)page_start, page_size / 1024, g_count - 1);
  return true;
}

bool shadow_page_protect(uintptr_t page_start, size_t page_size) {
  for (int i = 0; i < g_count; ++i) {
    if (g_pages[i].page_start == page_start && !g_pages[i].active) {
      /* Strip PROT_READ — only PROT_EXEC remains. */
      if (mprotect((void *)page_start, page_size, PROT_EXEC) != 0) {
        LOGE("mprotect PROT_EXEC failed for 0x%lx: %m",
             (unsigned long)page_start);
        return false;
      }
      g_pages[i].active = true;
      LOGI("protected page 0x%lx → PROT_EXEC only (slot %d)",
           (unsigned long)page_start, i);
      return true;
    }
  }
  LOGW("shadow_page_protect: page 0x%lx not found in backup table",
       (unsigned long)page_start);
  return false;
}

void shadow_cleanup(void) {
  for (int i = 0; i < g_count; ++i) {
    if (g_pages[i].active) {
      mprotect((void *)g_pages[i].page_start, g_pages[i].page_size,
               g_pages[i].original_prot);
    }
    free(g_pages[i].clean_backup);
    g_pages[i].clean_backup = NULL;
    g_pages[i].active = false;
  }
  g_count = 0;

  /* Restore previous handler. */
  sigaction(SIGSEGV, &g_prev_sa, NULL);

  /* Remove alternate stack. */
  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);

  LOGI("shadow memory cleaned up");
}
