#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="$REPO_ROOT/build/vitis_workspace"

if ! command -v xsct >/dev/null 2>&1; then
  echo "ERROR: xsct was not found. Source the Vitis 2022.2 settings script first." >&2
  exit 1
fi

xsct "$SCRIPT_DIR/create_vitis_project.tcl" "$WORKSPACE"
echo "Build complete: $WORKSPACE"
