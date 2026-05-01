# Known Issues — RISC-V custom `attn` instruction

**Author:** Yash Awasthi, Group 9
This document records all 9 issues encountered during the integration of the
custom `attn` RISC-V instruction into the riscv-gnu-toolchain fork, along with
root causes and exact fixes.

---

## Issue 1 — `DECLARE_INSN` placement in `riscv-opc.h`

**File:** `binutils/include/opcode/riscv-opc.h`

### Symptom

Build error or macro silently ignored:

```
error: expected declaration before '}' token
```

Or the `attn` mnemonic is not recognised by the assembler even though
`MATCH_ATTN` / `MASK_ATTN` are defined.

### Root cause

The automate script appended:

```c
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

*after* the closing `#endif /* DECLARE_INSN */` line.  The `DECLARE_INSN`
macro is only defined inside the `#ifdef DECLARE_INSN` ... `#endif` guard, so
any invocation outside that block either triggers a compile error (if
`DECLARE_INSN` is undefined at that point) or is silently ignored.

### Fix

Move the line to be the last entry **inside** the `#ifdef DECLARE_INSN` block:

```c
#ifdef DECLARE_INSN
...existing entries...
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)   // ← add here
#endif /* DECLARE_INSN */
```

---

## Issue 2 — Missing opcode entry in `riscv-opc.c`

**File:** `binutils/opcodes/riscv-opc.c`

### Symptom

```
$ riscv64-unknown-elf-as test.s
test.s:1: Error: unrecognized opcode `attn'
```

Or `objdump` shows the raw hex `0200000b` but labels it `.insn` rather than
`attn`.

### Root cause

The automate script added the macro definitions and `DECLARE_INSN` entry but
never added a row to the `riscv_opcodes[]` table in `riscv-opc.c`.  Without
this table entry, `gas` and `objdump` have no knowledge of the mnemonic.

### Fix

In `binutils/opcodes/riscv-opc.c`, locate the `riscv_opcodes[]` array and
insert before the `auipc` entry:

```c
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
```

Fields:
- `"attn"` — mnemonic string
- `0` — ISA version (0 = any)
- `INSN_CLASS_I` — base integer class
- `"d,s,t"` — operand format: dest, src1, src2
- `MATCH_ATTN` / `MASK_ATTN` — encoding constants
- `match_opcode` — standard opcode matching function
- `0` — no special flags

---

## Issue 3 — GCC files not updated by automate script

**Files:** `gcc/gcc/config/riscv/riscv-ftypes.def`,
`gcc/gcc/config/riscv/riscv-builtins.cc`,
`gcc/gcc/config/riscv/riscv.md`

### Symptom

`__builtin_riscv_attn` is undeclared:

```
error: implicit declaration of function '__builtin_riscv_attn'
```

Or `RISCV_ATYPE_DI` undeclared:

```
error: 'RISCV_ATYPE_DI' undeclared
```

### Root cause

The automate script silently failed to patch GCC files because either:
1. The `gcc` submodule was not initialised when the script ran, so the files
   didn't exist at the expected paths.
2. The anchor patterns the script searched for differed in GCC 15.2.0 vs the
   version the script was written against.

Additionally, the script attempted to use `DI` (signed double-integer) as the
argument type, but `RISCV_ATYPE_DI` is not defined — only `RISCV_ATYPE_UDI`
(unsigned DI) exists.

### Fix

**riscv-ftypes.def** — add:

```c
DEF_RISCV_FTYPE (2, (UDI, UDI, UDI))
```

(Two `UDI` inputs, one `UDI` output.)

**riscv-builtins.cc** — add (in the AVAIL + DIRECT_BUILTIN section):

```c
AVAIL (always_enabled, (!0))
DIRECT_BUILTIN (attn, RISCV_UDI_FTYPE_UDI_UDI, always_enabled)
```

**riscv.md** — append:

```scheme
(define_c_enum "unspec" [UNSPEC_ATTN])

(define_insn "riscv_attn"
  [(set (match_operand:DI 0 "register_operand" "=r")
        (unspec:DI [(match_operand:DI 1 "register_operand" "r")
                    (match_operand:DI 2 "register_operand" "r")]
                   UNSPEC_ATTN))]
  ""
  "attn\t%0,%1,%2"
  [(set_attr "type" "unknown")
   (set_attr "mode" "DI")])
```

---

## Issue 4 — Unconditional loop-optimizer init in `riscv-attn-detect.cc`

**File:** `gcc/gcc/config/riscv/riscv-attn-detect.cc`

### Symptom

Internal Compiler Error (ICE) during compilation of any C file:

```
during GIMPLE pass: riscv_attn_detect
internal compiler error: in scev_initialize, at tree-scalar-evolution.cc:...
```

### Root cause

The pass unconditionally called:

```cpp
loop_optimizer_init(LOOPS_NORMAL);
scev_initialize();
```

When the pass is placed after `pass_loop_distribution` (which is inside a
loop optimizer context), the loop optimizer is *already* initialised.  Calling
`loop_optimizer_init` a second time corrupts the loop tree, and the subsequent
`scev_initialize` triggers the ICE.

### Fix

Guard the initialisation with a check:

```cpp
bool local_loop_init = (loops_for_fn(fun) == NULL);
if (local_loop_init) {
    loop_optimizer_init(LOOPS_NORMAL);
    scev_initialize();
}

// ... detection logic ...

if (local_loop_init) {
    scev_finalize();
    loop_optimizer_finalize();
}
```

This makes the pass safe whether or not a loop context already exists.

---

## Issue 5 — `TODO_cleanup_cfg` in `todo_flags_finish`

**File:** `gcc/gcc/config/riscv/riscv-attn-detect.cc` (pass_data struct)

### Symptom

ICE after a successful detection+replacement:

```
internal compiler error: in rewrite_into_loop_closed_ssa, at ...
```

or

```
internal compiler error: in repair_loop_structures, at ...
```

### Root cause

The `pass_data` struct originally included:

```cpp
.todo_flags_finish = TODO_update_ssa | TODO_cleanup_cfg,
```

After the pass replaces attention loops with a built-in call, requesting
`TODO_cleanup_cfg` causes GCC's pass manager to call `cleanup_tree_cfg()`,
which in turn calls `repair_loop_structures()`, which calls
`rewrite_into_loop_closed_ssa()`.  At this point the loop tree has already
been finalised by the pass, so the SSA rewriter crashes.

### Fix

Remove `TODO_cleanup_cfg` from the finish flags:

```cpp
.todo_flags_finish = TODO_update_ssa,
```

---

## Issue 6 — CFG corruption in `replace_attention_with_builtin`

**File:** `gcc/gcc/config/riscv/riscv-attn-detect.cc` (Section E)

### Symptom

ICE when the pass attempts to replace a detected attention pattern:

```
internal compiler error: in compute_dominance_frontiers, at dominance.cc:...
```

or

```
internal compiler error: in bitmap_set_bit, at bitmap.cc:...
```

### Root cause

The replacement function removes the loop basic blocks (BBs) from the CFG
but leaves dangling SSA uses that reference values defined in the removed
blocks.  When `update_ssa()` is subsequently invoked, it tries to compute
dominance frontiers for the modified CFG but encounters inconsistencies
because:

1. Removed BBs still appear in some PHI node operand lists.
2. The loop tree no longer reflects the modified CFG.
3. Dominance information is stale.

The correct fix requires:
1. Properly redirecting all edges away from loop BBs before removal.
2. Removing or rewriting all PHI nodes that reference removed BB definitions.
3. Calling `free_dominance_info()` and letting `update_ssa` recompute.
4. Resetting loop info with `loops_state_clear` / `loops_state_set`.

### Current workaround

The replacement section is **disabled**.  Pattern detection still works and is
confirmed via:

```
-fdump-tree-riscv_attn_detect
```

The dump file will contain:

```
Attention pattern detected in 'attention'
```

Completing the CFG replacement is marked as future work.

---

## Issue 7 — Wrong placement of pass in `passes.def`

**File:** `gcc/gcc/passes.def`

### Symptom

ICE in `scev_initialize` even after applying the Issue 4 guard, when the pass
is placed outside the GIMPLE loop group.

### Root cause

`passes.def` organises GIMPLE passes into groups.  The GIMPLE loop group
(roughly between `pass_loop2` and `pass_complete_unrolli`) maintains a live
loop tree.  Passes placed *outside* this group run after the loop tree has been
destroyed.

The `riscv_attn_detect` pass uses SCEV analysis (`scev_initialize` /
`scev_const_prop`) which requires a live loop tree.  When placed outside the
loop group, even the conditional guard (`loops_for_fn(fun) == NULL`) will be
true (no loop tree), and the pass will try to rebuild it — but the CFG at that
point is already post-loop-opt and does not have the structure required for
loop discovery.

### Fix

Place the pass **inside** the GIMPLE loop group, immediately after
`pass_loop_distribution`:

```c
NEXT_PASS (pass_loop_distribution);
NEXT_PASS (pass_riscv_attn_detect);   // ← insert here
```

---

## Issue 8 — Broken build rule in `t-riscv`

**File:** `gcc/gcc/config/riscv/t-riscv`

### Symptom

```
make: *** [riscv-attn-detect.o] Error 1
```

with a message like:

```
gcc: error: cannot specify -o with -c, -S or -E with multiple files
```

or the file is never compiled at all (no rule found).

### Root cause

Two common mistakes:

1. **Using `$(COMPILE) $< $(OUTPUT_OPTION)`**: The `$(COMPILE)` macro is
   defined as `$(CC) ... -o $@`.  Adding `$(OUTPUT_OPTION)` (which expands to
   `-o $@`) produces a duplicate `-o $@` on the command line, which GCC rejects.

2. **Using `TM_H` as the prerequisite variable instead of the actual `.cc`
   path**: `TM_H` is a make variable containing header dependencies for the
   target machine file.  Listing it as the sole prerequisite means the `.cc`
   source file is never a dependency and the rule may never fire, or fires with
   the wrong source.

### Fix

Write the rule with an explicit source path and a two-line TAB-indented recipe:

```makefile
riscv-attn-detect.o: $(srcdir)/config/riscv/riscv-attn-detect.cc
	$(COMPILE) $<
	$(POSTCOMPILE)
```

- `$(COMPILE)` compiles the file and writes `$@` (output object).
- `$(POSTCOMPILE)` runs post-compilation steps (dependency generation, etc.).
- No `$(OUTPUT_OPTION)` needed — it's already in `$(COMPILE)`.

---

## Issue 9 — `riscv-attn-detect.o` not linked into `cc1`

**File:** `gcc/gcc/config.gcc`

### Symptom

The pass object is built successfully but `cc1` does not load it, so
`-fdump-tree-riscv_attn_detect` produces no output and the pass does not run.

```
cc1: error: unrecognized command-line option '-fdump-tree-riscv_attn_detect'
```

Or the pass simply never appears in `-fdump-tree-all` output.

### Root cause

GCC's build system uses the `extra_objs` variable (set per target in
`config.gcc`) to link target-specific object files into `cc1`.  Without an
entry for `riscv-attn-detect.o`, the object is compiled by the `t-riscv` rule
but never passed to the linker when assembling `cc1`.

### Fix

In `gcc/gcc/config.gcc`, find the RISC-V target stanza:

```bash
riscv*-*-*)
```

Locate the `extra_objs` assignment and append the new object:

```bash
extra_objs="${extra_objs} riscv-attn-detect.o"
```

Or if it is a plain assignment:

```bash
extra_objs="riscv-builtins.o riscv-c.o riscv-sr.o riscv-string.o \
            riscv-avlprop.o riscv-attn-detect.o"
```

---

## Summary table

| # | File(s) | Root cause | Status |
|---|---------|-----------|--------|
| 1 | `riscv-opc.h` | `DECLARE_INSN` outside `#ifdef` block | Fixed |
| 2 | `riscv-opc.c` | Missing row in `riscv_opcodes[]` | Fixed |
| 3 | `riscv-ftypes.def`, `riscv-builtins.cc`, `riscv.md` | Script silently skipped GCC files; wrong type `DI` vs `UDI` | Fixed |
| 4 | `riscv-attn-detect.cc` | Unconditional `scev_initialize` ICE | Fixed |
| 5 | `riscv-attn-detect.cc` | `TODO_cleanup_cfg` triggers post-loop SSA crash | Fixed |
| 6 | `riscv-attn-detect.cc` | Dangling SSA uses after BB removal | Disabled (future work) |
| 7 | `passes.def` | Pass placed outside GIMPLE loop group | Fixed |
| 8 | `t-riscv` | Duplicate `-o` flag / wrong recipe | Fixed |
| 9 | `config.gcc` | Missing `extra_objs` entry | Fixed |
