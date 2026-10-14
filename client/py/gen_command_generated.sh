#!/usr/bin/env sh
#
# Regenerate the Python FlatBuffers bindings for the rana_socket package.
#
# `rana_socket/command_generated.py` is generated output and is NOT tracked in
# git (see ../.gitignore). The normal C++ build produces it automatically via
# premake's gen_schema_daemon (which copies it in from schema/generated); run
# this script instead when you only work on the Python client and don't want to
# build the daemon.
#
# The flatc version MUST match the `flatbuffers==` pin in pyproject.toml — the
# Python runtime API surface is version-locked to the generator, so a mismatch
# breaks vectors of tables at runtime.
#
# Usage:  ./gen_command_generated.sh
# Env:    FLATC            flatc binary to use (default: flatc from PATH)
#
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)

: "${FLATC:=flatc}"
command -v "$FLATC" >/dev/null 2>&1 || {
    echo "error: '$FLATC' not found on PATH; install flatbuffers 25.12.19 and/or set FLATC" >&2
    exit 1
}

"$FLATC" --gen-onefile --python -o "$here/rana_socket" "$root/schema/command.fbs"
echo "generated: $here/rana_socket/command_generated.py"
