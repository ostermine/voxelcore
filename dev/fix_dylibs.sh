#!/bin/bash

# Copies the non-system dylibs the binary needs, and the ones they need in
# turn, into libs/ next to it and points every reference at the copies, so the
# build runs without Homebrew. Fails if a reference outside libs/ is left.
set -e

PROJECT_NAME=$1
CONFIG=$2
OUTPUT_DIR=$3
LIBS_DIR="$OUTPUT_DIR/libs"
BREW_LIB="$(brew --prefix)/lib"

mkdir -p "$LIBS_DIR"

TMP_BINARY="$OUTPUT_DIR/tmp_$PROJECT_NAME"
BINARY="$OUTPUT_DIR/$PROJECT_NAME"

cp "$BINARY" "$TMP_BINARY"

is_system() {
    [[ "$1" == /usr/lib/* || "$1" == /System/Library/* ]]
}

# non-system references of a Mach-O file, without its own install name
references() {
    local id
    id=$(otool -D "$1" | tail -n +2)
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r ref; do
        if [[ "$ref" != "$id" ]] && ! is_system "$ref"; then
            echo "$ref"
        fi
    done
}

# the file a reference points to: @rpath/@loader_path ones are in Homebrew
resolve() {
    case "$1" in
        @*) echo "$BREW_LIB/$(basename "$1")" ;;
        *) echo "$1" ;;
    esac
}

queue=("$TMP_BINARY")
while [ ${#queue[@]} -gt 0 ]; do
    file=${queue[0]}
    queue=("${queue[@]:1}")
    for ref in $(references "$file"); do
        if [[ "$ref" == @executable_path/libs/* ]]; then
            continue
        fi
        name=$(basename "$ref")
        if [ ! -f "$LIBS_DIR/$name" ]; then
            src=$(resolve "$ref")
            if [ ! -f "$src" ]; then
                echo "fix_dylibs: $ref of $file is not found" >&2
                exit 1
            fi
            cp -L "$src" "$LIBS_DIR/$name"
            chmod u+w "$LIBS_DIR/$name"
            install_name_tool -id "@executable_path/libs/$name" "$LIBS_DIR/$name"
            queue+=("$LIBS_DIR/$name")
        fi
        install_name_tool -change "$ref" "@executable_path/libs/$name" "$file"
    done
done

# install_name_tool invalidates the signatures, arm64 runs only signed code
for file in "$TMP_BINARY" "$LIBS_DIR"/*.dylib; do
    codesign --force --sign - "$file"
done

mv "$TMP_BINARY" "$BINARY"

for file in "$BINARY" "$LIBS_DIR"/*.dylib; do
    left=$(references "$file" | grep -v '^@executable_path/libs/' || true)
    if [ -n "$left" ]; then
        echo "fix_dylibs: $file still refers to $left" >&2
        exit 1
    fi
done
