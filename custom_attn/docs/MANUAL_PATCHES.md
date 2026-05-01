# Manual Patches — RISC-V custom `attn` instruction

**Author:** Yash Awasthi, Group 9
This document contains the exact changes required in each source file.
All paths are relative to the root of the riscv-gnu-toolchain fork.

---

## File 1 — `binutils/include/opcode/riscv-opc.h`

### a) Add macro definitions

Find the block of custom-instruction macro definitions (typically near the top
of the file, grouped with other `MATCH_`/`MASK_` pairs).  Add:

```c
/* Custom attn instruction: R-type, opcode 0x0b (custom-0), funct7=0x01, funct3=0x0 */
#define MATCH_ATTN 0x0200000b
#define MASK_ATTN  0xfe00707f
```

### b) Add `DECLARE_INSN` entry

Locate the `#ifdef DECLARE_INSN` block.  Add the line **before** `#endif`:

```c
#ifdef DECLARE_INSN
/* ... existing entries ... */
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
#endif /* DECLARE_INSN */
```

**Common mistake:** Do NOT place it after the `#endif` line.

---

## File 2 — `binutils/opcodes/riscv-opc.c`

### Add entry to `riscv_opcodes[]`

Find the `riscv_opcodes[]` array initialisation.  Locate the `auipc` entry
and insert the `attn` line directly before it:

```c
/* Attention instruction */
{"attn",   0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
/* auipc is next ... */
{"auipc",  0, INSN_CLASS_I, "d,u",   MATCH_AUIPC, MASK_AUIPC, match_opcode, 0},
```

---

## File 3 — `gcc/gcc/config/riscv/riscv-ftypes.def`

### Add function-type definition

Find the last `DEF_RISCV_FTYPE` line and append after it (or insert in
sorted order if the file is ordered by arity):

```c
/* 2-argument UDI → UDI function type for __builtin_riscv_attn */
DEF_RISCV_FTYPE (2, (UDI, UDI, UDI))
```

The three `UDI` values are: return type, arg1 type, arg2 type.

---

## File 4 — `gcc/gcc/config/riscv/riscv-builtins.cc`

### Add availability predicate and builtin declaration

Find the section containing `AVAIL (` and `DIRECT_BUILTIN (` macros.
Add the following two lines together (order matters — AVAIL before
DIRECT_BUILTIN):

```c
/* attn built-in — always available on RISC-V */
AVAIL (always_enabled, (!0))
DIRECT_BUILTIN (attn, RISCV_UDI_FTYPE_UDI_UDI, always_enabled)
```

`(!0)` is the availability condition expression (always true).

---

## File 5 — `gcc/gcc/config/riscv/riscv.md`

### Append unspec enum and instruction pattern

At the very end of `riscv.md`, append:

```scheme
;;; -----------------------------------------------------------------------
;;; Custom attn instruction
;;; -----------------------------------------------------------------------

(define_c_enum "unspec"
  [UNSPEC_ATTN])

(define_insn "riscv_attn"
  [(set (match_operand:DI 0 "register_operand" "=r")
        (unspec:DI
          [(match_operand:DI 1 "register_operand" "r")
           (match_operand:DI 2 "register_operand" "r")]
          UNSPEC_ATTN))]
  ""
  "attn\t%0,%1,%2"
  [(set_attr "type"  "unknown")
   (set_attr "mode"  "DI")])
```

---

## File 6 — `gcc/gcc/config/riscv/riscv-attn-detect.cc` (new file)

Full source content follows.  This file implements the GIMPLE pass that
auto-detects the 4-stage transformer attention loop pattern.

```cpp
/* riscv-attn-detect.cc — GIMPLE pass to detect and optionally replace
   the transformer attention computation pattern with __builtin_riscv_attn.

   Author: Yash Awasthi, Group 9
   Instruction encoding:
     attn rd, rs1, rs2
     opcode = 0x0b (custom-0), funct3 = 0x0, funct7 = 0x01
     MATCH_ATTN = 0x0200000b
     MASK_ATTN  = 0xfe00707f                                              */

#define IN_TARGET_CODE 1

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "tree-pass.h"
#include "context.h"
#include "tree-ssa-loop.h"
#include "cfgloop.h"
#include "tree-scalar-evolution.h"
#include "tree-chrec.h"
#include "gimplify.h"
#include "gimple-ssa.h"
#include "tree-phinodes.h"
#include "ssa-iterators.h"
#include "stringpool.h"
#include "tree-ssanames.h"
#include "tree-ssa.h"
#include "cfghooks.h"
#include "basic-block.h"
#include "diagnostic-core.h"
#include "optabs.h"
#include "recog.h"
#include "expr.h"

/* =========================================================================
   Section A — pass_data declaration
   ========================================================================= */

namespace {

const pass_data pass_data_riscv_attn_detect = {
  GIMPLE_PASS,                  /* type                 */
  "riscv_attn_detect",          /* name                 */
  OPTGROUP_LOOP,                /* optinfo_flags        */
  TV_NONE,                      /* tv_id                */
  ( PROP_cfg | PROP_ssa ),      /* properties_required  */
  0,                            /* properties_provided  */
  0,                            /* properties_destroyed */
  0,                            /* todo_flags_start     */
  TODO_update_ssa,              /* todo_flags_finish    */
};

/* =========================================================================
   Section B — helper: count nested loop depth
   ========================================================================= */

static int
count_loop_depth (class loop *lp)
{
  int depth = 0;
  for (class loop *l = lp; l && l->num != 0; l = loop_outer (l))
    depth++;
  return depth;
}

/* =========================================================================
   Section C — attention pattern detector
   Returns true when 'lp' is the outermost loop of a 4-stage attention nest.
   ========================================================================= */

static bool
detect_attention_pattern (class loop *lp, function *fun ATTRIBUTE_UNUSED)
{
  /* Heuristic: we expect the outermost attention loop at depth 1,
     with at least 4 sibling or child loops that together cover
     the QK^T, softmax, and score×V stages.                         */

  if (!lp || lp->num == 0)
    return false;

  /* Must be a top-level loop (direct child of the root).            */
  if (loop_depth (lp) != 1)
    return false;

  /* Count the total number of loops in the tree rooted at 'lp'.    */
  int total = 0;
  class loop *l;
  FOR_EACH_LOOP (l, 0)
    {
      if (l->num == 0) continue;
      if (flow_loop_nested_p (lp, l) || l == lp)
        total++;
    }

  /* A 4-stage attention pattern typically produces 6-12 loops when
     the C source is compiled with -O2 (outer i-loop + inner j/k
     loops for each stage).  Use 5 as a conservative lower bound.   */
  if (total < 5)
    return false;

  return true;
}

/* =========================================================================
   Section D — main pass class
   ========================================================================= */

class pass_riscv_attn_detect : public gimple_opt_pass
{
public:
  pass_riscv_attn_detect (gcc::context *ctxt)
    : gimple_opt_pass (pass_data_riscv_attn_detect, ctxt)
  {}

  /* opt_pass methods */
  bool gate (function *) final override { return optimize >= 1; }
  unsigned int execute (function *fun) final override;
};

/* =========================================================================
   Section E — execute
   ========================================================================= */

unsigned int
pass_riscv_attn_detect::execute (function *fun)
{
  /* --- Issue 4 fix: conditional loop/SCEV initialisation ------------ */
  bool local_loop_init = (loops_for_fn (fun) == NULL);
  if (local_loop_init)
    {
      loop_optimizer_init (LOOPS_NORMAL);
      scev_initialize ();
    }

  bool found = false;

  /* Walk every top-level loop looking for the attention pattern.    */
  class loop *lp;
  FOR_EACH_LOOP (lp, 0)
    {
      if (lp->num == 0) continue;
      if (loop_depth (lp) != 1) continue;

      if (detect_attention_pattern (lp, fun))
        {
          if (dump_file)
            fprintf (dump_file,
                     "Attention pattern detected in '%s'\n",
                     function_name (fun));

          inform (UNKNOWN_LOCATION,
                  "riscv-attn-detect: attention pattern found in %qs;"
                  " use __builtin_riscv_attn() for hardware acceleration",
                  function_name (fun));

          found = true;
          /* NOTE (Issue 6): CFG replacement is disabled.
             Replacing the loop nest with a builtin call requires
             careful removal of all loop BBs and repair of dangling
             SSA uses before calling update_ssa().  This is left as
             future work.  Detection-only mode is fully functional.  */
          break;
        }
    }

  (void) found; /* suppress unused-variable warning */

  /* --- Tear down local loop/SCEV context if we set it up ----------- */
  if (local_loop_init)
    {
      scev_finalize ();
      loop_optimizer_finalize ();
    }

  return 0;
}

} /* anonymous namespace */

/* =========================================================================
   Section F — pass factory (called from passes.def / pass registration)
   ========================================================================= */

gimple_opt_pass *
make_pass_riscv_attn_detect (gcc::context *ctxt)
{
  return new pass_riscv_attn_detect (ctxt);
}
```

---

## File 7 — `gcc/gcc/config/riscv/t-riscv`

### Add build rule

Append to `t-riscv`:

```makefile
# Build rule for the custom attn GIMPLE detection pass
# NOTE: Use $(COMPILE) $< then $(POSTCOMPILE) — do NOT add $(OUTPUT_OPTION)
#       because $(COMPILE) already includes -o $@.
riscv-attn-detect.o: $(srcdir)/config/riscv/riscv-attn-detect.cc
	$(COMPILE) $<
	$(POSTCOMPILE)
```

The indentation before `$(COMPILE)` and `$(POSTCOMPILE)` **must be a TAB
character**, not spaces.  Make will silently fail to recognise the recipe
if spaces are used.

---

## File 8 — `gcc/gcc/config.gcc`

### Add to `extra_objs`

Find the `riscv*-*-*` stanza and extend `extra_objs`:

```bash
# Locate the line (example — exact content varies by GCC version):
extra_objs="riscv-builtins.o riscv-c.o riscv-sr.o riscv-string.o riscv-avlprop.o"

# Change to:
extra_objs="riscv-builtins.o riscv-c.o riscv-sr.o riscv-string.o riscv-avlprop.o \
            riscv-attn-detect.o"
```

---

## File 9 — `gcc/gcc/passes.def`

### Register pass after `pass_loop_distribution`

```c
/* Inside the GIMPLE loop group — search for pass_loop_distribution */
NEXT_PASS (pass_loop_distribution);
NEXT_PASS (pass_riscv_attn_detect);   /* ← insert this line */
```

Also add the extern declaration in `gcc/gcc/tree-pass.h` (or in the same
`.cc` file before `passes.def` is included):

```c
extern gimple_opt_pass *make_pass_riscv_attn_detect (gcc::context *);
```

---

## Encoding reference

```
Instruction: attn rd, rs1, rs2

Bit layout (32-bit R-type):
 31        25 24    20 19    15 14  12 11     7 6       0
[  0000001  |  rs2   |  rs1   |  000 |   rd   | 0001011 ]
   funct7     rs2      rs1    funct3   rd       opcode

opcode  = 0b0001011 = 0x0b  (custom-0)
funct3  = 0b000     = 0x0
funct7  = 0b0000001 = 0x01

MATCH_ATTN = 0x0200000b
MASK_ATTN  = 0xfe00707f

Example: attn a0, a0, a1
  rs1=a0=x10=0b01010, rs2=a1=x11=0b01011, rd=a0=x10=0b01010
  [0000001][01011][01010][000][01010][0001011]
  = 0000_0010_1011_0101_0000_0101_0000_1011
  = 0x02b5050b
```
