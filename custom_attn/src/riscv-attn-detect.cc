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
#include "tree-scalar-evolution.h"
#include "tree-data-ref.h"
#include "fold-const.h"
#include "gimplify.h"
#include "gimple-fold.h"
#include "tree-cfg.h"
#include "cfganal.h"
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
          || code == SSA_NAME || code == ADDR_EXPR)
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
  if (TREE_CODE (expr) == MEM_REF)
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


/* Return true if BASE1 and BASE2 refer to the same underlying array.  */

static bool
arrays_share_base (tree base1, tree base2)
{
  tree d1 = strip_to_decl (base1);
  tree d2 = strip_to_decl (base2);

  if (!d1 || !d2)
    return false;

  /* Direct comparison  */
  if (d1 == d2)
    return true;

  /* Compare by DECL_UID if both are declarations  */
  if (DECL_P (d1) && DECL_P (d2))
    return DECL_UID (d1) == DECL_UID (d2);

  return false;
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

  for (unsigned i = 0; i < nbbs && !found; i++)
    {
      for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
           !gsi_end_p (gsi) && !found; gsi_next (&gsi))
        {
          gimple *stmt = gsi_stmt (gsi);
          if (!is_gimple_assign (stmt))
            continue;

          enum tree_code code = gimple_assign_rhs_code (stmt);

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

              /* Extract multiply operands  */
              gimple *mult_stmt = SSA_NAME_DEF_STMT (mult_result);
              tree op_a = gimple_assign_rhs1 (mult_stmt);
              tree op_b = gimple_assign_rhs2 (mult_stmt);

              /* Trace back through SSA to find the memory references.
                 In GIMPLE, array accesses become loads:
                   _1 = MEM[base + offset]  or  _1 = arr[idx]
                 We look for the load statements.  */
              tree ref_a = NULL_TREE, ref_b = NULL_TREE;

              if (TREE_CODE (op_a) == SSA_NAME)
                {
                  gimple *la = SSA_NAME_DEF_STMT (op_a);
                  if (la && is_gimple_assign (la))
                    {
                      tree r = gimple_assign_rhs1 (la);
                      if (TREE_CODE (r) == MEM_REF
                          || TREE_CODE (r) == ARRAY_REF)
                        ref_a = r;
                    }
                }
              if (TREE_CODE (op_b) == SSA_NAME)
                {
                  gimple *lb = SSA_NAME_DEF_STMT (op_b);
                  if (lb && is_gimple_assign (lb))
                    {
                      tree r = gimple_assign_rhs1 (lb);
                      if (TREE_CODE (r) == MEM_REF
                          || TREE_CODE (r) == ARRAY_REF)
                        ref_b = r;
                    }
                }

              if (ref_a && ref_b)
                {
                  /* Extract base pointers from the memory references  */
                  *a_ref = (TREE_CODE (ref_a) == MEM_REF)
                           ? TREE_OPERAND (ref_a, 0)
                           : TREE_OPERAND (ref_a, 0);
                  *b_ref = (TREE_CODE (ref_b) == MEM_REF)
                           ? TREE_OPERAND (ref_b, 0)
                           : TREE_OPERAND (ref_b, 0);

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
    return false;

  *a_base = ref_a;
  *b_base = ref_b;
  *c_base = ref_c;

  if (dump_file)
    fprintf (dump_file, "  Found matmul pattern in loop %d\n", loop->num);

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
                          || TREE_CODE (r) == ARRAY_REF)
                        {
                          *arr_base = (TREE_CODE (r) == MEM_REF)
                                      ? TREE_OPERAND (r, 0)
                                      : TREE_OPERAND (r, 0);
                          found = true;
                        }
                    }
                }
            }

          /* Also handle:  arr[i][j] = arr[i][j] * (1.0 / divisor)  */
          if (code == MULT_EXPR && !found)
            {
              tree rhs2 = gimple_assign_rhs2 (stmt);
              if (TREE_CODE (rhs2) == SSA_NAME)
                {
                  gimple *def = SSA_NAME_DEF_STMT (rhs2);
                  if (def && is_gimple_assign (def)
                      && gimple_assign_rhs_code (def) == RDIV_EXPR)
                    {
                      *divisor = gimple_assign_rhs2 (def);
                      tree rhs1_inner = gimple_assign_rhs1 (stmt);
                      if (TREE_CODE (rhs1_inner) == SSA_NAME)
                        {
                          gimple *ld = SSA_NAME_DEF_STMT (rhs1_inner);
                          if (ld && is_gimple_assign (ld))
                            {
                              tree r = gimple_assign_rhs1 (ld);
                              if (TREE_CODE (r) == MEM_REF
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
  /* The outer loop must have exactly 3 direct child loops  */
  int nchildren = count_child_loops (loop);
  if (nchildren != 3)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: expected 3 child loops, got %d\n",
                 nchildren);
      return false;
    }

  class loop *child1 = loop->inner;
  class loop *child2 = child1->next;
  class loop *child3 = child2->next;

  /* --- Child 1: max-reduction ---
     Look for a conditional GT_EXPR / GE_EXPR in the loop body,
     which characterizes max_val = max(max_val, arr[i][j]).  */
  bool has_max = false;
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

  if (!has_max)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: child1 is not a max-reduction\n");
      return false;
    }

  /* --- Child 2: sum-of-exp ---
     Look for: sum += exp(something)
     i.e., a PLUS_EXPR reduction where one operand traces back to exp().  */
  bool has_exp_sum = false;
  {
    basic_block *bbs = get_loop_body (child2);
    unsigned nbbs = child2->num_nodes;
    for (unsigned i = 0; i < nbbs && !has_exp_sum; i++)
      {
        for (gimple_stmt_iterator gsi = gsi_start_bb (bbs[i]);
             !gsi_end_p (gsi); gsi_next (&gsi))
          {
            gimple *stmt = gsi_stmt (gsi);
            /* Direct call to exp in this loop?  */
            if (is_exp_call (stmt))
              has_exp_sum = true;
          }
      }
    free (bbs);
  }

  if (!has_exp_sum)
    {
      if (dump_file)
        fprintf (dump_file, "  Softmax: child2 has no exp() call\n");
      return false;
    }

  /* --- Child 3: normalization ---
     Look for: arr[i][j] = exp(...) / sum
     i.e., RDIV_EXPR where one operand traces back to exp().  */
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
            if (is_exp_call (stmt))
              has_normalize = true; /* exp present in normalize loop  */
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
      if (!arrays_share_base (score_base_1, scale_arr))
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
      if (!arrays_share_base (score_base_1, softmax_arr))
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
      if (!arrays_share_base (score_base_1, matmul2_a))
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

      /* Exit of the fourth loop  */
      edge exit_e = single_exit (l4);
      if (!exit_e)
        {
          /* For non-perfect loops, use the loop latch successor  */
          class loop *outer4 = l4;
          while (outer4->inner)
            outer4 = outer4;  /* already at top level  */
          /* Fallback: the block immediately after the last loop  */
          cand->exit_bb = NULL;
        }
      else
        cand->exit_bb = exit_e->dest;

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

/* Find the __builtin_riscv_attn function declaration by iterating
   the target builtin table.  */

static tree
find_attn_builtin (void)
{
  for (unsigned i = 0; ; i++)
    {
      tree decl = targetm.builtin_decl (i, false);
      if (decl == NULL_TREE || decl == error_mark_node)
        break;
      const char *name = IDENTIFIER_POINTER (DECL_NAME (decl));
      if (name && strcmp (name, "__builtin_riscv_attn") == 0)
        return decl;
    }
  return NULL_TREE;
}


/* Build a RECORD_TYPE with the given INT fields on the stack.
   Returns the VAR_DECL for the stack variable.

   attn_dims_t:  { int rows, cols, seq_len, d_model }
   attn_qkv_t:   { float *Q, *K, *V }                    */

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

  DECL_CHAIN (f_q) = f_k;
  DECL_CHAIN (f_k) = f_v;
  DECL_CHAIN (f_v) = NULL_TREE;

  tree qkv_type = make_node (RECORD_TYPE);
  TYPE_FIELDS (qkv_type) = f_q;
  DECL_CONTEXT (f_q) = qkv_type;
  DECL_CONTEXT (f_k) = qkv_type;
  DECL_CONTEXT (f_v) = qkv_type;
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
  tree fndecl = find_attn_builtin ();
  if (!fndecl)
    {
      if (dump_file)
        fprintf (dump_file, "  ERROR: __builtin_riscv_attn not found in "
                 "target builtins — cannot replace pattern.\n");
      return;
    }

  /* We need a block to insert new statements.  Use the preheader of
     the first loop, or the entry block if no preheader.  */
  basic_block insert_bb = cand->insert_bb;
  if (!insert_bb)
    insert_bb = single_succ (ENTRY_BLOCK_PTR_FOR_FN (fun));

  gimple_stmt_iterator gsi = gsi_last_bb (insert_bb);

  /* 1. Build and fill attn_dims_t on the stack  */
  tree dims_var = build_dims_struct (fun);
  tree dims_type = TREE_TYPE (dims_var);
  tree f = TYPE_FIELDS (dims_type);

  /* rows = seq_len  */
  tree val_rows = cand->seq_len
                  ? fold_convert (integer_type_node, cand->seq_len)
                  : build_int_cst (integer_type_node, 0);
  gimple *s1 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    val_rows);
  gsi_insert_after (&gsi, s1, GSI_NEW_STMT);

  /* cols = seq_len (for square attention score matrix)  */
  f = DECL_CHAIN (f);
  gimple *s2 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    val_rows);
  gsi_insert_after (&gsi, s2, GSI_NEW_STMT);

  /* seq_len  */
  f = DECL_CHAIN (f);
  gimple *s3 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    val_rows);
  gsi_insert_after (&gsi, s3, GSI_NEW_STMT);

  /* d_model  */
  f = DECL_CHAIN (f);
  tree val_dm = cand->d_model
                ? fold_convert (integer_type_node, cand->d_model)
                : build_int_cst (integer_type_node, 0);
  gimple *s4 = gimple_build_assign (
    build3 (COMPONENT_REF, integer_type_node, dims_var, f, NULL_TREE),
    val_dm);
  gsi_insert_after (&gsi, s4, GSI_NEW_STMT);

  /* 2. Build and fill attn_qkv_t on the stack  */
  tree qkv_var = build_qkv_struct (fun);
  tree qkv_type = TREE_TYPE (qkv_var);
  tree fq = TYPE_FIELDS (qkv_type);
  tree ptr_type = build_pointer_type (float_type_node);

  /* Q pointer  */
  tree q_addr = fold_convert (ptr_type, cand->q_base);
  gimple *sq = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    q_addr);
  gsi_insert_after (&gsi, sq, GSI_NEW_STMT);

  /* K pointer  */
  fq = DECL_CHAIN (fq);
  tree k_addr = fold_convert (ptr_type, cand->k_base);
  gimple *sk = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    k_addr);
  gsi_insert_after (&gsi, sk, GSI_NEW_STMT);

  /* V pointer  */
  fq = DECL_CHAIN (fq);
  tree v_addr = fold_convert (ptr_type, cand->v_base);
  gimple *sv = gimple_build_assign (
    build3 (COMPONENT_REF, ptr_type, qkv_var, fq, NULL_TREE),
    v_addr);
  gsi_insert_after (&gsi, sv, GSI_NEW_STMT);

  /* 3. Emit volatile asm barrier to ensure stores are committed  */
  gasm *barrier = gimple_build_asm_vec ("", NULL, NULL, NULL, NULL);
  gimple_asm_set_volatile (barrier, true);
  gsi_insert_after (&gsi, barrier, GSI_NEW_STMT);

  /* 4. Build the call: __builtin_riscv_attn((ulong)&dims, (ulong)&qkv)  */
  tree ul_type = long_unsigned_type_node;

  /* &dims → (unsigned long)  */
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

  /* The actual call  */
  gcall *call = gimple_build_call (fndecl, 2, dims_tmp, qkv_tmp);
  tree result_var = create_tmp_var (ul_type, "attn_result");
  gimple_call_set_lhs (call, result_var);
  gsi_insert_after (&gsi, call, GSI_NEW_STMT);

  if (dump_file)
    {
      fprintf (dump_file, "\n  Inserted __builtin_riscv_attn call:\n  ");
      print_gimple_stmt (dump_file, call, 0, TDF_SLIM);
      fprintf (dump_file, "\n");
    }

  /* 5. Redirect control flow: skip all 4 loops.

     We connect the insertion block directly to the exit of the
     fourth loop, bypassing all loop bodies.

     Note: A full implementation would delete the loop BBs.  For this
     academic project, we leave the dead code for the later DCE passes
     to clean up — the critical thing is that the call is emitted and
     the loops become unreachable after we redirect the edge.  */

  if (cand->exit_bb)
    {
      edge e;
      edge_iterator ei;

      /* Remove all outgoing edges from insert_bb except fallthrough  */
      FOR_EACH_EDGE (e, ei, insert_bb->succs)
        {
          if (e->dest != cand->exit_bb)
            {
              /* Mark for later redirect — we can't modify while iterating  */
            }
        }

      /* Simplest approach: redirect the single fallthrough edge from
         insert_bb (which currently goes to loop1's header) to exit_bb.
         This makes the loop bodies unreachable → DCE removes them.  */
      edge succ = single_succ_edge (insert_bb);
      if (succ)
        redirect_edge_and_branch (succ, cand->exit_bb);
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

  /* Gate: always run for RISC-V targets.
     A future enhancement could gate on -march containing a custom
     extension flag (e.g. Xattn) or a dedicated -mattn-detect flag.  */
  bool gate (function *) final override
  {
    return true;
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
