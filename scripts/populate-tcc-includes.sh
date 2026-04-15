#!/bin/sh
# Populate TCC include tree inside an ext2 image using debugfs.
set -eu

IMG_PATH=${1:-disk.img}
SRC_INCLUDE_DIR=${2:-toolchain/musl-sysroot/opt/tcc/lib/tcc/include}
DST_INCLUDE_DIR=${3:-opt/tcc/lib/tcc/include}

if [ ! -d "$SRC_INCLUDE_DIR" ]; then
  echo "Missing include source directory: $SRC_INCLUDE_DIR" >&2
  exit 1
fi

ensure_dir() {
  rel_dir=$1
  cur="$DST_INCLUDE_DIR"

  old_ifs=$IFS
  IFS='/'
  # shellcheck disable=SC2086
  set -- $rel_dir
  IFS=$old_ifs

  for comp in "$@"; do
    [ -n "$comp" ] || continue
    cur="$cur/$comp"
    debugfs -w -R "mkdir $cur" "$IMG_PATH" >/dev/null 2>&1 || true
  done
}

rg --files "$SRC_INCLUDE_DIR" | while IFS= read -r file_path; do
  rel_path=${file_path#"$SRC_INCLUDE_DIR"/}
  rel_dir=$(dirname "$rel_path")
  if [ "$rel_dir" != "." ]; then
    ensure_dir "$rel_dir"
  fi
  debugfs -w -R "write $file_path $DST_INCLUDE_DIR/$rel_path" "$IMG_PATH" >/dev/null
done
#!/bin/sh
# Populate TCC include tree inside an ext2 image using debugfs.
set -eu

IMG_PATH=${1:-disk.img}
SRC_INCLUDE_DIR=${2:-toolchain/musl-sysroot/opt/tcc/lib/tcc/include}
DST_INCLUDE_DIR=${3:-opt/tcc/lib/tcc/include}

if [ ! -d "$SRC_INCLUDE_DIR" ]; then
  echo "Missing include source directory: $SRC_INCLUDE_DIR" >&2
  exit 1
fi

ensure_dir() {
  rel_dir=$1
  cur="$DST_INCLUDE_DIR"

  old_ifs=$IFS
  IFS='/'
  # shellcheck disable=SC2086
  set -- $rel_dir
  IFS=$old_ifs

  for comp in "$@"; do
    [ -n "$comp" ] || continue
    cur="$cur/$comp"
    debugfs -w -R "mkdir $cur" "$IMG_PATH" >/dev/null 2>&1 || true
  done
}

rg --files "$SRC_INCLUDE_DIR" | while IFS= read -r file_path; do
  rel_path=${file_path#"$SRC_INCLUDE_DIR"/}
  rel_dir=$(dirname "$rel_path")
  if [ "$rel_dir" != "." ]; then
    ensure_dir "$rel_dir"
  fi
  debugfs -w -R "write $file_path $DST_INCLUDE_DIR/$rel_path" "$IMG_PATH" >/dev/null
done
