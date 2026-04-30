# ─── RISC-V Custom attn Instruction — Demo Makefile ──────────────────────────
#
# Targets:
#   make            — build demo binary
#   make asm        — generate annotated assembly listing (.s)
#   make objdump    — disassemble and grep for the attn instruction
#   make run        — run under QEMU userspace emulation
#   make run-spike  — run under Spike + proxy kernel
#   make clean      — remove build artifacts
#
# Prerequisites:
#   - Patched riscv64-unknown-elf-gcc (from this repo)
#   - qemu-riscv64  (for `make run`)
#   - spike + pk    (for `make run-spike`)
# ─────────────────────────────────────────────────────────────────────────────

CC      = riscv64-unknown-elf-gcc
OBJDUMP = riscv64-unknown-elf-objdump

CFLAGS  = -O2 -march=rv64gc -mabi=lp64d
LDFLAGS = -lm

SRC     = demo_attn.c
BIN     = demo_attn
ASM     = demo_attn.s

.PHONY: all asm objdump run run-spike clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo ""
	@echo "Built: $(BIN)"
	@echo "Run:   make run       (QEMU)"
	@echo "       make run-spike (Spike)"
	@echo "Verify instruction emitted:"
	@$(OBJDUMP) -d $(BIN) | grep -E "attn|$(BIN):" | head -20

asm: $(SRC)
	$(CC) $(CFLAGS) -S $< -o $(ASM)
	@echo "Assembly written to $(ASM)"
	@echo ""
	@echo "Custom instruction in listing:"
	@grep -n "attn\|# Stage" $(ASM) || echo "(attn not found — check toolchain patch)"

objdump: $(BIN)
	@echo "─── Disassembly (attention function) ───────────────────────"
	$(OBJDUMP) -d $(BIN) | awk '/^[0-9a-f]+ <attention>:/,/^$/'
	@echo ""
	@echo "─── Custom instruction occurrences ─────────────────────────"
	$(OBJDUMP) -d $(BIN) | grep -n "attn" || echo "(none found)"

run: $(BIN)
	qemu-riscv64 ./$(BIN)

run-spike: $(BIN)
	spike --isa=rv64gc $$(which pk) ./$(BIN)

clean:
	rm -f $(BIN) $(ASM) *.o
