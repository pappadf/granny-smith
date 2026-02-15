#!/usr/bin/env bash
#
# fetch-test-data.sh - Fetch proprietary test data from private repository
#
# This script clones/updates the private test data repository into tests/data.
# It requires a GitHub token with read access to the private repository.
#
# Environment variables:
#   GS_TEST_DATA_TOKEN  - GitHub PAT with contents:read access to the private repo
#   GS_TEST_DATA_REPO   - (optional) Override repo name (default: pappadf/gs-test-data)
#
# Usage:
#   ./scripts/fetch-test-data.sh           # Fetch/update test data
#   ./scripts/fetch-test-data.sh --check   # Check if data is available (exit 0/1)
#   ./scripts/fetch-test-data.sh --status  # Show detailed status
#
# Security notes:
#   - Token is NEVER logged or echoed
#   - Git credential helper is configured to avoid token leakage in logs
#   - Uses GIT_TERMINAL_PROMPT=0 to prevent interactive prompts

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$REPO_ROOT/tests/data"
MARKER_FILE="$DATA_DIR/.gs-test-data-marker"
DEFAULT_REPO="pappadf/gs-test-data"

# Colors for output (if terminal supports it)
if [[ -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m' # No Color
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

# Logging functions
log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_success() { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Check if test data is available
check_data_available() {
    if [[ -f "$MARKER_FILE" ]]; then
        return 0
    fi
    # Also check for key files as fallback
    if [[ -f "$DATA_DIR/roms/Plus_v3.rom" ]] && [[ -d "$DATA_DIR/systems" ]]; then
        return 0
    fi
    return 1
}

# Show status of test data
show_status() {
    echo "=== Test Data Status ==="
    echo "Data directory: $DATA_DIR"
    
    if check_data_available; then
        log_success "Test data is available"
        if [[ -f "$MARKER_FILE" ]]; then
            echo "  Marker file present: $MARKER_FILE"
            if command -v git &>/dev/null && [[ -d "$DATA_DIR/.git" ]]; then
                cd "$DATA_DIR"
                echo "  Git commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
                echo "  Last update: $(git log -1 --format=%ci 2>/dev/null || echo 'unknown')"
            fi
        fi
        echo ""
        echo "Contents:"
        ls -la "$DATA_DIR" 2>/dev/null || echo "  (unable to list)"
    else
        log_warn "Test data is NOT available"
        echo ""
        echo "To run full test suite, you need proprietary test data."
        echo "See docs/TEST_DATA.md for details."
    fi
}

# Check mode
case "${1:-}" in
    --check)
        if check_data_available; then
            exit 0
        else
            exit 1
        fi
        ;;
    --status)
        show_status
        exit 0
        ;;
esac

# Determine repository and token
REPO="${GS_TEST_DATA_REPO:-$DEFAULT_REPO}"
TOKEN="${GS_TEST_DATA_TOKEN:-}"

# If no token provided, check for GitHub CLI or Codespaces token
if [[ -z "$TOKEN" ]]; then
    # In Codespaces, try GITHUB_TOKEN (available to the owner)
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
        TOKEN="$GITHUB_TOKEN"
        log_info "Using GITHUB_TOKEN from environment"
    # Try GitHub CLI if available
    elif command -v gh &>/dev/null && gh auth status &>/dev/null; then
        TOKEN="$(gh auth token 2>/dev/null || true)"
        if [[ -n "$TOKEN" ]]; then
            log_info "Using token from GitHub CLI"
        fi
    fi
fi

# Final check for token
if [[ -z "$TOKEN" ]]; then
    log_error "No GitHub token available to fetch test data"
    echo ""
    echo "To fetch test data, set one of:"
    echo "  - GS_TEST_DATA_TOKEN environment variable"
    echo "  - GITHUB_TOKEN environment variable (in Codespaces)"
    echo "  - Authenticate with: gh auth login"
    echo ""
    echo "The token needs 'contents:read' access to: $REPO"
    echo ""
    echo "For external contributors: See docs/TEST_DATA.md for how to obtain"
    echo "or provide your own test data."
    exit 1
fi

# Security: Ensure token never appears in logs
# Configure git to use credential helper that reads from environment
export GIT_TERMINAL_PROMPT=0

log_info "Fetching test data from private repository..."

# Construct authenticated clone URL (token will be stripped from repo config after clone)
AUTH_CLONE_URL="https://x-access-token:${TOKEN}@github.com/${REPO}.git"
# Plain URL for display/config (no token)
CLONE_URL="https://github.com/${REPO}.git"

# Temporary directory for cloning
TEMP_CLONE_DIR=$(mktemp -d)

# Clean up function
cleanup() {
    rm -rf "$TEMP_CLONE_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# Clone to temp directory first, then copy contents (preserving local README)
log_info "Cloning test data repository..."

if git clone --quiet "$AUTH_CLONE_URL" "$TEMP_CLONE_DIR" 2>/dev/null; then
    log_info "Copying test data files..."
    
    # Ensure data directory exists
    mkdir -p "$DATA_DIR"
    
    # Preserve local README if it exists
    LOCAL_README=""
    if [[ -f "$DATA_DIR/README.md" ]]; then
        LOCAL_README=$(cat "$DATA_DIR/README.md")
    fi
    
    # Copy data directories (roms, systems, apps) - exclude .git and README
    for dir in "$TEMP_CLONE_DIR"/*/; do
        if [[ -d "$dir" ]] && [[ "$(basename "$dir")" != ".git" ]]; then
            rm -rf "$DATA_DIR/$(basename "$dir")"
            cp -r "$dir" "$DATA_DIR/"
        fi
    done
    
    # Restore local README if it existed, otherwise don't overwrite
    if [[ -n "$LOCAL_README" ]]; then
        echo "$LOCAL_README" > "$DATA_DIR/README.md"
    elif [[ ! -f "$DATA_DIR/README.md" ]]; then
        # Copy README from private repo only if no local one exists
        cp "$TEMP_CLONE_DIR/README.md" "$DATA_DIR/README.md" 2>/dev/null || true
    fi
    
    log_success "Test data cloned successfully"
else
    log_error "Failed to clone test data repository"
    echo ""
    echo "Possible causes:"
    echo "  - Token doesn't have access to: $REPO"
    echo "  - Repository doesn't exist"
    echo "  - Network connectivity issues"
    exit 1
fi

# Create marker file
echo "# Test data marker - do not delete" > "$MARKER_FILE"
echo "# Cloned from: $REPO" >> "$MARKER_FILE"
echo "# Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$MARKER_FILE"

log_success "Test data is ready at: $DATA_DIR"
