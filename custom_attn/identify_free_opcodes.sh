#!/usr/bin/env bash
# ================================================================
# identify_free_opcodes.sh — Scan for free RISC-V custom opcode slots
# ================================================================
# Group 9 | RISC-V GNU Toolchain
#
# Scans riscv-opc.h and riscv-opcodes/extensions/ to find which
# custom opcode slots are free for new instructions.
#
# Usage:
#   ./identify_free_opcodes.sh              # Summary view
#   ./identify_free_opcodes.sh --detailed   # Show first 20 free slots
#   ./identify_free_opcodes.sh --check 0x0200000b  # Check specific MATCH
#   ./identify_free_opcodes.sh --riscv-opcodes      # Also scan riscv-opcodes dir
#
# Requirements:
#   - Run from the custom_attn/ directory
#   - binutils submodule must be initialized
# ================================================================

set -euo pipefail

# ── Colors ──────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── Paths ───────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
OPC_H="${REPO_ROOT}/binutils/include/opcode/riscv-opc.h"
RISCV_OPCODES_DIR="${REPO_ROOT}/riscv-opcodes/extensions"
RISCV_OPCODES_HOME="${HOME}/riscv-opcodes/extensions"

# ── Custom opcode bases ────────────────────────────────────────
declare -A CUSTOM_BASES=(
    ["custom-0"]="0x0B"
    ["custom-1"]="0x2B"
    ["custom-2"]="0x5B"
    ["custom-3"]="0x7B"
)

declare -A BASE_BITS=(
    ["0x0B"]="0x02"
    ["0x2B"]="0x0A"
    ["0x5B"]="0x16"
    ["0x7B"]="0x1E"
)

# Order for iteration
BASE_ORDER=("custom-0" "custom-1" "custom-2" "custom-3")

# ── Functions ──────────────────────────────────────────────────

check_opc_h() {
    if [[ ! -f "$OPC_H" ]]; then
        echo -e "${RED}ERROR:${NC} riscv-opc.h not found at:"
        echo "  $OPC_H"
        echo ""
        echo "Make sure binutils submodule is initialized:"
        echo "  git submodule update --init binutils"
        exit 1
    fi
    echo -e "  Source: ${CYAN}${OPC_H}${NC}"
}

# Extract all MATCH_* hex values from riscv-opc.h into a temp file
extract_match_values() {
    grep -oP '#define\s+MATCH_\w+\s+\K0x[0-9a-fA-F]+' "$OPC_H" | \
        awk '{printf "0x%08x\n", strtonum($1)}' | sort -u > "$MATCH_FILE"
    local count
    count=$(wc -l < "$MATCH_FILE")
    echo "  Found ${count} existing MATCH values in riscv-opc.h"
}

# Check if a MATCH value exists in the extracted set
is_taken() {
    local val
    val=$(printf "0x%08x" "$1")
    grep -qx "$val" "$MATCH_FILE"
}

# Get the name of the instruction that uses a MATCH value
get_match_name() {
    local val
    val=$(printf "0x%08x" "$1")
    # Also try without leading zeros
    local val_alt
    val_alt=$(printf "0x%x" "$1")
    grep -P "#define\s+MATCH_\w+\s+(${val}|${val_alt})\b" "$OPC_H" | \
        head -1 | grep -oP 'MATCH_\K\w+' || echo "unknown"
}

# Compute MATCH for R-type: opcode | (funct3 << 12) | (funct7 << 25)
compute_match_rtype() {
    local base=$1 f3=$2 f7=$3
    echo $(( base | (f3 << 12) | (f7 << 25) ))
}

# Compute MATCH for R4-type: opcode | (funct3 << 12) | (funct2 << 25)
compute_match_r4type() {
    local base=$1 f3=$2 f2=$3
    echo $(( base | (f3 << 12) | (f2 << 25) ))
}

# ── Summary mode ───────────────────────────────────────────────

show_summary() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║         Free RISC-V Custom Opcode Slots — Summary          ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""

    # R-type (0/1/2 inputs)
    echo -e "  ${BOLD}R-type (0/1/2 inputs):${NC}  funct3 × funct7 = 8 × 128 = 1024 per base"
    printf "  %-14s %-12s %-8s %-8s\n" "Base" "Total" "Used" "Free"
    echo "  ------------------------------------------------"

    local total_free_r=0
    for name in "${BASE_ORDER[@]}"; do
        local base_hex="${CUSTOM_BASES[$name]}"
        local base=$((base_hex))
        local used=0
        local total=1024

        for f3 in $(seq 0 7); do
            for f7 in $(seq 0 127); do
                local m
                m=$(compute_match_rtype $base $f3 $f7)
                if is_taken "$m"; then
                    used=$((used + 1))
                fi
            done
        done

        local free=$((total - used))
        total_free_r=$((total_free_r + free))

        if [[ $free -eq $total ]]; then
            printf "  %-14s %-12s %-8s ${GREEN}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        elif [[ $free -eq 0 ]]; then
            printf "  %-14s %-12s %-8s ${RED}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        else
            printf "  %-14s %-12s %-8s ${YELLOW}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        fi
    done
    echo -e "  ${BOLD}TOTAL${NC}                                      ${BOLD}${total_free_r}${NC}"
    echo ""

    # R4-type (3 inputs)
    echo -e "  ${BOLD}R4-type (3 inputs):${NC}  funct3 × funct2 = 8 × 4 = 32 per base"
    printf "  %-14s %-12s %-8s %-8s\n" "Base" "Total" "Used" "Free"
    echo "  ------------------------------------------------"

    local total_free_r4=0
    for name in "${BASE_ORDER[@]}"; do
        local base_hex="${CUSTOM_BASES[$name]}"
        local base=$((base_hex))
        local used=0
        local total=32

        for f3 in $(seq 0 7); do
            for f2 in $(seq 0 3); do
                local m
                m=$(compute_match_r4type $base $f3 $f2)
                if is_taken "$m"; then
                    used=$((used + 1))
                fi
            done
        done

        local free=$((total - used))
        total_free_r4=$((total_free_r4 + free))

        if [[ $free -eq $total ]]; then
            printf "  %-14s %-12s %-8s ${GREEN}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        elif [[ $free -eq 0 ]]; then
            printf "  %-14s %-12s %-8s ${RED}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        else
            printf "  %-14s %-12s %-8s ${YELLOW}%-8s${NC}\n" "$name" "$total" "$used" "$free"
        fi
    done
    echo -e "  ${BOLD}TOTAL${NC}                                      ${BOLD}${total_free_r4}${NC}"
    echo ""
}

# ── Detailed mode ──────────────────────────────────────────────

show_detailed() {
    local num_inputs=${1:-2}
    local limit=${2:-20}

    if [[ $num_inputs -le 2 ]]; then
        local funct_label="funct7"
        local type_label="R-type"
    else
        local funct_label="funct2"
        local type_label="R4-type"
    fi

    echo ""
    echo -e "  Free opcode slots for ${BOLD}${num_inputs}-input (${type_label})${NC} instructions"
    echo "  (showing first ${limit}):"
    echo ""
    printf "  %-14s %-8s %-10s %-14s %-14s\n" "Base" "funct3" "$funct_label" "MATCH" "MASK"
    echo "  ------------------------------------------------------------"

    local count=0
    for name in "${BASE_ORDER[@]}"; do
        local base_hex="${CUSTOM_BASES[$name]}"
        local base=$((base_hex))

        if [[ $num_inputs -le 2 ]]; then
            local mask="0xfe00707f"
            for f3 in $(seq 0 7); do
                for f7 in $(seq 0 127); do
                    local m
                    m=$(compute_match_rtype $base $f3 $f7)
                    if ! is_taken "$m"; then
                        printf "  %-14s %-8s %-10s 0x%08x    %s\n" "$name" "$f3" "$f7" "$m" "$mask"
                        count=$((count + 1))
                        if [[ $count -ge $limit ]]; then
                            echo "  ... (showing first ${limit} of many)"
                            echo ""
                            return
                        fi
                    fi
                done
            done
        else
            local mask="0x0600707f"
            for f3 in $(seq 0 7); do
                for f2 in $(seq 0 3); do
                    local m
                    m=$(compute_match_r4type $base $f3 $f2)
                    if ! is_taken "$m"; then
                        printf "  %-14s %-8s %-10s 0x%08x    %s\n" "$name" "$f3" "$f2" "$m" "$mask"
                        count=$((count + 1))
                        if [[ $count -ge $limit ]]; then
                            echo "  ... (showing first ${limit} of many)"
                            echo ""
                            return
                        fi
                    fi
                done
            done
        fi
    done

    echo ""
    echo "  Total free: ${count}"
}

# ── Check mode ─────────────────────────────────────────────────

check_match() {
    local val=$((${1}))

    if is_taken "$val"; then
        local name
        name=$(get_match_name "$val")
        echo -e "\n  0x$(printf '%08x' $val) is ${RED}TAKEN${NC} by: MATCH_${name}"
    else
        echo -e "\n  0x$(printf '%08x' $val) is ${GREEN}FREE${NC}"
    fi

    # Decode
    local opcode=$((val & 0x7F))
    local funct3=$(( (val >> 12) & 0x7 ))
    local funct7=$(( (val >> 25) & 0x7F ))

    local base_name="unknown"
    for name in "${BASE_ORDER[@]}"; do
        local base_hex="${CUSTOM_BASES[$name]}"
        if [[ $((base_hex)) -eq $opcode ]]; then
            base_name="$name"
            break
        fi
    done

    printf "  Decoding: opcode=0x%02x (%s) funct3=%d funct7=%d\n" "$opcode" "$base_name" "$funct3" "$funct7"
}

# ── riscv-opcodes scan mode ────────────────────────────────────

scan_riscv_opcodes() {
    local ext_dir=""
    if [[ -d "$RISCV_OPCODES_DIR" ]]; then
        ext_dir="$RISCV_OPCODES_DIR"
    elif [[ -d "$RISCV_OPCODES_HOME" ]]; then
        ext_dir="$RISCV_OPCODES_HOME"
    fi

    if [[ -z "$ext_dir" ]]; then
        echo -e "  ${YELLOW}riscv-opcodes/extensions/ not found — skipping${NC}"
        return
    fi

    echo -e "  Scanning: ${CYAN}${ext_dir}${NC}"

    # Extract used base opcodes (bits[6:2])
    local used_file
    used_file=$(mktemp)
    grep -ohP '6\.\.2=0x[0-9a-fA-F]+' -R "$ext_dir" 2>/dev/null | \
        sed 's/.*=0x//' | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | \
        sort -u > "$used_file"

    # All possible 5-bit values
    local all_file
    all_file=$(mktemp)
    for i in $(seq 0 31); do printf "0x%02x\n" "$i"; done > "$all_file"

    # Free bases
    echo ""
    echo -e "  ${BOLD}Free base opcodes (bits[6:2]):${NC}"
    local free
    free=$(comm -23 "$all_file" "$used_file")
    echo "$free" | while read -r val; do
        # Check if it's a custom slot
        local is_custom=""
        case "$val" in
            0x02) is_custom=" ← custom-0 (0x0B)" ;;
            0x0a) is_custom=" ← custom-1 (0x2B)" ;;
            0x16) is_custom=" ← custom-2 (0x5B)" ;;
            0x1e) is_custom=" ← custom-3 (0x7B)" ;;
        esac
        if [[ -n "$is_custom" ]]; then
            echo -e "    ${GREEN}${val}${NC}${is_custom}"
        else
            echo "    ${val}"
        fi
    done

    rm -f "$used_file" "$all_file"
}

# ── Main ───────────────────────────────────────────────────────

MODE="summary"
NUM_INPUTS=2
LIMIT=20
CHECK_VAL=""
DO_OPCODES_SCAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --detailed)  MODE="detailed"; shift ;;
        --inputs)    NUM_INPUTS="$2"; shift 2 ;;
        --limit)     LIMIT="$2"; shift 2 ;;
        --check)     MODE="check"; CHECK_VAL="$2"; shift 2 ;;
        --riscv-opcodes) DO_OPCODES_SCAN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--detailed] [--inputs N] [--limit N] [--check 0xHEX] [--riscv-opcodes]"
            echo ""
            echo "Options:"
            echo "  --detailed         Show detailed list of free slots"
            echo "  --inputs N         Number of inputs: 0-2 (R-type) or 3 (R4-type)"
            echo "  --limit N          Max slots in detailed mode (default: 20)"
            echo "  --check 0xHEX      Check if a specific MATCH value is free"
            echo "  --riscv-opcodes    Also scan riscv-opcodes/extensions/"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo ""
echo "================================================================"
echo "  RISC-V Custom Opcode Scanner (Group 9)"
echo "================================================================"
echo ""

# Create temp file for MATCH values
MATCH_FILE=$(mktemp)
trap "rm -f $MATCH_FILE" EXIT

check_opc_h
extract_match_values

if $DO_OPCODES_SCAN; then
    scan_riscv_opcodes
fi

case "$MODE" in
    summary)  show_summary ;;
    detailed) show_detailed "$NUM_INPUTS" "$LIMIT" ;;
    check)    check_match "$CHECK_VAL" ;;
esac

# Always show the recommended first free slot
echo -e "  ${BOLD}Recommended next free slot:${NC}"
for name in "${BASE_ORDER[@]}"; do
    local_base="${CUSTOM_BASES[$name]}"
    base=$((local_base))
    for f3 in $(seq 0 7); do
        for f7 in $(seq 0 127); do
            m=$(compute_match_rtype $base $f3 $f7)
            if ! is_taken "$m"; then
                printf "    Base: %s (0x%02x)  funct3=%d  funct7=%d\n" "$name" "$base" "$f3" "$f7"
                printf "    MATCH=0x%08x  MASK=0xfe00707f\n" "$m"
                echo ""
                exit 0
            fi
        done
    done
done

echo "    WARNING: No free slots found!"
