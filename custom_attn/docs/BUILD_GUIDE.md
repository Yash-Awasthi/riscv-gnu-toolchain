# Build Guide — RISC-V custom `attn` instruction (WSL)

**Author:** Yash Awasthi, Group 9
**Toolchain:** riscv-gnu-toolchain (GCC 15.2.0, Binutils 2.44)
**Host:** Ubuntu 22.04 / WSL2

---

## 0. Prerequisites

```bash
sudo apt update
sudo apt install -y autoconf automake autotools-dev curl python3 python3-pip \
    libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex \
    texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev ninja-build \
    git cmake libglib2.0-dev libslirp-dev
```

Disk space: at least **12 GB** free.  Build time: ~45–90 min depending on CPU.

---

## 1. Clone and initialise submodules

```bash
git clone https://github.com/<your-fork>/riscv-gnu-toolchain.git \
    ~/riscv-gnu-toolchain
cd ~/riscv-gnu-toolchain

# Only initialise the submodules we need to keep things fast
git submodule update --init gcc binutils-gdb
```

---

## 2. Apply manual patches to Binutils

### 2a. `binutils/include/opcode/riscv-opc.h`

Find the block that looks like:

```c
#ifdef DECLARE_INSN
...existing entries...
#endif /* DECLARE_INSN */
```

Add the following line **inside** the `#ifdef DECLARE_INSN` block, immediately
before the `#endif /* DECLARE_INSN */` line:

```c
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

Also add the macro definitions (near the top of the file, grouped with other
custom entries):

```c
#define MATCH_ATTN 0x0200000b
#define MASK_ATTN  0xfe00707f
```

> **Issue 1 warning:** If you used the automate script, it likely placed
> `DECLARE_INSN(attn, ...)` *after* the `#endif` line.  Verify placement
> manually.

### 2b. `binutils/opcodes/riscv-opc.c`

Find the `riscv_opcodes[]` array.  Locate the entry for `auipc` and insert
the `attn` entry directly before it:

```c
{"attn",   0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
```

> **Issue 2 warning:** The automate script never added this entry.  Without
> it the assembler silently ignores the `attn` mnemonic.

---

## 3. Apply manual patches to GCC

The gcc submodule lives at `gcc/gcc/config/riscv/`.

### 3a. `gcc/gcc/config/riscv/riscv-ftypes.def`

Append (or find the existing custom block and add):

```c
DEF_RISCV_FTYPE (2, (UDI, UDI, UDI))
```

> **Issue 3 note:** Use `UDI` (unsigned DI), **not** `DI`.  `RISCV_ATYPE_DI`
> does not exist; using `DI` causes a compile error.

### 3b. `gcc/gcc/config/riscv/riscv-builtins.cc`

Add near the bottom of the AVAIL / DIRECT_BUILTIN section:

```c
AVAIL (always_enabled, (!0))
DIRECT_BUILTIN (attn, RISCV_UDI_FTYPE_UDI_UDI, always_enabled)
```

### 3c. `gcc/gcc/config/riscv/riscv.md`

Append at the end of the file (or in the unspec enum block + instruction
pattern section):

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

### 3d. `gcc/gcc/config/riscv/riscv-attn-detect.cc` (new file)

Copy from `custom_attn/src/riscv-attn-detect.cc` (maintained in this repo).

See `docs/MANUAL_PATCHES.md` for the full file content.

### 3e. `gcc/gcc/config/riscv/t-riscv`

Add a build rule for the new translation unit.  The rule **must** use
TAB-indented recipe lines:

```makefile
riscv-attn-detect.o: $(srcdir)/config/riscv/riscv-attn-detect.cc
	$(COMPILE) $<
	$(POSTCOMPILE)
```

> **Issue 8 warning:** Do NOT write `$(COMPILE) $< $(OUTPUT_OPTION)` — the
> `$(COMPILE)` macro already includes `-o $@`, so adding `$(OUTPUT_OPTION)`
> produces a duplicate `-o` flag and a build error.

### 3f. `gcc/gcc/config.gcc`

Find the RISC-V target stanza (search for `riscv*-*-*`) and append
`riscv-attn-detect.o` to the `extra_objs` variable:

```bash
# Before:
extra_objs="riscv-builtins.o riscv-c.o riscv-sr.o ..."
# After:
extra_objs="riscv-builtins.o riscv-c.o riscv-sr.o ... riscv-attn-detect.o"
```

> **Issue 9 note:** Without this, `riscv-attn-detect.o` is compiled but never
> linked into `cc1`.

### 3g. `gcc/gcc/passes.def`

Register the pass inside the **GIMPLE loop group**, after
`pass_loop_distribution`:

```c
NEXT_PASS (pass_riscv_attn_detect);
```

Search for `pass_loop_distribution` and insert immediately after that line.

> **Issue 7 warning:** Do NOT place this pass outside the loop group (e.g.,
> after `pass_complete_unrolli`).  Outside the loop group the loop tree has
> already been finalised, and `scev_initialize()` will ICE even with the
> conditional guard.

---

## 4. Configure and build

```bash
cd ~/riscv-gnu-toolchain
mkdir -p build
cd build

../configure \
    --prefix=/opt/riscv \
    --with-arch=rv64gc \
    --with-abi=lp64d \
    --with-multilib-generator="rv64gc-lp64d--"

# Full parallel build — pipe to tee so you keep a log
make -j$(nproc) 2>&1 | tee build.log
```

Build success indicator:

```
[100%] Built target riscv64-unknown-elf-gcc
```

Add to PATH:

```bash
echo 'export PATH=/opt/riscv/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

---

## 5. Smoke tests

### 5a. Built-in test

```c
// test_builtin.c
#include <stdint.h>
uint64_t test(uint64_t a, uint64_t b) {
    return __builtin_riscv_attn(a, b);
}
```

```bash
riscv64-unknown-elf-gcc -O2 -o test_builtin.o -c test_builtin.c
riscv64-unknown-elf-objdump -d test_builtin.o | grep attn
# Expected output line containing:  0200000b   attn   a0,a0,a1
```

### 5b. GIMPLE detection test

```c
// attention.c  — plain C 4-stage attention loop
#include <math.h>
void attention(float *Q, float *K, float *V, float *out,
               int seq_len, int d_k, int d_v) {
    float scores[seq_len][seq_len];
    // Stage 1: QK^T
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++) {
            float s = 0;
            for (int k = 0; k < d_k; k++) s += Q[i*d_k+k]*K[j*d_k+k];
            scores[i][j] = s / sqrtf(d_k);
        }
    // Stage 2: softmax
    for (int i = 0; i < seq_len; i++) {
        float mx = scores[i][0];
        for (int j = 1; j < seq_len; j++) if (scores[i][j] > mx) mx = scores[i][j];
        float sm = 0;
        for (int j = 0; j < seq_len; j++) { scores[i][j] = expf(scores[i][j]-mx); sm += scores[i][j]; }
        for (int j = 0; j < seq_len; j++) scores[i][j] /= sm;
    }
    // Stage 3: scores × V
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < d_v; j++) {
            float s = 0;
            for (int k = 0; k < seq_len; k++) s += scores[i][k]*V[k*d_v+j];
            out[i*d_v+j] = s;
        }
}
```

```bash
riscv64-unknown-elf-gcc -O2 -fdump-tree-riscv_attn_detect attention.c -o /dev/null
grep -i "attention pattern" attention.c.*riscv_attn_detect*
# Expected: "Attention pattern detected in 'attention'"
```

---

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `attn` not recognised by assembler | Missing entry in `riscv_opcodes[]` | Issue 2 — add to `riscv-opc.c` |
| `DECLARE_INSN` compile error | Wrong placement in `riscv-opc.h` | Issue 1 — move inside `#ifdef` block |
| `RISCV_ATYPE_DI` undeclared | Wrong type in `riscv-ftypes.def` | Issue 3 — change `DI` to `UDI` |
| ICE in `scev_initialize` | Unconditional loop init | Issue 4 — add `local_loop_init` guard |
| ICE in `repair_loop_structures` | `TODO_cleanup_cfg` in finish flags | Issue 5 — remove `TODO_cleanup_cfg` |
| ICE in `bitmap_set_bit` | CFG corruption after BB removal | Issue 6 — **resolved**; see `KNOWN_ISSUES.md` |
| Pass ICE outside loop context | Wrong placement in `passes.def` | Issue 7 — place after `pass_loop_distribution` |
| Duplicate `-o` flag link error | Wrong `t-riscv` recipe | Issue 8 — use `$(COMPILE) $<` only |
| Pass linked but not called | Missing `extra_objs` entry | Issue 9 — add to `config.gcc` |
| ICE in `calc_dfs_tree` | Stale dominance info after edge redirect | Issue 10 — fixed; see `KNOWN_ISSUES.md` |
| DCE crash `bb==NULL` | Broken virtual operand chain after BB removal | Issue 11 — fixed; see `KNOWN_ISSUES.md` |

---

## 7. Rebuilding after changes

```bash
cd ~/riscv-gnu-toolchain/build
# Rebuild just GCC (faster):
make -j$(nproc) all-gcc 2>&1 | tee rebuild.log

# Or full rebuild:
make -j$(nproc) 2>&1 | tee rebuild.log
```

To force-rebuild `riscv-attn-detect.o` only:

```bash
cd ~/riscv-gnu-toolchain/build/build-gcc-newlib-stage2/gcc
make riscv-attn-detect.o
```
