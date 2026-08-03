#!/bin/bash
# Shim for nvcc that produces empty output files without compiling.
# Responds to --version queries with real nvcc output so CMake is satisfied.
# Usage: place this earlier in PATH than real nvcc, or set CMAKE_CUDA_COMPILER to it.

REAL_NVCC="/usr/local/cuda/bin/nvcc"

output_file=""
dryrun=0
version=0
fatbin=0

# Parse args to find -o <output> and detect --version/--dryrun
args=("$@")
for ((i=0; i<${#args[@]}; i++)); do
    case "${args[$i]}" in
        -o)
            output_file="${args[$((i+1))]}"
            ;;
        --version|-V)
            version=1
            ;;
        --dryrun|-dryrun)
            dryrun=1
            ;;
        -fatbin|--fatbin)
            fatbin=1
            ;;
    esac
done

# Version queries: hardcoded output so no real CUDA toolkit is needed
if [[ $version -eq 1 ]]; then
    cat <<'EOF'
nvcc: NVIDIA (R) Cuda compiler driver
Copyright (c) 2005-2025 NVIDIA Corporation
Built on Wed_Mar_12_19:20:56_PDT_2025
Cuda compilation tools, release 12.8, V12.8.93
Build cuda_12.8.r12.8/compiler.35583870_0
EOF
    exit 0
fi

# Dryrun: return success
if [[ $dryrun -eq 1 ]]; then
    exit 0
fi

# If there's an output file, create an empty artifact of the right type
if [[ -n "$output_file" ]]; then
    mkdir -p "$(dirname "$output_file")"
    case "$output_file" in
        *.o|*.obj)
            # Produce a minimal ELF relocatable so the linker doesn't choke
            echo -ne '\x7fELF\x02\x01\x01\x00' > "$output_file"
            dd if=/dev/zero bs=1 count=56 >> "$output_file" 2>/dev/null
            ;;
        *.fatbin|*.cubin)
            touch "$output_file"
            ;;
        *)
            touch "$output_file"
            ;;
    esac
fi

exit 0
