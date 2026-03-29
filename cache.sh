#!/usr/bin/env bash

set -euo pipefail

DEFAULT_CONFIG_PATH="config.json"
DEFAULT_OUTPUT_DIR="bgpdata"

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

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command_name=""
config_path="$DEFAULT_CONFIG_PATH"
output_dir_override=""

usage() {
  cat <<'EOF'
Usage: ./cache.sh <size|clear> [--config PATH] [--output-dir PATH]

Commands:
  size   Show cached file count and total size
  clear  Delete all cached files under output_dir
EOF
}

extract_output_dir_from_config() {
  local path="$1"
  if [ ! -f "$path" ]; then
    printf "%s\n" "$DEFAULT_OUTPUT_DIR"
    return 0
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
    local resolved_config="$config_path"
    if [[ "$resolved_config" != /* ]]; then
      resolved_config="$repo_root/$resolved_config"
    fi
    output_dir="$(extract_output_dir_from_config "$resolved_config")"
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    size|clear)
      if [ -n "$command_name" ]; then
        echo "Only one command may be provided." >&2
        usage >&2
        exit 1
      fi
      command_name="$1"
      shift
      ;;
    --config)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --config" >&2
        exit 1
      fi
      config_path="$2"
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

cache_root="$(resolve_cache_root)"
mapfile -t cache_summary < <(summarize_cache "$cache_root")
file_count="${cache_summary[0]}"
total_bytes="${cache_summary[1]}"

case "$command_name" in
  size)
    echo "cache_root: $cache_root"
    echo "files: $file_count"
    echo "bytes: $total_bytes"
    echo "human_size: $(format_bytes "$total_bytes")"
    ;;
  clear)
    if [ ! -d "$cache_root" ]; then
      echo "cache_root: $cache_root"
      echo "cache is already empty"
      exit 0
    fi

    rm -rf "$cache_root"
    echo "cache_root: $cache_root"
    echo "removed_files: $file_count"
    echo "removed_bytes: $total_bytes"
    echo "removed_human_size: $(format_bytes "$total_bytes")"
    ;;
esac
