#!/usr/bin/env bash

set -euo pipefail

DEFAULT_OUTPUT_DIR="bgpdata"
DEFAULT_BUILD_DIR="build"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command_name=""
output_dir_override=""
build_dir="$DEFAULT_BUILD_DIR"
run_args=()

usage() {
  cat <<'EOF'
Usage: ./manage.sh <build|run|cache-size|cache-clear> [options] [-- program_args...]

Commands:
  build                      Configure and build the project
  run                        Configure, build, and run the project executable
  cache-size                 Show cached file count and total size
  cache-clear                Delete all cached files under output_dir

Options:
  --build-dir PATH           Override the build directory for build/run
  --output-dir PATH          Override output_dir for cache commands
  --help, -h                 Show this help text
EOF
}

extract_output_dir_from_config() {
  local path="$1"
  if [ ! -f "$path" ]; then
    echo "Required config file does not exist: $path" >&2
    exit 1
  fi

  local extracted
  extracted="$(sed -n 's/^[[:space:]]*"output_dir"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$path" | head -n 1)"
  if [ -n "$extracted" ]; then
    printf "%s\n" "$extracted"
  else
    printf "%s\n" "$DEFAULT_OUTPUT_DIR"
  fi
}

resolve_cache_root() {
  local output_dir
  if [ -n "$output_dir_override" ]; then
    output_dir="$output_dir_override"
  else
    output_dir="$(extract_output_dir_from_config "$repo_root/config.json")"
  fi

  if [[ "$output_dir" = /* ]]; then
    printf "%s\n" "$output_dir"
  else
    printf "%s\n" "$repo_root/$output_dir"
  fi
}

summarize_cache() {
  local cache_root="$1"
  if [ ! -d "$cache_root" ]; then
    printf "0\n0\n"
    return 0
  fi

  local file_count
  local total_bytes
  file_count="$(find "$cache_root" -type f | wc -l | tr -d ' ')"
  total_bytes="$(find "$cache_root" -type f -printf '%s\n' | awk '{sum += $1} END {print sum + 0}')"
  printf "%s\n%s\n" "$file_count" "$total_bytes"
}

format_bytes() {
  local bytes="$1"
  local units=("B" "KiB" "MiB" "GiB" "TiB")
  local unit_index=0
  local value="$bytes"

  while [ "$value" -ge 1024 ] && [ "$unit_index" -lt $((${#units[@]} - 1)) ]; do
    value=$((value / 1024))
    unit_index=$((unit_index + 1))
  done

  printf "%s %s\n" "$value" "${units[$unit_index]}"
}

run_build() {
  local resolved_build_dir
  resolved_build_dir="$(resolve_build_dir)"

  cmake -S "$repo_root" -B "$resolved_build_dir"
  cmake --build "$resolved_build_dir"
}

resolve_build_dir() {
  local resolved_build_dir
  if [[ "$build_dir" = /* ]]; then
    resolved_build_dir="$build_dir"
  else
    resolved_build_dir="$repo_root/$build_dir"
  fi

  printf "%s\n" "$resolved_build_dir"
}

run_program() {
  local resolved_build_dir
  local executable_path
  resolved_build_dir="$(resolve_build_dir)"
  executable_path="$resolved_build_dir/bgpstream_prefix_stats"

  if [ ! -x "$executable_path" ]; then
    echo "Executable does not exist or is not runnable: $executable_path" >&2
    exit 1
  fi

  "$executable_path" "${run_args[@]}"
}

run_cache_size() {
  local cache_root="$1"
  mapfile -t cache_summary < <(summarize_cache "$cache_root")
  local file_count="${cache_summary[0]}"
  local total_bytes="${cache_summary[1]}"

  echo "cache_root: $cache_root"
  echo "files: $file_count"
  echo "bytes: $total_bytes"
  echo "human_size: $(format_bytes "$total_bytes")"
}

run_cache_clear() {
  local cache_root="$1"
  mapfile -t cache_summary < <(summarize_cache "$cache_root")
  local file_count="${cache_summary[0]}"
  local total_bytes="${cache_summary[1]}"

  if [ ! -d "$cache_root" ]; then
    echo "cache_root: $cache_root"
    echo "cache is already empty"
    return 0
  fi

  rm -rf "$cache_root"
  echo "cache_root: $cache_root"
  echo "removed_files: $file_count"
  echo "removed_bytes: $total_bytes"
  echo "removed_human_size: $(format_bytes "$total_bytes")"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    build|run|cache-size|cache-clear)
      if [ -n "$command_name" ]; then
        echo "Only one command may be provided." >&2
        usage >&2
        exit 1
      fi
      command_name="$1"
      shift
      ;;
    --)
      shift
      run_args=("$@")
      break
      ;;
    --build-dir)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --build-dir" >&2
        exit 1
      fi
      build_dir="$2"
      shift 2
      ;;
    --output-dir)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --output-dir" >&2
        exit 1
      fi
      output_dir_override="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "$command_name" ]; then
  usage >&2
  exit 1
fi

case "$command_name" in
  build)
    run_build
    ;;
  run)
    run_build
    run_program
    ;;
  cache-size)
    run_cache_size "$(resolve_cache_root)"
    ;;
  cache-clear)
    run_cache_clear "$(resolve_cache_root)"
    ;;
esac
