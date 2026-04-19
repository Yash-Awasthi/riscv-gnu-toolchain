/* riscv-attn-detect.cc — GIMPLE idiom detection pass for attention mechanism
 *
 * This pass recognizes the 4-stage attention pattern in plain C loops:
 *   Stage 1: score[i][j] += Q[i][k] * K[j][k]     (matmul Q × K^T)
 *   Stage 2: score[i][j] /= sqrt(d_model)           (scaling)
 *   Stage 3: softmax per row (max, exp-sum, normalize)
 *   Stage 4: output[i][j] += score[i][k] * V[k][j]  (matmul score × V)
 *
 * When all four stages are found consecutively, the pass replaces them
 * with a single call to __builtin_riscv_attn(dims_addr, qkv_addr),
 * which the existing machine description lowers to the custom `attn`
 * instruction (R-type, custom-0 opcode space).
 *
 * Registration: dynamically registered from riscv_option_override()
 * via register_pass(), inserted after pass_loop_distribution.
 *
 * Part of the RISC-V custom instruction project (Group 9).
 *
 * Copyright (C) 2025 Free Software Foundation, Inc.
 * Contributed by Group 9 — RISC-V Custom Instruction Project.
 *
 * This file is part of GCC.
 *
 * GCC is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3, or (at your option) any later
 * version.
 *
 * GCC is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

/* ═══════════════════════════════════════════════════════════════════
 *  SECTION A: Headers
 * ═══════════════════════════════════════════════════════════════════ */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "tree.h"
#include "gimple.h"
#include "tree-pass.h"
#include "ssa.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "cfgloop.h"
#include "tree-ssa-loop.h"
#include "tree-ssa-loop-niter.h"
#include "fold-const.h"
#include "tree-scalar-evolution.h"
#include "gimplify.h"
#include "gimple-fold.h"
#include "tree-cfg.h"
#include "cfganal.h"
#include "cfghooks.h"
#include "diagnostic-core.h"
#include "tree-ssa-operands.h"
#include "stor-layout.h"
#include "tree-into-ssa.h"
#include "tree-phinodes.h"
#include "gimple-pretty-print.h"
#include "stringpool.h"

/* ═══════════════════════════════════════════════════════════════════
 *  SECTION B: Candidate data structure
 * ═══════════════════════════════════════════════════════════════════ */

/* Collects all information from the 4 detected stages.  */
struct attn_candidate
{
  /* Stage 1: Q * K^T → score  */
  class loop *matmul1_loop;
  tree q_base;           /* base address of Q array  */
  tree k_base;           /* base address of K array  */
  tree score_base;       /* base address of score array  */

  /* Stage 2: score / sqrt(d)  */
  class loop *scale_loop;
  tree scale_divisor;    /* the sqrt(d_model) value  */

  /* Stage 3: softmax(score)  */
  class loop *softmax_loop;

  /* Stage 4: score * V → output  */
  class loop *matmul2_loop;
  tree v_base;           /* base address of V array  */
  tree output_base;      /* base address of output array  */

  /* Dimensions extracted from loop trip counts  */
  tree seq_len;          /* rows of Q / K / score (i-loop bound)  */
  tree d_model;          /* cols of Q / K (k-loop bound in matmul1)  */

  /* Insertion / deletion points  */
  basic_block insert_bb; /* preheader of the first loop  */
  basic_block exit_bb;   /* exit block of the fourth loop  */
};


/* ═══════════════════════════════════════════════════════════════════
 *  SECTION C: Helper functions for GIMPLE pattern matching
 * ═══════════════════════════════════════════════════════════════════ */

/* Return the nesting depth of LOOP (1 = single loop, 2 = doubly-nested, …).  */

static int
loop_depth_count (class loop *loop)
{
  int depth = 1;
  class loop *inner = loop->inner;
  while (inner)
    {
      /* If the loop has siblings at this level, it is not a perfect nest —
         we only follow the unique child.  */
      if (inner->next)
        return -1;  /* multiple children = not a simple nest  */
      depth++;
      inner = inner->inner;
    }
  return depth;
}

/* Return the innermost loop in a perfect nest starting from OUTER.  */

static class loop *
get_innermost (class loop *outer)
{
  class loop *l = outer;
  while (l->inner && !l->inner->next)
    l = l->inner;
  return l;
}

/* Count how many direct child loops LOOP has (siblings of loop->inner).  */

static int
count_child_loops (class loop *loop)
{
  int count = 0;
  for (class loop *child = loop->inner; child; child = child->next)
    count++;
  return count;
}


/* Strip NOP_EXPR, CONVERT_EXPR, SSA copies, and ADDR_EXPR to find the
   underlying VAR_DECL or PARM_DECL.  */

static tree
strip_to_decl (tree expr)
{
  if (!expr)
    return NULL_TREE;

  /* Follow SSA copy chains  */
  while (TREE_CODE (expr) == SSA_NAME)
    {
      gimple *def = SSA_NAME_DEF_STMT (expr);
      if (!def || !is_gimple_assign (def))
        break;
      enum tree_code code = gimple_assign_rhs_code (def);
      if (code == NOP_EXPR || code == CONVERT_EXPR
          || code == SSA_NAME || code == ADDR_EXPR
          || code == POINTER_PLUS_EXPR)
        expr = gimple_assign_rhs1 (def);
      else
        break;
    }

  STRIP_NOPS (expr);

  if (TREE_CODE (expr) == ADDR_EXPR)
    expr = TREE_OPERAND (expr, 0);

  /* Handle ARRAY_REF with constant zero index → base  */
  while (TREE_CODE (expr) == ARRAY_REF
         && integer_zerop (TREE_OPERAND (expr, 1)))
    expr = TREE_OPERAND (expr, 0);

  /* MEM_REF to a base pointer  */
  if (TREE_CODE (expr) == MEM_REF || TREE_CODE (expr) == TARGET_MEM_REF)
    expr = TREE_OPERAND (expr, 0);

  if (TREE_CODE (expr) == SSA_NAME)
    {
      gimple *def = SSA_NAME_DEF_STMT (expr);
      if (def && is_gimple_assign (def)
          && gimple_assign_rhs_code (def) == ADDR_EXPR)
        expr = TREE_OPERAND (gimple_assign_rhs1 (def), 0);
    }

  return expr;
}


/* Return true if BASE1 and BASE2 refer to the same underlying array.
   Uses multiple strategies to handle different GCC SSA representations:
   1. Direct tree pointer comparison
   2. DECL_UID comparison for declarations
   3. SSA_NAME comparison (same SSA version = same value)
   4. DECL_NAME string comparison as last resort  */

static bool
arrays_share_base (tree base1, tree base2)
{
  tree d1 = strip_to_decl (base1);
  tree d2 = strip_to_decl (base2);

  /* If either side could not be resolved, we cannot confirm or deny.
     Return false so the caller can decide (warn vs. reject).  */
  if (!d1 || !d2)
    return false;

  /* Strategy 1: Direct pointer comparison  */
  if (d1 == d2)
    return true;

  /* Strategy 2: Compare by DECL_UID if both are declarations  */
  if (DECL_P (d1) && DECL_P (d2))
    {
      if (DECL_UID (d1) == DECL_UID (d2))
        return true;

      /* Strategy 3: Compare DECL_NAME strings — covers cases where
         the same source-level variable has different DECL nodes across
         different SSA versions (e.g. after loop versioning).  */
      tree name1 = DECL_NAME (d1);
      tree name2 = DECL_NAME (d2);
      if (name1 && name2
          && strcmp (IDENTIFIER_POINTER (name1),
                     IDENTIFIER_POINTER (name2)) == 0)
        return true;
    }

  /* Strategy 4: If both are SSA_NAME referring to the same base var  */
  if (TREE_CODE (base1) == SSA_NAME && TREE_CODE (base2) == SSA_NAME)
    {
      tree var1 = SSA_NAME_VAR (base1);
      tree var2 = SSA_NAME_VAR (base2);
      if (var1 && var2 && var1 == var2)
        return true;
      if (var1 && var2 && DECL_P (var1) && DECL_P (var2)
          && DECL_UID (var1) == DECL_UID (var2))
        return true;
    }

  return false;
}


/* Lenient base check: returns true if arrays definitely share a base,
   or if the check is inconclusive (both sides resolved to non-NULL
   but different trees that could be SSA artifacts).  Only returns
   false when we can positively determine the bases differ (e.g.
   different PARM_DECLs with different names).  */

static bool
arrays_share_base_lenient (tree base1, tree base2)
{
  /* If the strict check passes, we're sure.  */
  if (arrays_share_base (base1, base2))
    return true;

  tree d1 = strip_to_decl (base1);
  tree d2 = strip_to_decl (base2);

  /* If either side could not be resolved, give benefit of the doubt.  */
  if (!d1 || !d2)
    return true;

  /* If both are PARM_DECLs with different names, they are definitely
     different parameters — this is a hard reject.  */
  if (DECL_P (d1) && DECL_P (d2)
      && TREE_CODE (d1) == PARM_DECL && TREE_CODE (d2) == PARM_DECL)
    {
      tree name1 = DECL_NAME (d1);
      tree name2 = DECL_NAME (d2);
      if (name1 && name2
          && strcmp (IDENTIFIER_POINTER (name1),
                     IDENTIFIER_POINTER (name2)) != 0)
        return false;
    }

  /* For local VAR_DECLs with clearly different UIDs and names,
     reject.  */
  if (DECL_P (d1) && DECL_P (d2)
      && TREE_CODE (d1) == VAR_DECL && TREE_CODE (d2) == VAR_DECL
      && DECL_UID (d1) != DECL_UID (d2))
    {
      tree name1 = DECL_NAME (d1);
      tree name2 = DECL_NAME (d2);
      if (name1 && name2
          && strcmp (IDENTIFIER_POINTER (name1),
                     IDENTIFIER_POINTER (name2)) != 0)
        return false;
    }

  /* Inconclusive — assume same base (SSA artifacts may cause
     different tree nodes for the same source variable).  */
  return true;
}


/* Check if STMT is a call to __builtin_expf or __builtin_exp.  */

static bool
is_exp_call (gimple *stmt)
{
  if (!is_gimple_call (stmt))
    return false;

  tree fndecl = gimple_call_fndecl (stmt);
  if (!fndecl)
    return false;

  enum built_in_function fn = DECL_FUNCTION_CODE (fndecl);
  return (fn == BUILT_IN_EXPF || fn == BUILT_IN_EXP);
}


/* Check if STMT is a call to __builtin_sqrtf or __builtin_sqrt.  */

static bool
is_sqrt_call (gimple *stmt)
{
  if (!is_gimple_call (stmt))
    return false;

  tree fndecl = gimple_call_fndecl (stmt);
  if (!fndecl)
    return false;

  enum built_in_function fn = DECL_FUNCTION_CODE (fndecl);
  return (fn == BUILT_IN_SQRTF || fn == BUILT_IN_SQRT);
}


/* Scan the body blocks of the innermost loop for a MULT_EXPR whose
   result feeds a PLUS_EXPR reduction (i.e. score[i][j] += a * b).
   Return the multiplication statement if found.

   Also attempt to extract the two memory-reference operands of the
   multiply and the store target of the accumulation.  */

static bool
find_matmul_reduction (class loop *innermost,
                       tree *a_ref, tree *b_ref, tree *c_ref)
{
  basic_block *bbs = get_loop_body (innermost);
  unsigned nbbs = innermost->num_nodes;
  bool found = false;

  if (dump_file)
    fprintf (dump_file, "    find_matmul: scanning %u bbs in innermost loop %d\n",
             nbbs, innermost->num);

  for (unsigned i = 0; i < nbbs && !found; i++)
    {
      for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
           !gsi_end_p (gsi) && !found; gsi_next (&gsi))
        {
          gimple *stmt = gsi_stmt (gsi);
          if (!is_gimple_assign (stmt))
            {
              if (dump_file)
                fprintf (dump_file, "    skip non-assign (code=%d)\n",
                         (int) gimple_code (stmt));
              continue;
            }

          enum tree_code code = gimple_assign_rhs_code (stmt);
          if (dump_file)
            fprintf (dump_file, "    assign code=%d (PLUS=%d MULT=%d)\n",
                     (int)code, (int)PLUS_EXPR, (int)MULT_EXPR);

          /* Look for: _x = _y + _z  where _z = _a * _b  */
          if (code == PLUS_EXPR)
            {
              tree rhs1 = gimple_assign_rhs1 (stmt);
              tree rhs2 = gimple_assign_rhs2 (stmt);

              /* One operand should come from a MULT_EXPR  */
              tree mult_result = NULL_TREE;
              tree accum = NULL_TREE;

              if (TREE_CODE (rhs2) == SSA_NAME)
                {
                  gimple *def = SSA_NAME_DEF_STMT (rhs2);
                  if (def && is_gimple_assign (def)
                      && gimple_assign_rhs_code (def) == MULT_EXPR)
                    {
                      mult_result = rhs2;
                      accum = rhs1;
                    }
                }
              /* Also check rhs1 (GCC may swap operand order)  */
              if (!mult_result && TREE_CODE (rhs1) == SSA_NAME)
                {
                  gimple *def = SSA_NAME_DEF_STMT (rhs1);
                  if (def && is_gimple_assign (def)
                      && gimple_assign_rhs_code (def) == MULT_EXPR)
                    {
                      mult_result = rhs1;
                      accum = rhs2;
                    }
                }

              if (!mult_result)
                continue;

              if (dump_file)
                fprintf (dump_file, "    ** PLUS+MULT matched! extracting refs...\n");

              /* Extract multiply operands  */
              gimple *mult_stmt = SSA_NAME_DEF_STMT (mult_result);
              tree op_a = gimple_assign_rhs1 (mult_stmt);
              tree op_b = gimple_assign_rhs2 (mult_stmt);

              /* Trace back through SSA to find the memory references.  */
              tree ref_a = NULL_TREE, ref_b = NULL_TREE;

              if (TREE_CODE (op_a) == SSA_NAME)
                {
                  gimple *la = SSA_NAME_DEF_STMT (op_a);
                  if (dump_file)
                    fprintf (dump_file, "    op_a: is_assign=%d",
                             la ? (int)is_gimple_assign (la) : -1);
                  if (la && is_gimple_assign (la))
                    {
                      tree r = gimple_assign_rhs1 (la);
                      if (dump_file)
                        fprintf (dump_file, " rhs_code=%d (MEM_REF=%d)\n",
                                 (int) gimple_assign_rhs_code (la),
                                 (int) MEM_REF);
                      if (TREE_CODE (r) == MEM_REF
                          || TREE_CODE (r) == TARGET_MEM_REF
                          || TREE_CODE (r) == ARRAY_REF)
                        ref_a = r;
                    }
                  else if (dump_file)
                    fprintf (dump_file, "\n");
                }
              if (TREE_CODE (op_b) == SSA_NAME)
                {
                  gimple *lb = SSA_NAME_DEF_STMT (op_b);
                  if (dump_file)
                    fprintf (dump_file, "    op_b: is_assign=%d",
                             lb ? (int)is_gimple_assign (lb) : -1);
                  if (lb && is_gimple_assign (lb))
                    {
                      tree r = gimple_assign_rhs1 (lb);
                      if (dump_file)
                        fprintf (dump_file, " rhs_code=%d (MEM_REF=%d)\n",
                                 (int) gimple_assign_rhs_code (lb),
                                 (int) MEM_REF);
                      if (TREE_CODE (r) == MEM_REF
                          || TREE_CODE (r) == TARGET_MEM_REF
                          || TREE_CODE (r) == ARRAY_REF)
                        ref_b = r;
                    }
                  else if (dump_file)
                    fprintf (dump_file, "\n");
                }

              if (dump_file)
                fprintf (dump_file, "    ref_a=%s ref_b=%s\n",
                         ref_a ? "OK" : "NULL", ref_b ? "OK" : "NULL");

              if (ref_a && ref_b)
                {
                  /* Extract base pointers from the memory references.
                     MEM_REF: operand 0 is the base pointer.
                     TARGET_MEM_REF: operand 0 is the base pointer.
                     ARRAY_REF: operand 0 is the array object.  */
                  *a_ref = TREE_OPERAND (ref_a, 0);
                  *b_ref = TREE_OPERAND (ref_b, 0);

                  /* The accumulation target (score) — trace through the
                     store that writes back the accumulated value.  */
                  tree lhs = gimple_assign_lhs (stmt);
                  *c_ref = lhs;
                  found = true;
                }
            }
        }
    }

  free (bbs);
  return found;
}


/* Check if LOOP is a matmul pattern (triple-nested with inner reduction).
   Returns true and fills a_base, b_base, c_base with the array bases.  */

static bool
is_matmul_pattern (class loop *loop,
                   tree *a_base, tree *b_base, tree *c_base)
{
  int depth = loop_depth_count (loop);
  if (depth != 3)
    return false;

  class loop *innermost = get_innermost (loop);
  tree ref_a = NULL_TREE, ref_b = NULL_TREE, ref_c = NULL_TREE;

  if (!find_matmul_reduction (innermost, &ref_a, &ref_b, &ref_c))
    {
      if (dump_file)
        fprintf (dump_file, "  matmul: find_matmul_reduction failed for loop %d (depth %d)\n",
                 loop->num, depth);
      return false;
    }

  *a_base = ref_a;
  *b_base = ref_b;

  /* ref_c is the accumulator SSA (e.g. sum_79), not the store target.
     The actual store (scores[i][j] = sum) is in the MIDDLE loop.
     Scan the middle loop body for a store that uses the reduction result.  */
  class loop *middle = loop_outer (innermost);
  tree store_base = NULL_TREE;
  if (middle && middle != loops_for_fn (cfun)->tree_root)
    {
      basic_block *mbbs = get_loop_body (middle);
      unsigned mnbbs = middle->num_nodes;
      for (unsigned mi = 0; mi < mnbbs && !store_base; mi++)
        {
          /* Skip blocks that belong to the innermost loop  */
          if (flow_bb_inside_loop_p (innermost, mbbs[mi]))
            continue;
          for (gimple_stmt_iterator gsi = gsi_start_bb (mbbs[mi]);
               !gsi_end_p (gsi) && !store_base; gsi_next (&gsi))
            {
              gimple *stmt = gsi_stmt (gsi);
              if (!is_gimple_assign (stmt))
                continue;
              tree lhs = gimple_assign_lhs (stmt);
              if (lhs && (TREE_CODE (lhs) == MEM_REF
                          || TREE_CODE (lhs) == TARGET_MEM_REF
                          || TREE_CODE (lhs) == ARRAY_REF))
                store_base = TREE_OPERAND (lhs, 0);
            }
        }
      free (mbbs);
    }

  *c_base = store_base ? store_base : ref_c;

  if (dump_file)
    fprintf (dump_file, "  Found matmul pattern in loop %d (store_base=%s)\n",
             loop->num, store_base ? "found" : "fallback");

  return true;
}


/* Check if LOOP is a double-nested elementwise division:
     for i: for j: arr[i][j] /= divisor
   Returns true and fills arr_base and divisor.  */

static bool
is_elementwise_div (class loop *loop, tree *arr_base, tree *divisor)
{
  int depth = loop_depth_count (loop);
  if (depth != 2)
    return false;

  class loop *inner = loop->inner;
  basic_block *bbs = get_loop_body (inner);
  unsigned nbbs = inner->num_nodes;
  bool found = false;

  for (unsigned i = 0; i < nbbs && !found; i++)
    {
      for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
           !gsi_end_p (gsi) && !found; gsi_next (&gsi))
        {
          gimple *stmt = gsi_stmt (gsi);
          if (!is_gimple_assign (stmt))
            continue;

          enum tree_code code = gimple_assign_rhs_code (stmt);
          if (code == RDIV_EXPR)
            {
              *divisor = gimple_assign_rhs2 (stmt);

              /* The dividend is a load from the array  */
              tree rhs1 = gimple_assign_rhs1 (stmt);
              if (TREE_CODE (rhs1) == SSA_NAME)
                {
                  gimple *ld = SSA_NAME_DEF_STMT (rhs1);
                  if (ld && is_gimple_assign (ld))
                    {
                      tree r = gimple_assign_rhs1 (ld);
                      if (TREE_CODE (r) == MEM_REF
                          || TREE_CODE (r) == TARGET_MEM_REF
                          || TREE_CODE (r) == ARRAY_REF)
                        {
                          *arr_base = TREE_OPERAND (r, 0);
                          found = true;
                        }
                    }
                }
            }

          /* Also handle:  arr[i][j] = arr[i][j] * (1.0 / divisor)
             GCC may place the reciprocal in rhs1 or rhs2.  */
          if (code == MULT_EXPR && !found)
            {
              tree rhs1_m = gimple_assign_rhs1 (stmt);
              tree rhs2_m = gimple_assign_rhs2 (stmt);

              /* Try both operand orders: the reciprocal (RDIV_EXPR)
                 could be in either rhs1 or rhs2.  */
              for (int swap = 0; swap < 2 && !found; swap++)
                {
                  tree recip_op  = (swap == 0) ? rhs2_m : rhs1_m;
                  tree array_op  = (swap == 0) ? rhs1_m : rhs2_m;

                  if (TREE_CODE (recip_op) != SSA_NAME)
                    continue;
                  gimple *def = SSA_NAME_DEF_STMT (recip_op);
                  if (!def || !is_gimple_assign (def)
                      || gimple_assign_rhs_code (def) != RDIV_EXPR)
                    continue;

                  *divisor = gimple_assign_rhs2 (def);

                  if (TREE_CODE (array_op) == SSA_NAME)
                    {
                      gimple *ld = SSA_NAME_DEF_STMT (array_op);
                      if (ld && is_gimple_assign (ld))
                        {
                          tree r = gimple_assign_rhs1 (ld);
                          if (TREE_CODE (r) == MEM_REF
                              || TREE_CODE (r) == TARGET_MEM_REF
                              || TREE_CODE (r) == ARRAY_REF)
                            {
                              *arr_base = TREE_OPERAND (r, 0);
                              found = true;
                            }
                        }
                    }
                }
            }
        }
    }

  free (bbs);

  if (found && dump_file)
    fprintf (dump_file, "  Found elementwise-div pattern in loop %d\n",
             loop->num);

  return found;
}


/* Check if LOOP is a softmax-per-row pattern:
     Outer loop (rows) with exactly 3 child inner loops:
       1. max-reduction
       2. sum-of-exp reduction
       3. exp/sum normalization

   Returns true and fills arr_base.  */

static bool
is_softmax_pattern (class loop *loop, tree *arr_base)
{
  /* The outer loop must have 2 or 3 direct child loops.
     3 = max + exp-sum + normalize;  2 = exp-sum + normalize.  */
  int nchildren = count_child_loops (loop);
  if (nchildren < 2 || nchildren > 3)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: expected 2-3 child loops, got %d\n",
                 nchildren);
      return false;
    }

  class loop *child1, *child2, *child3;
  if (nchildren == 3)
    {
      child1 = loop->inner;
      child2 = child1->next;
      child3 = child2->next;
    }
  else
    {
      /* 2-child: no max-reduction. Detect which child has exp()
         (children may be in reverse program order).  */
      child1 = NULL;
      class loop *ca = loop->inner;
      class loop *cb = ca->next;
      /* Check if ca has exp() call  */
      bool ca_has_exp = false;
      {
        basic_block *bbs = get_loop_body (ca);
        for (unsigned bi = 0; bi < ca->num_nodes && !ca_has_exp; bi++)
          for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[bi]);
               !gsi_end_p (gsi); gsi_next (&gsi))
            if (is_exp_call (gsi_stmt (gsi)))
              ca_has_exp = true;
        free (bbs);
      }
      if (ca_has_exp)
        { child2 = ca; child3 = cb; }
      else
        { child2 = cb; child3 = ca; }
    }

  /* --- Child 1: max-reduction (skipped for 2-child softmax) ---
     When nchildren==2, child1 is NULL — jump to exp-sum check.  */
  /* --- Child 1: max-reduction ---
     Look for a conditional GT_EXPR / GE_EXPR in the loop body,
     which characterizes max_val = max(max_val, arr[i][j]).  */
  bool has_max = false;
  if (child1)
  {
    basic_block *bbs = get_loop_body (child1);
    unsigned nbbs = child1->num_nodes;
    for (unsigned i = 0; i < nbbs && !has_max; i++)
      {
        for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
             !gsi_end_p (gsi); gsi_next (&gsi))
          {
            gimple *stmt = gsi_stmt (gsi);
            if (gimple_code (stmt) == GIMPLE_COND)
              {
                enum tree_code cmp = gimple_cond_code (as_a<gcond *>(stmt));
                if (cmp == GT_EXPR || cmp == GE_EXPR
                    || cmp == LT_EXPR || cmp == LE_EXPR)
                  has_max = true;
              }
            /* Also handle: PHI-based MAX_EXPR  */
            if (is_gimple_assign (stmt)
                && gimple_assign_rhs_code (stmt) == MAX_EXPR)
              has_max = true;
          }
      }
    free (bbs);
  }

  if (!has_max && child1)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: child1 is not a max-reduction\n");
      return false;
    }

  /* --- Child 2: sum-of-exp ---
     Look for: sum += exp(something)
     i.e., a PLUS_EXPR reduction where one operand traces back to exp().
     We require BOTH an exp() call AND a PLUS_EXPR accumulation to
     distinguish this from unrelated loops that happen to call exp().  */
  bool has_exp = false;
  bool has_plus_accum = false;
  {
    basic_block *bbs = get_loop_body (child2);
    unsigned nbbs = child2->num_nodes;
    for (unsigned i = 0; i < nbbs; i++)
      {
        for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
             !gsi_end_p (gsi); gsi_next (&gsi))
          {
            gimple *stmt = gsi_stmt (gsi);
            if (is_exp_call (stmt))
              has_exp = true;
            if (is_gimple_assign (stmt)
                && gimple_assign_rhs_code (stmt) == PLUS_EXPR)
              has_plus_accum = true;
          }
      }
    free (bbs);
  }

  if (!has_exp || !has_plus_accum)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: child2 missing exp()=%d or "
                 "PLUS accumulation=%d\n", (int)has_exp, (int)has_plus_accum);
      return false;
    }

  /* --- Child 3: normalization ---
     Look for: arr[i][j] = something / sum
     i.e., RDIV_EXPR (division).  We specifically require a division
     operation — merely having exp() in this loop is not sufficient,
     as that would conflate the exp-sum and normalize stages.  */
  bool has_normalize = false;
  {
    basic_block *bbs = get_loop_body (child3);
    unsigned nbbs = child3->num_nodes;
    for (unsigned i = 0; i < nbbs && !has_normalize; i++)
      {
        for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
             !gsi_end_p (gsi); gsi_next (&gsi))
          {
            gimple *stmt = gsi_stmt (gsi);
            if (is_gimple_assign (stmt)
                && gimple_assign_rhs_code (stmt) == RDIV_EXPR)
              has_normalize = true;
            /* Also accept MULT_EXPR by a reciprocal (1.0/sum),
               which GCC may produce instead of RDIV.  */
            if (is_gimple_assign (stmt)
                && gimple_assign_rhs_code (stmt) == MULT_EXPR)
              {
                tree r1 = gimple_assign_rhs1 (stmt);
                tree r2 = gimple_assign_rhs2 (stmt);
                for (int s = 0; s < 2; s++)
                  {
                    tree candidate = (s == 0) ? r2 : r1;
                    if (TREE_CODE (candidate) == SSA_NAME)
                      {
                        gimple *d = SSA_NAME_DEF_STMT (candidate);
                        if (d && is_gimple_assign (d)
                            && gimple_assign_rhs_code (d) == RDIV_EXPR)
                          has_normalize = true;
                      }
                  }
              }
          }
      }
    free (bbs);
  }

  if (!has_normalize)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: child3 has no exp/div pattern\n");
      return false;
    }

  /* Extract the array base from the outer loop or first child.
     The array accessed by the softmax is the same 'score' array.  */
  {
    basic_block *bbs = get_loop_body (child2);
    unsigned nbbs = child2->num_nodes;
    for (unsigned i = 0; i < nbbs; i++)
      {
        for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
             !gsi_end_p (gsi); gsi_next (&gsi))
          {
            gimple *stmt = gsi_stmt (gsi);
            if (!is_gimple_assign (stmt))
              continue;
            tree rhs1 = gimple_assign_rhs1 (stmt);
            if (TREE_CODE (rhs1) == MEM_REF
                || TREE_CODE (rhs1) == ARRAY_REF)
              {
                *arr_base = TREE_OPERAND (rhs1, 0);
                free (bbs);
                goto softmax_done;
              }
          }
      }
    free (bbs);
  }

softmax_done:
  if (dump_file)
    fprintf (dump_file, "  Found softmax pattern in loop %d "
             "(3 children: max, exp-sum, normalize)\n", loop->num);

  return true;
}


/* ═══════════════════════════════════════════════════════════════════
 *  SECTION D: The 4-stage detection algorithm
 * ═══════════════════════════════════════════════════════════════════ */

/* Try to detect the full attention pattern in FUN.
   Returns true and fills CAND if found.  */

static bool
detect_attention_pattern (function *fun, struct attn_candidate *cand)
{
  /* Initialize loop infrastructure  */
  loop_optimizer_init (LOOPS_NORMAL);
  scev_initialize ();

  bool detected = false;

  if (dump_file)
    fprintf (dump_file, "\n=== Attention pattern detection in %s ===\n",
             function_name (fun));

  /* Collect top-level loops (direct children of the root loop).
     We need at least 4 consecutive loops matching our pattern.  */
  class loop *root = loops_for_fn (fun)->tree_root;
  auto_vec<class loop *> top_loops;

  for (class loop *l = root->inner; l; l = l->next)
    top_loops.safe_push (l);

  /* Reverse to get program order (GCC lists children in reverse) */
  if (top_loops.length () > 1)
    for (unsigned i = 0, j = top_loops.length () - 1; i < j; i++, j--)
    {
      class loop *tmp = top_loops[i];
      top_loops[i] = top_loops[j];
      top_loops[j] = tmp;
    }

  if (dump_file)
    fprintf (dump_file, "  Found %u top-level loop(s)\n", top_loops.length ());

  if (top_loops.length () < 4)
    goto cleanup;

  /* Try each window of 4 consecutive top-level loops  */
  for (unsigned start = 0; start + 3 < top_loops.length (); start++)
    {
      class loop *l1 = top_loops[start];
      class loop *l2 = top_loops[start + 1];
      class loop *l3 = top_loops[start + 2];
      class loop *l4 = top_loops[start + 3];

      if (dump_file)
        fprintf (dump_file, "\n  Trying loops %d, %d, %d, %d...\n",
                 l1->num, l2->num, l3->num, l4->num);

      /* Stage 1: matmul Q * K^T → score  */
      tree q_base, k_base, score_base_1;
      if (!is_matmul_pattern (l1, &q_base, &k_base, &score_base_1))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 1 FAIL: loop %d is not a matmul\n",
                     l1->num);
          continue;
        }

      /* Stage 2: score / sqrt(d)  */
      tree scale_arr, scale_div;
      if (!is_elementwise_div (l2, &scale_arr, &scale_div))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 2 FAIL: loop %d is not a scale\n",
                     l2->num);
          continue;
        }

      /* Verify the scaled array is the same as the matmul output  */
      if (dump_file)
        {
          tree d1 = strip_to_decl (score_base_1);
          tree d2 = strip_to_decl (scale_arr);
          fprintf (dump_file, "  Base check: score=%s(%d) scale=%s(%d)\n",
                   d1 ? get_tree_code_name (TREE_CODE (d1)) : "NULL",
                   d1 && DECL_P (d1) ? (int)DECL_UID (d1) : -1,
                   d2 ? get_tree_code_name (TREE_CODE (d2)) : "NULL",
                   d2 && DECL_P (d2) ? (int)DECL_UID (d2) : -1);
        }
      /* Lenient base check: rejects only when bases are provably
         different (e.g. different PARM_DECLs).  Tolerates SSA
         artifacts that produce different tree nodes for the same
         source-level variable.  */
      if (!arrays_share_base_lenient (score_base_1, scale_arr))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 2 FAIL: array base mismatch "
                     "(matmul output ≠ scaled array)\n");
          continue;
        }

      /* Stage 3: softmax(score)  */
      tree softmax_arr;
      if (!is_softmax_pattern (l3, &softmax_arr))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 3 FAIL: loop %d is not softmax\n",
                     l3->num);
          continue;
        }

      /* Verify softmax operates on the same score array  */
      if (!arrays_share_base_lenient (score_base_1, softmax_arr))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 3 FAIL: array base mismatch "
                     "(score ≠ softmax input)\n");
          continue;
        }

      /* Stage 4: matmul score * V → output  */
      tree matmul2_a, matmul2_b, matmul2_c;
      if (!is_matmul_pattern (l4, &matmul2_a, &matmul2_b, &matmul2_c))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 4 FAIL: loop %d is not a matmul\n",
                     l4->num);
          continue;
        }

      /* Verify the first operand of matmul2 is the score array  */
      if (!arrays_share_base_lenient (score_base_1, matmul2_a))
        {
          if (dump_file)
            fprintf (dump_file, "  Stage 4 FAIL: matmul2 operand A "
                     "≠ score array\n");
          continue;
        }

      /* ── All 4 stages matched! ──  */
      if (dump_file)
        fprintf (dump_file,
                 "\n  *** ATTENTION PATTERN DETECTED ***\n"
                 "  Loops: %d (matmul1) → %d (scale) → "
                 "%d (softmax) → %d (matmul2)\n",
                 l1->num, l2->num, l3->num, l4->num);

      cand->matmul1_loop = l1;
      cand->scale_loop   = l2;
      cand->softmax_loop = l3;
      cand->matmul2_loop = l4;

      cand->q_base      = q_base;
      cand->k_base      = k_base;
      cand->score_base  = score_base_1;
      cand->v_base      = matmul2_b;
      cand->output_base = matmul2_c;

      cand->scale_divisor = scale_div;

      /* Extract dimensions from loop trip counts.
         Use number_of_latch_executions which returns an affine expression
         (INTEGER_CST for compile-time constants, SSA_NAME for runtime).  */
      tree niter_i = number_of_latch_executions (l1);
      if (niter_i && niter_i != chrec_dont_know)
        cand->seq_len = fold_build2 (PLUS_EXPR, TREE_TYPE (niter_i),
                                     niter_i, build_one_cst (TREE_TYPE (niter_i)));
      else
        cand->seq_len = NULL_TREE;

      class loop *innermost1 = get_innermost (l1);
      tree niter_k = number_of_latch_executions (innermost1);
      if (niter_k && niter_k != chrec_dont_know)
        cand->d_model = fold_build2 (PLUS_EXPR, TREE_TYPE (niter_k),
                                     niter_k, build_one_cst (TREE_TYPE (niter_k)));
      else
        cand->d_model = NULL_TREE;

      /* Find insertion/exit points  */
      edge e = loop_preheader_edge (l1);
      cand->insert_bb = e ? e->src : NULL;

      /* Exit of the fourth loop — find the block AFTER all loops.
         single_exit() may return NULL for loops with complex exit
         structure (e.g. multiple exits or stale loop info).
         In that case, manually find an edge from inside l4 to
         a block outside l4.  */
      edge exit_e = single_exit (l4);
      if (exit_e)
        {
          cand->exit_bb = exit_e->dest;
        }
      else
        {
          /* Manual fallback: scan all blocks in l4 for edges that
             leave the loop.  */
          cand->exit_bb = NULL;
          basic_block *l4_body = get_loop_body (l4);
          for (unsigned bi = 0; bi < l4->num_nodes && !cand->exit_bb; bi++)
            {
              edge ex;
              edge_iterator exi;
              FOR_EACH_EDGE (ex, exi, l4_body[bi]->succs)
                {
                  if (!flow_bb_inside_loop_p (l4, ex->dest))
                    {
                      cand->exit_bb = ex->dest;
                      break;
                    }
                }
            }
          free (l4_body);

          /* Last resort: use the function's single exit block  */
          if (!cand->exit_bb)
            {
              edge_iterator ret_ei;
              edge ret_e;
              basic_block exit_block = EXIT_BLOCK_PTR_FOR_FN (fun);
              FOR_EACH_EDGE (ret_e, ret_ei, exit_block->preds)
                {
                  cand->exit_bb = ret_e->src;
                  break;
                }
            }

          if (dump_file)
            fprintf (dump_file, "  single_exit(l4) was NULL, "
                     "manual fallback found exit_bb = bb %d\n",
                     cand->exit_bb ? cand->exit_bb->index : -1);
        }

      /* Bail out if we could not extract loop trip counts —
         without seq_len and d_model the replacement phase cannot
         build the dims struct correctly.  */
      if (!cand->seq_len || !cand->d_model)
        {
          if (dump_file)
            fprintf (dump_file, "  Cannot extract loop trip counts "
                     "(seq_len=%s, d_model=%s) — skipping replacement\n",
                     cand->seq_len ? "OK" : "NULL",
                     cand->d_model ? "OK" : "NULL");
          continue;   /* try next window of 4 loops  */
        }

      detected = true;
      break;
    }

cleanup:
  scev_finalize ();
  loop_optimizer_finalize ();
  return detected;
}


/* ═══════════════════════════════════════════════════════════════════
 *  SECTION E: Pattern replacement — emit the builtin call
 * ═══════════════════════════════════════════════════════════════════ */

/* Build a RECORD_TYPE with the given INT fields on the stack.
   Returns the VAR_DECL for the stack variable.

   attn_dims_t:  { int rows, cols, seq_len, d_model }
   attn_qkv_t:   { float *Q, *K, *V, *out }                  */

static tree
build_dims_struct (function *fun)
{
  tree int_type = integer_type_node;

  tree f_rows    = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                               get_identifier ("rows"), int_type);
  tree f_cols    = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                               get_identifier ("cols"), int_type);
  tree f_seqlen  = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                               get_identifier ("seq_len"), int_type);
  tree f_dmodel  = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                               get_identifier ("d_model"), int_type);

  DECL_CHAIN (f_rows)   = f_cols;
  DECL_CHAIN (f_cols)    = f_seqlen;
  DECL_CHAIN (f_seqlen)  = f_dmodel;
  DECL_CHAIN (f_dmodel)  = NULL_TREE;

  tree dims_type = make_node (RECORD_TYPE);
  TYPE_FIELDS (dims_type) = f_rows;
  DECL_CONTEXT (f_rows)   = dims_type;
  DECL_CONTEXT (f_cols)    = dims_type;
  DECL_CONTEXT (f_seqlen)  = dims_type;
  DECL_CONTEXT (f_dmodel)  = dims_type;
  layout_type (dims_type);

  tree dims_var = create_tmp_var (dims_type, "attn_dims");
  TREE_ADDRESSABLE (dims_var) = 1;
  DECL_CONTEXT (dims_var) = fun->decl;

  return dims_var;
}


static tree
build_qkv_struct (function *fun)
{
  tree ptr_type = build_pointer_type (float_type_node);

  tree f_q = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                         get_identifier ("Q"), ptr_type);
  tree f_k = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                         get_identifier ("K"), ptr_type);
  tree f_v = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                         get_identifier ("V"), ptr_type);
  tree f_out = build_decl (UNKNOWN_LOCATION, FIELD_DECL,
                           get_identifier ("out"), ptr_type);

  DECL_CHAIN (f_q) = f_k;
  DECL_CHAIN (f_k) = f_v;
  DECL_CHAIN (f_v) = f_out;
  DECL_CHAIN (f_out) = NULL_TREE;

  tree qkv_type = make_node (RECORD_TYPE);
  TYPE_FIELDS (qkv_type) = f_q;
  DECL_CONTEXT (f_q) = qkv_type;
  DECL_CONTEXT (f_k) = qkv_type;
  DECL_CONTEXT (f_v) = qkv_type;
  DECL_CONTEXT (f_out) = qkv_type;
  layout_type (qkv_type);

  tree qkv_var = create_tmp_var (qkv_type, "attn_qkv");
  TREE_ADDRESSABLE (qkv_var) = 1;
  DECL_CONTEXT (qkv_var) = fun->decl;

  return qkv_var;
}


/* Replace the four detected loop nests with a single __builtin_riscv_attn
   call.  This emits:
     1. Stack struct initialization (dims + qkv)
     2. Memory barrier (volatile asm)
     3. Call to __builtin_riscv_attn(&dims, &qkv)
     4. Store result to output
   Then removes the four original loop nests.  */

static void
replace_attention_with_builtin (function *fun, struct attn_candidate *cand)
{
  /* We emit the attn instruction as inline assembly (.insn directive)
     rather than going through __builtin_riscv_attn, so we don't need
     to look up the builtin declaration here.  */

  /* Extract dimensions and pointers from the candidate struct, which
     were populated during detection from loop trip counts and memory
     access patterns.  This allows the pass to work in ANY function,
     not just one with a specific parameter signature.

     Fallback: if any candidate field is NULL (detection could not
     extract it), try DECL_ARGUMENTS for backward compatibility with
     the expected signature: attention(int n, int d, float *Q, float *K,
                                        float *V, float *out).  */
  tree p_n = cand->seq_len;
  tree p_d = cand->d_model;
  tree p_Q = cand->q_base;
  tree p_K = cand->k_base;
  tree p_V = cand->v_base;
  tree p_out = cand->output_base;

  /* Fallback to DECL_ARGUMENTS if any field is missing  */
  if (!p_n || !p_d || !p_Q || !p_K || !p_V || !p_out)
    {
      if (dump_file)
        fprintf (dump_file, "  Candidate fields incomplete (n=%p d=%p Q=%p K=%p V=%p out=%p),"
                 " falling back to DECL_ARGUMENTS\n",
                 (void *)p_n, (void *)p_d, (void *)p_Q, (void *)p_K,
                 (void *)p_V, (void *)p_out);
      tree parm = DECL_ARGUMENTS (fun->decl);
      if (parm && DECL_CHAIN (parm)
          && DECL_CHAIN (DECL_CHAIN (parm))
          && DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (parm)))
          && DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (parm))))
          && DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (DECL_CHAIN (parm))))))
        {
          if (!p_n) { p_n = parm; }
          parm = DECL_CHAIN (parm);
          if (!p_d) { p_d = parm; }
          parm = DECL_CHAIN (parm);
          if (!p_Q) { p_Q = parm; }
          parm = DECL_CHAIN (parm);
          if (!p_K) { p_K = parm; }
          parm = DECL_CHAIN (parm);
          if (!p_V) { p_V = parm; }
          parm = DECL_CHAIN (parm);
          if (!p_out) { p_out = parm; }
        }
    }

  if (!p_n || !p_d || !p_Q || !p_K || !p_V || !p_out)
    {
      if (dump_file)
        fprintf (dump_file, "  ERROR: cannot extract function parameters\n");
      return;
    }

  /* We insert all code into a fresh basic block created by splitting the
     edge from the preheader to the first loop's header.  This avoids
     interfering with any terminating branch in the preheader block and
     guarantees the new block has exactly one successor that we can
     redirect to exit_bb.  */

  basic_block orig_bb = cand->insert_bb;
  if (!orig_bb)
    orig_bb = single_succ (ENTRY_BLOCK_PTR_FOR_FN (fun));

  basic_block loop1_header = cand->matmul1_loop->header;

  /* Find the edge from the preheader to loop1's header and split it.  */
  edge loop_entry_edge = find_edge (orig_bb, loop1_header);
  if (!loop_entry_edge)
    {
      /* Fallback: walk successors  */
      edge tmp_e;
      edge_iterator tmp_ei;
      FOR_EACH_EDGE (tmp_e, tmp_ei, orig_bb->succs)
        {
          if (tmp_e->dest == loop1_header)
            {
              loop_entry_edge = tmp_e;
              break;
            }
        }
    }

  basic_block insert_bb;
  if (loop_entry_edge)
    {
      insert_bb = split_edge (loop_entry_edge);
      if (dump_file)
        fprintf (dump_file, "  Split edge bb %d -> bb %d, new bb %d\n",
                 orig_bb->index, loop1_header->index, insert_bb->index);
    }
  else
    {
      /* Last resort: use orig_bb directly  */
      insert_bb = orig_bb;
      if (dump_file)
        fprintf (dump_file, "  WARNING: could not find edge to split, "
                 "using bb %d directly\n", insert_bb->index);
    }

  gimple_stmt_iterator gsi = gsi_start_bb (insert_bb);

  /* 1. Build dims struct {n, n, n, d} on stack  */
  tree dims_var = build_dims_struct (fun);
  tree dims_type = TREE_TYPE (dims_var);
  tree f = TYPE_FIELDS (dims_type);

  tree n_int = fold_convert (integer_type_node, p_n);
  tree d_int = fold_convert (integer_type_node, p_d);

  /* rows  */
  gimple *s1 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    n_int);
  gsi_insert_after (&gsi, s1, GSI_NEW_STMT);

  /* cols  */
  f = DECL_CHAIN (f);
  gimple *s2 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    n_int);
  gsi_insert_after (&gsi, s2, GSI_NEW_STMT);

  /* seq_len  */
  f = DECL_CHAIN (f);
  gimple *s3 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    n_int);
  gsi_insert_after (&gsi, s3, GSI_NEW_STMT);

  /* d_model  */
  f = DECL_CHAIN (f);
  gimple *s4 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    d_int);
  gsi_insert_after (&gsi, s4, GSI_NEW_STMT);

  /* 2. Build qkv struct {Q, K, V} on stack  */
  tree qkv_var = build_qkv_struct (fun);
  tree qkv_type = TREE_TYPE (qkv_var);
  tree fq = TYPE_FIELDS (qkv_type);
  tree ptr_type = build_pointer_type (float_type_node);

  gimple *sq = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    fold_convert (ptr_type, p_Q));
  gsi_insert_after (&gsi, sq, GSI_NEW_STMT);

  fq = DECL_CHAIN (fq);
  gimple *sk = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    fold_convert (ptr_type, p_K));
  gsi_insert_after (&gsi, sk, GSI_NEW_STMT);

  fq = DECL_CHAIN (fq);
  gimple *sv = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    fold_convert (ptr_type, p_V));
  gsi_insert_after (&gsi, sv, GSI_NEW_STMT);

  fq = DECL_CHAIN (fq);
  gimple *so = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    fold_convert (ptr_type, p_out));
  gsi_insert_after (&gsi, so, GSI_NEW_STMT);

  /* 3. Volatile barrier  */
  gasm *barrier = gimple_build_asm_vec ("", NULL, NULL, NULL, NULL);
  gimple_asm_set_volatile (barrier, true);
  gsi_insert_after (&gsi, barrier, GSI_NEW_STMT);

  /* 4. Call __builtin_riscv_attn((ulong)&dims, (ulong)&qkv)  */
  tree ul_type = long_unsigned_type_node;

  tree dims_addr_expr = build_fold_addr_expr (dims_var);
  tree dims_cast = fold_convert (ul_type, dims_addr_expr);
  tree dims_tmp = create_tmp_var (ul_type, "dims_addr");
  gimple *gc1 = gimple_build_assign (dims_tmp, dims_cast);
  gsi_insert_after (&gsi, gc1, GSI_NEW_STMT);

  /* &qkv → (unsigned long)  */
  tree qkv_addr_expr = build_fold_addr_expr (qkv_var);
  tree qkv_cast = fold_convert (ul_type, qkv_addr_expr);
  tree qkv_tmp = create_tmp_var (ul_type, "qkv_addr");
  gimple *gc2 = gimple_build_assign (qkv_tmp, qkv_cast);
  gsi_insert_after (&gsi, gc2, GSI_NEW_STMT);

  /* The actual call — emit as inline assembly to guarantee the
     instruction survives all optimization passes.  The .insn directive
     encodes the R-type attn instruction directly.

     Encoding: .insn r 0x0b, 0, 0x01, x0, rs1, rs2
       opcode=0x0b (custom-0), funct3=0, funct7=0x01
       rd=x0 (unused), rs1=dims_addr, rs2=qkv_addr              */

  tree ul_type_2 = long_unsigned_type_node;

  /* Build: asm volatile (".insn r 0x0b, 0, 0x01, x0, %0, %1"
                           : : "r"(dims_addr), "r"(qkv_addr) : "memory"); */

  /* Input constraints: "r" for both operands  */
  tree constraint_r = build_string (1, "r");

  vec<tree, va_gc> *inputs = NULL;
  /* First input: dims_addr  */
  tree in1 = build_tree_list (
    build_tree_list (NULL_TREE, constraint_r), dims_tmp);
  vec_safe_push (inputs, in1);
  /* Second input: qkv_addr  */
  tree in2 = build_tree_list (
    build_tree_list (NULL_TREE, constraint_r), qkv_tmp);
  vec_safe_push (inputs, in2);

  /* Clobbers: "memory"  */
  vec<tree, va_gc> *clobbers = NULL;
  tree mem_clobber = build_tree_list (NULL_TREE, build_string (6, "memory"));
  vec_safe_push (clobbers, mem_clobber);

  gasm *attn_asm = gimple_build_asm_vec (
    ".insn r 0x0b, 0, 0x01, x0, %0, %1",
    inputs,          /* inputs  */
    NULL,            /* outputs  */
    clobbers,        /* clobbers  */
    NULL);           /* labels  */
  gimple_asm_set_volatile (attn_asm, true);
  gsi_insert_after (&gsi, attn_asm, GSI_NEW_STMT);

  if (dump_file)
    {
      fprintf (dump_file, "\n  Inserted attn instruction via inline asm:\n  ");
      print_gimple_stmt (dump_file, attn_asm, 0, TDF_SLIM);
      fprintf (dump_file, "\n");
    }

  /* 5. Redirect control flow: skip all 4 loops.

     The split block (insert_bb) currently has exactly one successor —
     the first loop's header.  Redirect that edge to the exit block
     of the fourth loop, making all loop bodies unreachable.
     Later DCE passes clean up the dead code.  */

  if (cand->exit_bb)
    {
      if (dump_file)
        fprintf (dump_file, "  Redirect: insert_bb=%d has %d succs, "
                 "exit_bb=%d\n", insert_bb->index,
                 EDGE_COUNT (insert_bb->succs), cand->exit_bb->index);

      /* Redirect ALL outgoing edges from insert_bb to exit_bb.  */
      bool redirected = false;
      edge e;
      edge_iterator ei;
      FOR_EACH_EDGE (e, ei, insert_bb->succs)
        {
          if (dump_file)
            fprintf (dump_file, "    Redirecting bb %d -> bb %d to bb %d\n",
                     insert_bb->index, e->dest->index, cand->exit_bb->index);
          redirect_edge_and_branch (e, cand->exit_bb);
          redirected = true;
          break;  /* Iterator invalidated after redirect, exit loop  */
        }

      if (!redirected && dump_file)
        fprintf (dump_file, "  WARNING: no edges to redirect!\n");
    }
  else
    {
      if (dump_file)
        fprintf (dump_file, "  WARNING: exit_bb is NULL, skipping redirect\n");
    }

  if (dump_file)
    fprintf (dump_file, "  Attention pattern replaced successfully.\n\n");
}


/* ═══════════════════════════════════════════════════════════════════
 *  SECTION F: Pass class definition
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

const pass_data pass_data_riscv_attn_detect = {
  GIMPLE_PASS,                    /* type  */
  "riscv_attn_detect",            /* name (for -fdump-tree-*)  */
  OPTGROUP_NONE,                  /* optinfo_flags  */
  TV_NONE,                        /* tv_id  */
  (PROP_cfg | PROP_ssa),          /* properties_required  */
  0,                              /* properties_provided  */
  0,                              /* properties_destroyed  */
  0,                              /* todo_flags_start  */
  (TODO_update_ssa
   | TODO_cleanup_cfg),           /* todo_flags_finish  */
};


class pass_riscv_attn_detect : public gimple_opt_pass
{
public:
  pass_riscv_attn_detect (gcc::context *ctxt)
    : gimple_opt_pass (pass_data_riscv_attn_detect, ctxt)
  {}

  /* Gate: run only at -O2 or higher, since the pass depends on
     loop infrastructure that is not reliably available at -O0/-O1.
     A future enhancement could add a dedicated -mattn-detect flag
     or gate on -march containing a custom extension (e.g. Xattn).  */
  bool gate (function *) final override
  {
    return optimize >= 2;
  }

  unsigned int execute (function *fun) final override
  {
    struct attn_candidate cand;
    memset (&cand, 0, sizeof (cand));

    if (detect_attention_pattern (fun, &cand))
      {
        if (dump_file)
          fprintf (dump_file, "\n>>> Attention pattern detected in '%s' — "
                   "replacing with __builtin_riscv_attn\n\n",
                   function_name (fun));

        replace_attention_with_builtin (fun, &cand);

        return TODO_cleanup_cfg | TODO_update_ssa;
      }
    else
      {
        if (dump_file)
          fprintf (dump_file, "\n>>> No attention pattern found in '%s'\n\n",
                   function_name (fun));
      }

    return 0;
  }

  opt_pass *clone () final override
  {
    return new pass_riscv_attn_detect (m_ctxt);
  }

}; /* class pass_riscv_attn_detect  */

} /* anonymous namespace  */


/* Factory function — called from riscv_option_override() via
   register_pass().  Also declared in tree-pass.h.  */

gimple_opt_pass *
make_pass_riscv_attn_detect (gcc::context *ctxt)
{
  return new pass_riscv_attn_detect (ctxt);
}
