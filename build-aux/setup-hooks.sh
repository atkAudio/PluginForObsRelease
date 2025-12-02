#!/bin/bash
# Sets up git hooks for the project

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
GIT_HOOKS_DIR="$REPO_ROOT/.git/hooks"
SOURCE_HOOK="$SCRIPT_DIR/pre-commit"
DEST_HOOK="$GIT_HOOKS_DIR/pre-commit"

if [ ! -d "$GIT_HOOKS_DIR" ]; then
    echo "Error: Git hooks directory not found. Are you in a git repository?"
    exit 1
fi

if [ ! -f "$SOURCE_HOOK" ]; then
    echo "Error: Source pre-commit hook not found at: $SOURCE_HOOK"
    exit 1
fi

cp "$SOURCE_HOOK" "$DEST_HOOK"
chmod +x "$DEST_HOOK"
echo "Pre-commit hook installed successfully!"

# Check for required tools
echo ""
echo "Checking for required tools..."

if command -v clang-format >/dev/null 2>&1; then
    echo "  clang-format: $(clang-format --version)"
else
    echo "  clang-format: NOT FOUND"
    echo "    Install LLVM or add clang-format to PATH"
fi

if command -v gersemi >/dev/null 2>&1; then
    echo "  gersemi: $(gersemi --version)"
else
    echo "  gersemi: NOT FOUND"
    echo "    Install with: pip install gersemi"
fi
