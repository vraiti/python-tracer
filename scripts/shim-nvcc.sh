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

# Version queries must return real output (CMake parses this)
if [[ $version -eq 1 ]]; then
    exec "$REAL_NVCC" --version
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
