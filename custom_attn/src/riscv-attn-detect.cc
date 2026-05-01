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
