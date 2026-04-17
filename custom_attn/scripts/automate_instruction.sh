#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════
# automate_instruction.sh — Bash version of the RISC-V Custom Instruction Generator
# ══════════════════════════════════════════════════════════════════════
# Group 9 | RISC-V GNU Toolchain
#
# Usage:
#   ./automate_instruction.sh add <name> <inputs:0-3> [description]
#   ./automate_instruction.sh delete <name> [name2 ...]
#   ./automate_instruction.sh list
#   ./automate_instruction.sh scan <inputs:0-3>
#   ./automate_instruction.sh batch <file>
#   ./automate_instruction.sh                          # Interactive
#
# Supports 0/1/2/3 input instructions, batch add, delete, scan.
# Modifies all 6 toolchain source files.
# ══════════════════════════════════════════════════════════════════════
set -euo pipefail

# ── Paths ────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
BINUTILS_DIR="$REPO_ROOT/binutils"
GCC_DIR="$REPO_ROOT/gcc"

OPC_H="$BINUTILS_DIR/include/opcode/riscv-opc.h"
OPC_C="$BINUTILS_DIR/opcodes/riscv-opc.c"
FTYPES_DEF="$GCC_DIR/gcc/config/riscv/riscv-ftypes.def"
BUILTINS_CC="$GCC_DIR/gcc/config/riscv/riscv-builtins.cc"
RISCV_MD="$GCC_DIR/gcc/config/riscv/riscv.md"
RV_CUSTOM="$REPO_ROOT/riscv-opcodes/extensions/rv_custom"

INSTALL_PREFIX="$HOME/riscv_custom"
BUILD_DIR="$REPO_ROOT/build_custom"
DEMO_DIR="$(dirname "$SCRIPT_DIR")/demo"
SRC_DIR="$(dirname "$SCRIPT_DIR")/src"

MARKER="(auto-generated)"

# ── Custom opcode bases ──────────────────────────────────────────────
# base_name  base_opcode  bits_6_2
BASES=("custom-0:0x0B:0x02" "custom-1:0x2B:0x0A" "custom-2:0x5B:0x16" "custom-3:0x7B:0x1E")

# ── Helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()    { echo -e "  ${CYAN}[$1]${NC} $2"; }
ok()      { echo -e "  ${GREEN}[$1]${NC} $2"; }
warn()    { echo -e "  ${YELLOW}[$1]${NC} $2"; }
err()     { echo -e "  ${RED}[$1]${NC} $2"; }

banner() {
    echo "================================================================"
    echo "  RISC-V Custom Instruction Generator (Group 9) — Bash Edition"
    echo "  Supports: 0/1/2/3 inputs | batch | delete"
    echo "================================================================"
}

# ── Input config lookup ──────────────────────────────────────────────
get_operand_fmt() {
    case "$1" in
        0) echo "d" ;;
        1) echo "d,s" ;;
        2) echo "d,s,t" ;;
        3) echo "d,s,t,r" ;;
    esac
}

get_format_name() {
    case "$1" in
        0|1|2) echo "R-type" ;;
        3)     echo "R4-type" ;;
    esac
}

get_ftype_args() { echo "$1"; }

get_ftype_tuple() {
    case "$1" in
        0) echo "(DI)" ;;
        1) echo "(DI, DI)" ;;
        2) echo "(DI, DI, DI)" ;;
        3) echo "(DI, DI, DI, DI)" ;;
    esac
}

get_ftype_name() {
    case "$1" in
        0) echo "RISCV_DI_FTYPE" ;;
        1) echo "RISCV_DI_FTYPE_DI" ;;
        2) echo "RISCV_DI_FTYPE_DI_DI" ;;
        3) echo "RISCV_DI_FTYPE_DI_DI_DI" ;;
    esac
}

get_asm_operands() {
    case "$1" in
        0) echo "%0" ;;
        1) echo "%0,%1" ;;
        2) echo "%0,%1,%2" ;;
        3) echo "%0,%1,%2,%3" ;;
    esac
}

to_upper() { echo "$1" | tr '[:lower:]' '[:upper:]'; }
sanitize_name() { echo "$1" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9_]/_/g'; }

# ══════════════════════════════════════════════════════════════════════
#  STEP 1 — Find free opcode
# ══════════════════════════════════════════════════════════════════════

parse_existing_matches() {
    grep -oP '#define\s+MATCH_\w+\s+\K0x[0-9a-fA-F]+' "$OPC_H" 2>/dev/null || true
}

compute_match() {
    # compute_match <base_opc> <funct3> <funct7_or_funct2> <num_inputs>
    local base=$1 f3=$2 f7_f2=$3 ni=$4
    if [[ $ni -le 2 ]]; then
        printf "0x%08x" $(( base | (f3 << 12) | (f7_f2 << 25) ))
    else
        printf "0x%08x" $(( base | (f3 << 12) | (f7_f2 << 25) ))
    fi
}

compute_mask() {
    local ni=$1
    if [[ $ni -le 2 ]]; then
        echo "0xfe00707f"
    else
        echo "0x0600707f"
    fi
}

find_free_opcode() {
    # find_free_opcode <num_inputs>
    # Sets: FREE_BASE_NAME, FREE_BASE_OPC, FREE_BASE_BITS, FREE_F3, FREE_F7F2, FREE_MATCH, FREE_MASK
    local ni=$1
    local existing
    existing=$(parse_existing_matches)

    for entry in "${BASES[@]}"; do
        IFS=: read -r bname bopc bbits <<< "$entry"
        local bopc_dec=$(( bopc ))

        if [[ $ni -le 2 ]]; then
            local f7_max=127
        else
            local f7_max=3
        fi

        for (( f3=0; f3<8; f3++ )); do
            for (( f7=0; f7<=f7_max; f7++ )); do
                local match_val
                match_val=$(compute_match "$bopc_dec" "$f3" "$f7" "$ni")
                if ! echo "$existing" | grep -qi "^${match_val}$"; then
                    FREE_BASE_NAME="$bname"
                    FREE_BASE_OPC="$bopc"
                    FREE_BASE_BITS="$bbits"
                    FREE_F3="$f3"
                    FREE_F7F2="$f7"
                    FREE_MATCH="$match_val"
                    FREE_MASK=$(compute_mask "$ni")
                    return 0
                fi
            done
        done
    done
    return 1
}

# ══════════════════════════════════════════════════════════════════════
#  SCAN — Show free opcodes
# ══════════════════════════════════════════════════════════════════════

cmd_scan() {
    local ni=$1
    local flabel="funct7"
    [[ $ni -eq 3 ]] && flabel="funct2"

    printf "\n%-12s %-8s %-10s %-14s %-14s\n" "Slot" "funct3" "$flabel" "MATCH" "MASK"
    echo "------------------------------------------------------------"

    local existing count=0
    existing=$(parse_existing_matches)

    for entry in "${BASES[@]}"; do
        IFS=: read -r bname bopc bbits <<< "$entry"
        local bopc_dec=$(( bopc ))
        local f7_max=127
        [[ $ni -eq 3 ]] && f7_max=3

        for (( f3=0; f3<8; f3++ )); do
            for (( f7=0; f7<=f7_max; f7++ )); do
                local mv
                mv=$(compute_match "$bopc_dec" "$f3" "$f7" "$ni")
                if ! echo "$existing" | grep -qi "^${mv}$"; then
                    local mk=$(compute_mask "$ni")
                    printf "%-12s %-8s %-10s %-14s %-14s\n" "$bname" "$f3" "$f7" "$mv" "$mk"
                    (( count++ ))
                    [[ $count -ge 20 ]] && echo "... (showing first 20 of many)" && return 0
                fi
            done
        done
    done

    [[ $count -eq 0 ]] && echo "  No free slots found!"
    return 0
}

# ══════════════════════════════════════════════════════════════════════
#  LIST — Show existing custom instructions
# ══════════════════════════════════════════════════════════════════════

cmd_list() {
    echo ""
    if ! grep -q "$MARKER" "$OPC_H" 2>/dev/null; then
        echo "  No auto-generated custom instructions found."
        return 0
    fi

    printf "  %-20s %-8s %-10s %-14s %-14s\n" "Name" "Inputs" "Format" "MATCH" "MASK"
    echo "  ------------------------------------------------------------------"

    grep -A2 "$MARKER" "$OPC_H" | grep -oP '#define MATCH_(\w+)\s+(0x[0-9a-fA-F]+)' | while read -r _ _ name match; do
        : # handled below
    done

    # Parse more carefully
    local in_block=0 name="" match_v="" mask_v=""
    while IFS= read -r line; do
        if [[ "$line" == *"Custom Instruction"*"$MARKER"* ]]; then
            name=$(echo "$line" | sed -n 's/.*Custom Instruction.*— \(\w\+\).*/\1/p')
        elif [[ -n "$name" ]] && [[ "$line" == *"MATCH_"* ]] && [[ -z "$match_v" ]]; then
            match_v=$(echo "$line" | grep -oP '0x[0-9a-fA-F]+')
        elif [[ -n "$name" ]] && [[ "$line" == *"MASK_"* ]] && [[ -n "$match_v" ]]; then
            mask_v=$(echo "$line" | grep -oP '0x[0-9a-fA-F]+')

            # Determine format from opc.c
            local fmt ni
            fmt=$(grep "\"$name\"" "$OPC_C" 2>/dev/null | sed -n 's/.*"\([^"]*\)".*MATCH_.*/\1/p' | head -1)
            ni=$(echo "$fmt" | tr -cd ',' | wc -c)

            printf "  %-20s %-8s %-10s %-14s %-14s\n" "$name" "$ni" "$fmt" "$match_v" "$mask_v"
            name="" match_v="" mask_v=""
        fi
    done < "$OPC_H"
}

# ══════════════════════════════════════════════════════════════════════
#  ADD — Modify all 6 files
# ══════════════════════════════════════════════════════════════════════

modify_opc_h() {
    local name=$1 match_val=$2 mask_val=$3
    local upper=$(to_upper "$name")

    if grep -q "MATCH_${upper}" "$OPC_H" 2>/dev/null; then
        info "opc.h" "MATCH_${upper} already defined — skipping"
        return
    fi

    # Add MATCH/MASK after "Instruction opcode macros"
    local define_block
    define_block="\n/* Custom Instruction — ${name} ${MARKER} */\n#define MATCH_${upper}  ${match_val}\n#define MASK_${upper}   ${mask_val}\n"

    if grep -q "Instruction opcode macros" "$OPC_H"; then
        sed -i "/Instruction opcode macros/a\\${define_block}" "$OPC_H"
    else
        # Fallback: insert before first MATCH define
        sed -i "0,/^#define MATCH_/{s/^#define MATCH_/${define_block}\n#define MATCH_/}" "$OPC_H"
    fi

    # Add DECLARE_INSN at end of DECLARE_INSN block
    local last_line
    last_line=$(grep -n "DECLARE_INSN" "$OPC_H" | tail -1 | cut -d: -f1)
    if [[ -n "$last_line" ]]; then
        sed -i "${last_line}a\\DECLARE_INSN(${name}, MATCH_${upper}, MASK_${upper})" "$OPC_H"
    fi

    ok "opc.h" "Added MATCH_${upper}, MASK_${upper}, DECLARE_INSN"
}

modify_opc_c() {
    local name=$1 match_val=$2 mask_val=$3 ni=$4
    local upper=$(to_upper "$name")
    local fmt=$(get_operand_fmt "$ni")

    if grep -q "\"${name}\"" "$OPC_C" 2>/dev/null; then
        info "opc.c" "\"${name}\" already present — skipping"
        return
    fi

    local entry="\n/* Custom Instruction — ${name} ${MARKER} */\n{\"${name}\", 0, INSN_CLASS_I, \"${fmt}\", MATCH_${upper}, MASK_${upper}, match_opcode, 0},"

    # Insert after opening brace of riscv_opcodes[]
    sed -i "/const struct riscv_opcode riscv_opcodes\[\]/,/{/{s/{/{\n${entry}/;}" "$OPC_C" 2>/dev/null || \
    sed -i "/riscv_opcodes\[\]/,/{/s/{/{\n${entry}/" "$OPC_C"

    ok "opc.c" "Added \"${name}\" to riscv_opcodes[]"
}

modify_ftypes_def() {
    local ni=$1
    local fargs=$(get_ftype_args "$ni")
    local ftuple=$(get_ftype_tuple "$ni")
    local fname=$(get_ftype_name "$ni")
    local fline="DEF_RISCV_FTYPE (${fargs}, ${ftuple})"

    if grep -qF "$fline" "$FTYPES_DEF" 2>/dev/null; then
        info "ftypes.def" "${fname} already defined — skipping"
        return
    fi

    echo "" >> "$FTYPES_DEF"
    echo "/* Custom instruction ftype ${MARKER}: ${fname} */" >> "$FTYPES_DEF"
    echo "$fline" >> "$FTYPES_DEF"
    ok "ftypes.def" "Added ${fname}"
}

modify_builtins_cc() {
    local name=$1 ni=$2
    local fname=$(get_ftype_name "$ni")

    # 1. RISCV_ATYPE_DI
    if ! grep -q "RISCV_ATYPE_DI" "$BUILTINS_CC" 2>/dev/null; then
        sed -i '/#define RISCV_ATYPE_SI/a\#define RISCV_ATYPE_DI long_integer_type_node' "$BUILTINS_CC"
        ok "builtins.cc" "Added RISCV_ATYPE_DI"
    else
        info "builtins.cc" "RISCV_ATYPE_DI already present — skipping"
    fi

    # 2. AVAIL(always_enabled)
    if ! grep -q "always_enabled" "$BUILTINS_CC" 2>/dev/null; then
        sed -i '/AVAIL (hint_pause/a\AVAIL (always_enabled, (!0))' "$BUILTINS_CC"
        ok "builtins.cc" "Added AVAIL(always_enabled)"
    else
        info "builtins.cc" "AVAIL(always_enabled) already present — skipping"
    fi

    # 3. DIRECT_BUILTIN
    if grep -q "DIRECT_BUILTIN (${name}," "$BUILTINS_CC" 2>/dev/null; then
        info "builtins.cc" "DIRECT_BUILTIN(${name}) already present — skipping"
        return
    fi

    local insert="\n  /* Custom Instruction — ${name} ${MARKER} */\n  DIRECT_BUILTIN (${name}, ${fname}, always_enabled),"

    if grep -q '#include "corev.def"' "$BUILTINS_CC"; then
        sed -i "/#include \"corev.def\"/a\\${insert}" "$BUILTINS_CC"
    else
        local last_line
        last_line=$(grep -n "DIRECT_BUILTIN" "$BUILTINS_CC" | tail -1 | cut -d: -f1)
        sed -i "${last_line}a\\${insert}" "$BUILTINS_CC"
    fi
    ok "builtins.cc" "Added DIRECT_BUILTIN(${name})"
}

modify_riscv_md() {
    local name=$1 ni=$2
    local upper=$(to_upper "$name")
    local unspec="UNSPEC_${upper}"
    local asm_ops=$(get_asm_operands "$ni")

    # 1. Add to unspec enum
    if ! grep -q "$unspec" "$RISCV_MD" 2>/dev/null; then
        sed -i "/(define_c_enum \"unspec\" \[/a\\  ;; Custom Instruction — ${name} ${MARKER}\n  ${unspec}" "$RISCV_MD"
        ok "riscv.md" "Added ${unspec} to unspec enum"
    else
        info "riscv.md" "${unspec} already in unspec enum — skipping"
    fi

    # 2. Add define_insn
    if grep -q "riscv_${name}" "$RISCV_MD" 2>/dev/null; then
        info "riscv.md" "define_insn \"riscv_${name}\" already present — skipping"
        return
    fi

    local operands=""
    case "$ni" in
        0) operands="(unspec:DI [] ${unspec})" ;;
        1) operands="(unspec:DI [(match_operand:DI 1 \"register_operand\" \"r\")]\n                               ${unspec})" ;;
        2) operands="(unspec:DI [(match_operand:DI 1 \"register_operand\" \"r\")\n                                (match_operand:DI 2 \"register_operand\" \"r\")]\n                               ${unspec})" ;;
        3) operands="(unspec:DI [(match_operand:DI 1 \"register_operand\" \"r\")\n                                (match_operand:DI 2 \"register_operand\" \"r\")\n                                (match_operand:DI 3 \"register_operand\" \"r\")]\n                               ${unspec})" ;;
    esac

    cat >> "$RISCV_MD" << ENDOFMD

;; Custom Instruction — ${name} ${MARKER}
(define_insn "riscv_${name}"
  [(set (match_operand:DI 0 "register_operand" "=r")
        $(echo -e "$operands"))]
  ""
  "${name}\t${asm_ops}"
  [(set_attr "type" "arith")
   (set_attr "mode" "DI")])
ENDOFMD

    ok "riscv.md" "Added define_insn \"riscv_${name}\""
}

modify_rv_custom() {
    local name=$1 ni=$2 base_bits=$3 f3=$4 f7f2=$5

    if [[ ! -f "$RV_CUSTOM" ]]; then
        mkdir -p "$(dirname "$RV_CUSTOM")"
        cat > "$RV_CUSTOM" << 'EOF'
# rv_custom — Custom RISC-V instructions (auto-managed)
# Format: name  rd [rs1] [rs2] [rs3]  31..25=funct7  14..12=funct3  6..2=base  1..0=3

EOF
    fi

    local line=""
    case "$ni" in
        0) line="${name} rd 19..15=0 24..20=0 31..25=${f7f2} 14..12=${f3} 6..2=${base_bits} 1..0=3" ;;
        1) line="${name} rd rs1 24..20=0 31..25=${f7f2} 14..12=${f3} 6..2=${base_bits} 1..0=3" ;;
        2) line="${name} rd rs1 rs2 31..25=${f7f2} 14..12=${f3} 6..2=${base_bits} 1..0=3" ;;
        3) line="${name} rd rs1 rs2 rs3 26..25=${f7f2} 14..12=${f3} 6..2=${base_bits} 1..0=3" ;;
    esac

    if grep -q "^${name} " "$RV_CUSTOM" 2>/dev/null; then
        local existing
        existing=$(grep "^${name} " "$RV_CUSTOM")
        if [[ "$existing" == "$line" ]]; then
            info "rv_custom" "${name} already present with same encoding — skipping"
            return
        fi
        # Encoding differs — update the line
        sed -i "s|^${name} .*|${line}|" "$RV_CUSTOM"
        ok "rv_custom" "Updated ${name} encoding in rv_custom"
        return
    fi

    echo "$line" >> "$RV_CUSTOM"
    ok "rv_custom" "Added ${name} to riscv-opcodes/extensions/rv_custom"
}

# ══════════════════════════════════════════════════════════════════════
#  DELETE — Remove from all 6 files
# ══════════════════════════════════════════════════════════════════════

delete_from_opc_h() {
    local name=$1 upper=$(to_upper "$1")
    local before=$(wc -c < "$OPC_H")

    # Remove comment + MATCH + MASK block
    sed -i "/Custom Instruction.*${name}.*${MARKER}/,/MASK_${upper}/d" "$OPC_H"
    # Remove DECLARE_INSN
    sed -i "/DECLARE_INSN(${name},.*MATCH_${upper}.*MASK_${upper})/d" "$OPC_H"

    local after=$(wc -c < "$OPC_H")
    if [[ $after -lt $before ]]; then
        ok "opc.h" "Removed MATCH_${upper}, MASK_${upper}, DECLARE_INSN"
    else
        warn "opc.h" "${name} not found — nothing to remove"
    fi
}

delete_from_opc_c() {
    local name=$1
    local before=$(wc -c < "$OPC_C")

    sed -i "/Custom Instruction.*${name}.*${MARKER}/{N;d;}" "$OPC_C"

    local after=$(wc -c < "$OPC_C")
    if [[ $after -lt $before ]]; then
        ok "opc.c" "Removed \"${name}\" from riscv_opcodes[]"
    else
        warn "opc.c" "${name} not found — nothing to remove"
    fi
}

delete_from_builtins_cc() {
    local name=$1
    local before=$(wc -c < "$BUILTINS_CC")

    # Remove comment + DIRECT_BUILTIN block
    sed -i "/Custom Instruction.*${name}.*${MARKER}/{N;d;}" "$BUILTINS_CC"

    local after=$(wc -c < "$BUILTINS_CC")
    if [[ $after -lt $before ]]; then
        ok "builtins.cc" "Removed DIRECT_BUILTIN(${name})"
    else
        warn "builtins.cc" "${name} not found — nothing to remove"
    fi
}

delete_from_riscv_md() {
    local name=$1 upper=$(to_upper "$1")
    local before=$(wc -c < "$RISCV_MD")

    # Remove UNSPEC entry
    sed -i "/Custom Instruction.*${name}.*${MARKER}/{N;d;}" "$RISCV_MD"
    # Remove define_insn block (multi-line)
    sed -i "/Custom Instruction.*${name}.*${MARKER}/,/(set_attr \"mode\" \"DI\")]/d" "$RISCV_MD"

    # Also remove standalone UNSPEC_ line if leftover
    sed -i "/^  UNSPEC_${upper}$/d" "$RISCV_MD"

    local after=$(wc -c < "$RISCV_MD")
    if [[ $after -lt $before ]]; then
        ok "riscv.md" "Removed UNSPEC_${upper} and define_insn"
    else
        warn "riscv.md" "${name} not found — nothing to remove"
    fi
}

delete_from_ftypes_def() {
    local ni=$1
    local fname=$(get_ftype_name "$ni")

    # Check if other instructions still use this ftype
    local count
    count=$(grep -c "DIRECT_BUILTIN.*${fname}" "$BUILTINS_CC" 2>/dev/null || echo "0")
    if [[ $count -gt 0 ]]; then
        info "ftypes.def" "${fname} still used by other instructions — keeping"
        return
    fi

    local before=$(wc -c < "$FTYPES_DEF")
    sed -i "/Custom instruction ftype.*${MARKER}.*${fname}/{N;d;}" "$FTYPES_DEF"
    local after=$(wc -c < "$FTYPES_DEF")

    if [[ $after -lt $before ]]; then
        ok "ftypes.def" "Removed ${fname}"
    else
        warn "ftypes.def" "${fname} not found — nothing to remove"
    fi
}

delete_from_rv_custom() {
    local name=$1

    if [[ ! -f "$RV_CUSTOM" ]]; then
        warn "rv_custom" "File not found — nothing to remove"
        return
    fi

    local before=$(wc -c < "$RV_CUSTOM")
    sed -i "/^${name} /d" "$RV_CUSTOM"
    local after=$(wc -c < "$RV_CUSTOM")

    if [[ $after -lt $before ]]; then
        ok "rv_custom" "Removed ${name}"
    else
        warn "rv_custom" "${name} not found — nothing to remove"
    fi
}

cmd_delete() {
    for name in "$@"; do
        name=$(sanitize_name "$name")
        echo ""
        echo "  Deleting: ${name}"
        echo "  --------------------------------------------------"

        # Determine num_inputs from opc.c
        local ni=2
        local fmt
        fmt=$(grep "\"${name}\"" "$OPC_C" 2>/dev/null | sed -n 's/.*"\([^"]*\)".*MATCH_.*/\1/p' | head -1 || true)
        if [[ -n "$fmt" ]]; then
            ni=$(echo "$fmt" | tr -cd ',' | wc -c)
        fi

        delete_from_opc_h "$name"
        delete_from_opc_c "$name"
        delete_from_builtins_cc "$name"
        delete_from_riscv_md "$name"
        delete_from_ftypes_def "$ni"
        delete_from_rv_custom "$name"

        # Clean up demo/src files
        for f in \
            "$DEMO_DIR/main_${name}.c" \
            "$DEMO_DIR/main_${name}.o" \
            "$DEMO_DIR/main_${name}_objdump.txt" \
            "$DEMO_DIR/main_${name}_hexdump.txt" \
            "$DEMO_DIR/build_${name}.sh" \
            "$SRC_DIR/${name}_additions.txt"; do
            if [[ -f "$f" ]]; then
                rm -f "$f"
                info "cleanup" "Removed $(basename "$f")"
            fi
        done

        echo "  Done — \"${name}\" removed"
    done
}

# ══════════════════════════════════════════════════════════════════════
#  GENERATE — Demo C code + build script
# ══════════════════════════════════════════════════════════════════════

generate_demo() {
    local name=$1 ni=$2 desc=$3
    mkdir -p "$DEMO_DIR"

    local builtin="__builtin_riscv_${name}"
    local wrapper="wrapper_${name}"
    local fmt=$(get_format_name "$ni")

    local wrapper_params wrapper_call main_body
    case "$ni" in
        0)
            wrapper_params="void"
            wrapper_call="${builtin}()"
            main_body="    volatile unsigned long result = ${wrapper}();"
            ;;
        1)
            wrapper_params="unsigned long a0"
            wrapper_call="${builtin}(a0)"
            main_body="    unsigned long A = 42;\n    volatile unsigned long result = ${wrapper}((unsigned long)&A);"
            ;;
        2)
            wrapper_params="unsigned long a0, unsigned long a1"
            wrapper_call="${builtin}(a0, a1)"
            main_body="    float A = 2.0f;\n    float B = 4.0f;\n    volatile unsigned long result = ${wrapper}(\n        (unsigned long)&A, (unsigned long)&B);"
            ;;
        3)
            wrapper_params="unsigned long a0, unsigned long a1, unsigned long a2"
            wrapper_call="${builtin}(a0, a1, a2)"
            main_body="    float A = 2.0f;\n    float B = 4.0f;\n    float C = 6.0f;\n    volatile unsigned long result = ${wrapper}(\n        (unsigned long)&A, (unsigned long)&B, (unsigned long)&C);"
            ;;
    esac

    cat > "$DEMO_DIR/main_${name}.c" << ENDOFC
/*
 * main_${name}.c — Demo for custom RISC-V instruction "${name}"
 * Description: ${desc}
 * Inputs: ${ni} (${fmt})
 * Builtin: ${builtin}()
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \\
 *       -ffreestanding -nostdinc -c main_${name}.c -o main_${name}.o
 *
 * Disassemble:
 *   riscv64-unknown-elf-objdump -d main_${name}.o
 */

__attribute__((noinline))
unsigned long ${wrapper}(${wrapper_params})
{
    return ${wrapper_call};
}

int main(void)
{
$(echo -e "${main_body}")
    (void)result;
    return 0;
}
ENDOFC

    ok "demo" "Generated main_${name}.c"

    # Build script
    cat > "$DEMO_DIR/build_${name}.sh" << ENDOFSH
#!/usr/bin/env bash
set -euo pipefail
RISCV_PREFIX="\${RISCV_PREFIX:-riscv64-unknown-elf}"
DIR="\$(cd "\$(dirname "\$0")" && pwd)"

echo "=== Compiling main_${name}.c ==="
"\${RISCV_PREFIX}-gcc" -O2 -march=rv64imac -mabi=lp64 \\
    -ffreestanding -nostdinc \\
    -c "\$DIR/main_${name}.c" -o "\$DIR/main_${name}.o"

echo ""
echo "=== Full Disassembly ==="
"\${RISCV_PREFIX}-objdump" -d "\$DIR/main_${name}.o" | tee "\$DIR/main_${name}_objdump.txt"

echo ""
echo "=== Grep for '${name}' instruction ==="
"\${RISCV_PREFIX}-objdump" -d "\$DIR/main_${name}.o" | grep -n "${name}" || echo "(not found)"

echo ""
echo "Build complete."
ENDOFSH
    chmod +x "$DEMO_DIR/build_${name}.sh"
    ok "demo" "Generated build_${name}.sh"
}

# ══════════════════════════════════════════════════════════════════════
#  BUILD — Rebuild toolchain
# ══════════════════════════════════════════════════════════════════════

rebuild_toolchain() {
    local nproc
    nproc=$(nproc 2>/dev/null || echo 4)

    # Binutils
    local bb="$BUILD_DIR/build-binutils-newlib"
    if [[ ! -d "$bb" ]]; then
        for alt in build-binutils build; do
            [[ -d "$BUILD_DIR/$alt" ]] && bb="$BUILD_DIR/$alt" && break
        done
    fi

    if [[ -d "$bb" ]]; then
        echo ""
        echo "[BUILD] Rebuilding binutils (incremental)..."
        make -C "$bb" -j"$nproc" && make -C "$bb" install
    fi

    # GCC
    local gb=""
    for cand in build-gcc-newlib-stage1 build-gcc-stage1 build-gcc; do
        if [[ -d "$BUILD_DIR/$cand" ]] && [[ -f "$BUILD_DIR/$cand/Makefile" ]]; then
            gb="$BUILD_DIR/$cand"
            break
        fi
    done

    if [[ -n "$gb" ]]; then
        echo ""
        echo "[BUILD] Rebuilding GCC (incremental)..."
        make -C "$gb" all-gcc -j"$nproc" && make -C "$gb" install-gcc
    fi
}

# ══════════════════════════════════════════════════════════════════════
#  COMPILE — Build demo + objdump
# ══════════════════════════════════════════════════════════════════════

compile_and_dump() {
    local name=$1
    local prefix="$INSTALL_PREFIX/bin/riscv64-unknown-elf"
    local src="$DEMO_DIR/main_${name}.c"
    local obj="$DEMO_DIR/main_${name}.o"

    if [[ ! -x "${prefix}-gcc" ]]; then
        warn "COMPILE" "Compiler not found at ${prefix}-gcc — skipping compile/dump"
        echo "  Build the toolchain first (Steps 7-8 in README), then run:"
        echo "    ${prefix}-gcc -O2 -march=rv64imac -mabi=lp64 -ffreestanding -nostdinc -c ${src} -o ${obj}"
        echo "    ${prefix}-objdump -d ${obj}"
        return 0
    fi

    echo ""
    echo "[COMPILE] ${src} → ${obj}"
    "${prefix}-gcc" -O2 -march=rv64imac -mabi=lp64 \
        -ffreestanding -nostdinc \
        -c "$src" -o "$obj"

    echo ""
    echo "[OBJDUMP] Full disassembly:"
    "${prefix}-objdump" -d "$obj" | tee "$DEMO_DIR/main_${name}_objdump.txt"

    echo ""
    echo "[GREP] Lines containing '${name}':"
    "${prefix}-objdump" -d "$obj" | grep -n "$name" || echo "  (not found)"
}

# ══════════════════════════════════════════════════════════════════════
#  ADD — Full add pipeline for one instruction
# ══════════════════════════════════════════════════════════════════════

cmd_add() {
    local name=$1 ni=$2 desc="${3:-Custom instruction}"
    name=$(sanitize_name "$name")

    local fmt=$(get_format_name "$ni")
    echo ""
    echo "  Instruction : ${name}"
    echo "  Inputs      : ${ni} (${fmt})"
    echo "  Description : ${desc}"

    echo ""
    echo "  Finding free opcode slot..."
    if ! find_free_opcode "$ni"; then
        err "FATAL" "No free opcode slots for ${ni}-input instructions!"
        echo "  Use: $0 delete <name> to free up a slot."
        return 1
    fi

    echo "    Base     : ${FREE_BASE_NAME} (${FREE_BASE_OPC})"
    echo "    funct3   : ${FREE_F3}"
    if [[ $ni -le 2 ]]; then
        echo "    funct7   : ${FREE_F7F2}"
    else
        echo "    funct2   : ${FREE_F7F2}"
    fi
    echo "    MATCH    : ${FREE_MATCH}"
    echo "    MASK     : ${FREE_MASK}"

    echo ""
    echo "  Modifying 6 toolchain source files..."
    modify_opc_h "$name" "$FREE_MATCH" "$FREE_MASK"
    modify_opc_c "$name" "$FREE_MATCH" "$FREE_MASK" "$ni"
    modify_ftypes_def "$ni"
    modify_builtins_cc "$name" "$ni"
    modify_riscv_md "$name" "$ni"
    modify_rv_custom "$name" "$ni" "$FREE_BASE_BITS" "$FREE_F3" "$FREE_F7F2"

    echo ""
    echo "  Generating demo..."
    generate_demo "$name" "$ni" "$desc"

    # Export for batch use
    LAST_MATCH="$FREE_MATCH"
    LAST_MASK="$FREE_MASK"
}

# ══════════════════════════════════════════════════════════════════════
#  BATCH — Add multiple instructions from file
# ══════════════════════════════════════════════════════════════════════

cmd_batch() {
    local file=$1
    if [[ ! -f "$file" ]]; then
        err "FATAL" "Batch file not found: ${file}"
        exit 1
    fi

    local count=0
    local names=()
    while IFS= read -r line; do
        line=$(echo "$line" | sed 's/#.*//' | xargs)
        [[ -z "$line" ]] && continue

        local name ni desc
        name=$(echo "$line" | awk '{print $1}')
        ni=$(echo "$line" | awk '{print $2}')
        desc=$(echo "$line" | awk '{$1=""; $2=""; print}' | xargs)
        [[ -z "$desc" ]] && desc="Custom instruction"

        if [[ ! "$ni" =~ ^[0-3]$ ]]; then
            warn "batch" "Skipping '${name}': inputs must be 0-3, got '${ni}'"
            continue
        fi

        (( count++ ))
        if [[ $count -gt 4 ]]; then
            warn "batch" "Max 4 instructions per batch — skipping rest"
            break
        fi

        echo ""
        echo "━━━ Instruction ${count}/4: ${name} ━━━"
        cmd_add "$name" "$ni" "$desc"
        names+=("$name")
    done < "$file"

    echo ""
    echo "================================================================"
    echo "  BATCH DONE — Added ${#names[@]} instruction(s): ${names[*]}"
    echo "================================================================"
}

# ══════════════════════════════════════════════════════════════════════
#  INTERACTIVE — Prompt user
# ══════════════════════════════════════════════════════════════════════

cmd_interactive() {
    echo ""
    read -rp "  Instruction name (e.g., my_pow): " name
    [[ -z "$name" ]] && err "FATAL" "Name cannot be empty" && exit 1

    read -rp "  Number of inputs (0, 1, 2, or 3) [default=2]: " ni
    [[ ! "$ni" =~ ^[0-3]$ ]] && ni=2

    read -rp "  Short description [Custom instruction]: " desc
    [[ -z "$desc" ]] && desc="Custom instruction"

    cmd_add "$name" "$ni" "$desc"
}

# ══════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════

banner

case "${1:-}" in
    add)
        shift
        if [[ $# -lt 2 ]]; then
            echo "Usage: $0 add <name> <inputs:0-3> [description]"
            exit 1
        fi
        name=$1; ni=$2; shift 2
        desc="${*:-Custom instruction}"
        cmd_add "$name" "$ni" "$desc"
        ;;
    delete)
        shift
        if [[ $# -lt 1 ]]; then
            echo "Usage: $0 delete <name> [name2 ...]"
            exit 1
        fi
        cmd_delete "$@"
        ;;
    list)
        cmd_list
        ;;
    scan)
        shift
        if [[ $# -lt 1 ]] || [[ ! "$1" =~ ^[0-3]$ ]]; then
            echo "Usage: $0 scan <inputs:0-3>"
            exit 1
        fi
        cmd_scan "$1"
        ;;
    batch)
        shift
        if [[ $# -lt 1 ]]; then
            echo "Usage: $0 batch <file>"
            exit 1
        fi
        cmd_batch "$1"
        ;;
    help|--help|-h)
        echo ""
        echo "Usage:"
        echo "  $0 add <name> <inputs:0-3> [description]"
        echo "  $0 delete <name> [name2 ...]"
        echo "  $0 list"
        echo "  $0 scan <inputs:0-3>"
        echo "  $0 batch <file>"
        echo "  $0                          # Interactive"
        echo ""
        echo "Examples:"
        echo "  $0 add my_pow 2 '(a^b)^a'"
        echo "  $0 add hw_status 0 'read hardware status'"
        echo "  $0 add my_relu 1 'max(0,x)'"
        echo "  $0 add fused_mac 3 'a*b+c'"
        echo "  $0 delete my_pow"
        echo "  $0 delete my_pow fused_mac"
        echo "  $0 batch instructions_example.txt"
        echo "  $0 scan 2"
        echo "  $0 list"
        ;;
    "")
        cmd_interactive
        ;;
    *)
        err "FATAL" "Unknown command: $1"
        echo "Run '$0 help' for usage."
        exit 1
        ;;
esac

echo ""
echo "Done."
