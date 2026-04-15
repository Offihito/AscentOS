#!/bin/sh
# Populate directory tree inside an ext2 image using debugfs.
set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=$(dirname "$SCRIPT_DIR")

IMG_PATH=${1:-$ROOT_DIR/disk.img}
# Ensure IMG_PATH is absolute because we cd into SRC_DIR later
IMG_PATH=$(readlink -f "$IMG_PATH")
SRC_DIR=${2:-}
# Ensure SRC_DIR is absolute because we cd into it later
[ -n "$SRC_DIR" ] && SRC_DIR=$(readlink -f "$SRC_DIR")
DST_DIR=${3:-}

if [ -z "$SRC_DIR" ] || [ -z "$DST_DIR" ]; then
  echo "Usage: $0 <img_path> <src_dir> <dst_dir>"
  exit 1
fi

# Remove trailing slashes
SRC_DIR=${SRC_DIR%/}
DST_DIR=${DST_DIR%/}

if [ ! -d "$SRC_DIR" ]; then
  echo "Missing source directory: $SRC_DIR" >&2
  exit 1
fi

ensure_dir() {
  cur=""
  old_ifs=$IFS
  IFS='/'
  # shellcheck disable=SC2086
  set -- $1
  IFS=$old_ifs

  for comp in "$@"; do
    [ -n "$comp" ] || continue
    if [ -z "$cur" ]; then
      cur="$comp"
    else
      cur="$cur/$comp"
    fi
    debugfs -w -R "mkdir $cur" "$IMG_PATH" >/dev/null 2>&1 || true
  done
}

ensure_dir "$DST_DIR"

# Using cd to avoid absolute path stripping issues
(
  cd "$SRC_DIR"
  find . -type f | while IFS= read -r file_path; do
    # Remove leading ./ from file_path
    rel_path=${file_path#./}
    rel_dir=$(dirname "$rel_path")
    if [ "$rel_dir" != "." ]; then
      ensure_dir "$DST_DIR/$rel_dir"
    fi
    debugfs -w -R "write $SRC_DIR/$rel_path $DST_DIR/$rel_path" "$IMG_PATH" >/dev/null
  done
)
