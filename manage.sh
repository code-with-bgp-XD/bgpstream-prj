#!/usr/bin/env bash

set -euo pipefail

DEFAULT_OUTPUT_DIR="bgpdata"
DEFAULT_DEBUG_BUILD_DIR="build"
DEFAULT_RELEASE_BUILD_DIR="build-release"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command_name=""
output_dir_override=""
build_dir=""
run_args=()
download_project=""
download_collector=""
download_start_date=""
download_end_date=""
download_workers=""
download_parser_workers=""
download_message_batch_size=""
download_limit=""
download_option_used=false

usage() {
  cat <<'EOF'
Usage: ./manage.sh <command> [options] [-- program_args...]

Commands:
  build                      Configure and build Debug into build/
  build-release              Configure and build Release into build-release/
  run                        Configure, build, and run the Debug executable
  run-release                Configure, build, and run the Release executable
  download                   Build Debug, download MRT files, and generate parsed caches
  download-release           Same as download, using the Release executable
  cache-size                 Show cached file count and total size
  cache-clear                Delete generated parsed caches only; preserve source MRT files

Options:
  --build-dir PATH           Override the directory used by build/run/download commands
  --output-dir PATH          Override output_dir for cache-size/cache-clear only
  --project NAME             Temporarily override cache.project for download commands
  --collector NAME           Temporarily override cache.collector for download commands
  --start-date YYYY-MM-DD    Temporarily override cache.start_date for download commands
  --end-date YYYY-MM-DD      Temporarily override cache.end_date for download commands
  --download-workers N       Override concurrency for download commands
  --parser-workers N         Override preparse concurrency for download commands
  --message-batch-size N     Override preparse batch size for download commands
  --limit N                  Limit matched files for download commands (for testing)
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
  total_bytes="$(find "$cache_root" -type f -printf '%s\n' | awk 'BEGIN { sum = 0 } { sum += $1 } END { printf "%.0f\n", sum }')"
  printf "%s\n%s\n" "$file_count" "$total_bytes"
}

format_bytes() {
  local bytes="$1"
  awk -v bytes="$bytes" '
    BEGIN {
      split("B KiB MiB GiB TiB", units, " ")
      unit_index = 1
      value = bytes + 0

      while (value >= 1024 && unit_index < length(units)) {
        value /= 1024
        unit_index += 1
      }

      if (unit_index == 1) {
        printf "%.0f %s\n", value, units[unit_index]
      } else {
        printf "%.1f %s\n", value, units[unit_index]
      }
    }
  '
}

run_build() {
  local build_type="$1"
  local resolved_build_dir
  resolved_build_dir="$(resolve_build_dir)"

  cmake -S "$repo_root" -B "$resolved_build_dir" -DCMAKE_BUILD_TYPE="$build_type"
  cmake --build "$resolved_build_dir"
}

resolve_build_dir() {
  local resolved_build_dir
  local selected_build_dir="$build_dir"
  if [ -z "$selected_build_dir" ]; then
    if [[ "$command_name" == *-release ]]; then
      selected_build_dir="$DEFAULT_RELEASE_BUILD_DIR"
    else
      selected_build_dir="$DEFAULT_DEBUG_BUILD_DIR"
    fi
  fi

  if [[ "$selected_build_dir" = /* ]]; then
    resolved_build_dir="$selected_build_dir"
  else
    resolved_build_dir="$repo_root/$selected_build_dir"
  fi

  printf "%s\n" "$resolved_build_dir"
}

run_program() {
  local resolved_build_dir
  local executable_path
  resolved_build_dir="$(resolve_build_dir)"
  executable_path="$resolved_build_dir/bgpstream_analyzer"

  if [ ! -x "$executable_path" ]; then
    echo "Executable does not exist or is not runnable: $executable_path" >&2
    exit 1
  fi

  "$executable_path" "${run_args[@]}"
}

run_download() {
  local resolved_build_dir
  local executable_path
  local -a download_args
  resolved_build_dir="$(resolve_build_dir)"
  executable_path="$resolved_build_dir/bgpstream_analyzer"

  if [ ! -x "$executable_path" ]; then
    echo "Executable does not exist or is not runnable: $executable_path" >&2
    exit 1
  fi

  download_args=(
    --download-only
  )
  if [ -n "$download_project" ]; then
    download_args+=(--project "$download_project")
  fi
  if [ -n "$download_collector" ]; then
    download_args+=(--collector "$download_collector")
  fi
  if [ -n "$download_start_date" ]; then
    download_args+=(--start-date "$download_start_date")
  fi
  if [ -n "$download_end_date" ]; then
    download_args+=(--end-date "$download_end_date")
  fi
  if [ -n "$download_workers" ]; then
    download_args+=(--download-workers "$download_workers")
  fi
  if [ -n "$download_parser_workers" ]; then
    download_args+=(--parser-workers "$download_parser_workers")
  fi
  if [ -n "$download_message_batch_size" ]; then
    download_args+=(--message-batch-size "$download_message_batch_size")
  fi
  if [ -n "$download_limit" ]; then
    download_args+=(--limit "$download_limit")
  fi

  "$executable_path" "${download_args[@]}"
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
  if [ ! -d "$cache_root" ]; then
    echo "cache_root: $cache_root"
    echo "cache is already empty"
    return 0
  fi

  local file_count
  local total_bytes
  file_count="$(find "$cache_root" -type f -path '*/parsed-cache/*' \
    \( -name '*.bgpcache' -o -name '*.bgpcache.part' \) | wc -l | tr -d ' ')"
  total_bytes="$(find "$cache_root" -type f -path '*/parsed-cache/*' \
    \( -name '*.bgpcache' -o -name '*.bgpcache.part' \) -printf '%s\n' |
    awk 'BEGIN { sum = 0 } { sum += $1 } END { printf "%.0f\n", sum }')"
  find "$cache_root" -type f -path '*/parsed-cache/*' \
    \( -name '*.bgpcache' -o -name '*.bgpcache.part' \) -delete
  find "$cache_root" -type d -name parsed-cache -empty -delete
  echo "cache_root: $cache_root"
  echo "source_mrt_files_preserved: true"
  echo "removed_files: $file_count"
  echo "removed_bytes: $total_bytes"
  echo "removed_human_size: $(format_bytes "$total_bytes")"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    build|build-release|run|run-release|download|download-release|cache-size|cache-clear)
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
    --project)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --project" >&2
        exit 1
      fi
      download_project="$2"
      download_option_used=true
      shift 2
      ;;
    --collector)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --collector" >&2
        exit 1
      fi
      download_collector="$2"
      download_option_used=true
      shift 2
      ;;
    --start-date)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --start-date" >&2
        exit 1
      fi
      download_start_date="$2"
      download_option_used=true
      shift 2
      ;;
    --end-date)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --end-date" >&2
        exit 1
      fi
      download_end_date="$2"
      download_option_used=true
      shift 2
      ;;
    --download-workers)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --download-workers" >&2
        exit 1
      fi
      download_workers="$2"
      download_option_used=true
      shift 2
      ;;
    --parser-workers)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --parser-workers" >&2
        exit 1
      fi
      download_parser_workers="$2"
      download_option_used=true
      shift 2
      ;;
    --message-batch-size)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --message-batch-size" >&2
        exit 1
      fi
      download_message_batch_size="$2"
      download_option_used=true
      shift 2
      ;;
    --limit)
      if [ "$#" -lt 2 ]; then
        echo "Missing value for --limit" >&2
        exit 1
      fi
      download_limit="$2"
      download_option_used=true
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

if [[ "$command_name" != "download" && "$command_name" != "download-release" ]] &&
  [ "$download_option_used" = true ]; then
  echo "Download source/date options may only be used with download or download-release." >&2
  usage >&2
  exit 1
fi

if [[ "$command_name" == "download" || "$command_name" == "download-release" ]] &&
  [ -n "$output_dir_override" ]; then
  echo "The download directory is fixed by cache.output_dir in config.json." >&2
  exit 1
fi

case "$command_name" in
  build)
    run_build Debug
    ;;
  build-release)
    run_build Release
    ;;
  run)
    run_build Debug
    run_program
    ;;
  run-release)
    run_build Release
    run_program
    ;;
  download)
    run_build Debug
    run_download
    ;;
  download-release)
    run_build Release
    run_download
    ;;
  cache-size)
    run_cache_size "$(resolve_cache_root)"
    ;;
  cache-clear)
    run_cache_clear "$(resolve_cache_root)"
    ;;
esac
