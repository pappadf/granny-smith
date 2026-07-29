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
#   ./scripts/fetch-test-data.sh           # Fetch the PINNED data revision
#   ./scripts/fetch-test-data.sh --update  # Fetch latest and move the pin
#   ./scripts/fetch-test-data.sh --check   # 0 = ok, 1 = absent, 2 = wrong revision
#   ./scripts/fetch-test-data.sh --status  # Show detailed status
#
# Data revision pinning:
#   Goldens are byte-exact, so a test is only meaningful against the media it
#   was captured from. tests/data is a copy of the gs-test-data repo, so that
#   repo's COMMIT HASH is the data's identity — no separate manifest needed.
#   tests/data-version records the revision this checkout's tests were tuned
#   against; a plain fetch reproduces exactly that. Adopting newer data is a
#   deliberate act: --update moves the pin, and the diff shows up in review
#   next to whatever goldens had to be recaptured.
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
PIN_FILE="$REPO_ROOT/tests/data-version"

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

# The pinned gs-test-data revision, or empty if this checkout does not pin one.
# Comments and blank lines are ignored so the file can explain itself.
read_pin() {
    [[ -f "$PIN_FILE" ]] || return 0
    grep -vE '^[[:space:]]*(#|$)' "$PIN_FILE" | head -1 | tr -d '[:space:]'
}

# The revision actually present in tests/data, recorded by the last fetch.
# Empty for data fetched before pinning existed, or supplied by hand.
read_marker_commit() {
    [[ -f "$MARKER_FILE" ]] || return 0
    sed -n 's/^# Commit: //p' "$MARKER_FILE" | head -1 | tr -d '[:space:]'
}

# Check if test data is available
check_data_available() {
    if [[ -f "$MARKER_FILE" ]]; then
        return 0
    fi
    # Also check for key files as fallback
    if [[ -f "$DATA_DIR/roms/plus-v3-4d1f8172.rom" ]] && [[ -d "$DATA_DIR/systems" ]]; then
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
        pin="$(read_pin)"
        have="$(read_marker_commit)"
        echo "  Pinned revision:  ${pin:-<none recorded>}"
        echo "  Present revision: ${have:-<unknown>}"
        if [[ -n "$pin" && -n "$have" && "$pin" != "$have" ]]; then
            log_warn "  MISMATCH — goldens may not correspond to this media"
        fi
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
        echo "See docs/guide/TEST_DATA.md for details."
    fi
}

# Check mode
UPDATE_PIN=0
case "${1:-}" in
    --check)
        if ! check_data_available; then
            exit 1
        fi
        pin="$(read_pin)"
        have="$(read_marker_commit)"
        # No pin, or data predating the marker's Commit line: nothing to verify.
        # Silence here is deliberate — an external contributor using their own
        # media must not be told their tree is broken.
        if [[ -z "$pin" || -z "$have" ]]; then
            exit 0
        fi
        if [[ "$pin" != "$have" ]]; then
            log_error "test data revision mismatch"
            echo "  pinned  (tests/data-version): $pin"
            echo "  present (tests/data)........: $have"
            echo ""
            echo "Goldens are byte-exact, so results against the wrong media are not"
            echo "meaningful. Run scripts/fetch-test-data.sh to get the pinned revision,"
            echo "or --update to adopt what is present and move the pin."
            # 2, not 1: the data IS present, it is the wrong revision. Callers
            # print very different advice for the two cases.
            exit 2
        fi
        exit 0
        ;;
    --update)
        UPDATE_PIN=1
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
    echo "For external contributors: See docs/guide/TEST_DATA.md for how to obtain"
    echo "or provide your own test data."
    exit 1
fi

# Security: Ensure token never appears in logs
# Configure git to use credential helper that reads from environment
export GIT_TERMINAL_PROMPT=0

# Suppress git-lfs smudge globally so the clone never stalls waiting for LFS objects.
if git lfs version &>/dev/null 2>&1; then
    git lfs install --skip-repo >/dev/null 2>&1 || true
fi

# Ensure 7z is available (used to extract large binary archives)
if ! command -v 7z &>/dev/null; then
    log_info "Installing p7zip-full..."
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq && sudo apt-get install -y -qq p7zip-full 2>/dev/null || true
    fi
fi

log_info "Fetching test data from private repository..."

# Plain URL with no embedded credentials. The token is passed via an
# ephemeral Authorization header (http.extraheader) so it never appears
# in process argv, /proc/<pid>/cmdline, or the cloned repo's .git/config.
CLONE_URL="https://github.com/${REPO}.git"
# Base64-encode "x-access-token:<token>" for HTTP Basic auth.
AUTH_HEADER_VALUE="$(printf 'x-access-token:%s' "$TOKEN" | base64 -w0 2>/dev/null || printf 'x-access-token:%s' "$TOKEN" | base64 | tr -d '\n')"

# Temporary directory for cloning
TEMP_CLONE_DIR=$(mktemp -d)

# Clean up function
cleanup() {
    # Scrub the auth header value from memory before exit.
    AUTH_HEADER_VALUE=""
    rm -rf "$TEMP_CLONE_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# Clone to temp directory first, then copy contents (preserving local README)
log_info "Cloning test data repository..."

# Clone with GIT_LFS_SKIP_SMUDGE=1 to skip LFS pointer resolution entirely;
# large binaries are stored as .7z archives and extracted below.
# -c http.extraheader=... keeps the token off the command line and out of config.
if GIT_LFS_SKIP_SMUDGE=1 git \
        -c "http.https://github.com/.extraheader=AUTHORIZATION: Basic ${AUTH_HEADER_VALUE}" \
        clone --quiet --depth 1 "$CLONE_URL" "$TEMP_CLONE_DIR" 2>/dev/null; then

    # Reproduce the PINNED revision rather than whatever HEAD happens to be,
    # unless --update was asked for. The clone above is --depth 1 on the default
    # branch; GitHub serves a fetch by explicit sha, so one extra shallow fetch
    # is all a rewind costs.
    PIN="$(read_pin)"
    if [[ "$UPDATE_PIN" -eq 0 && -n "$PIN" ]]; then
        if [[ "$(git -C "$TEMP_CLONE_DIR" rev-parse HEAD)" != "$PIN" ]]; then
            log_info "Rewinding to pinned revision ${PIN:0:12}..."
            if GIT_LFS_SKIP_SMUDGE=1 git -C "$TEMP_CLONE_DIR" \
                    -c "http.https://github.com/.extraheader=AUTHORIZATION: Basic ${AUTH_HEADER_VALUE}" \
                    fetch --quiet --depth 1 origin "$PIN" 2>/dev/null \
               && git -C "$TEMP_CLONE_DIR" checkout --quiet FETCH_HEAD 2>/dev/null; then
                :
            else
                log_error "Could not fetch pinned revision $PIN from $REPO"
                echo ""
                echo "The pin in tests/data-version names a revision this token cannot"
                echo "reach — it may have been force-pushed away, or the pin is wrong."
                echo "Use --update to adopt current HEAD and move the pin deliberately."
                exit 1
            fi
        fi
    fi
    DATA_COMMIT="$(git -C "$TEMP_CLONE_DIR" rev-parse HEAD)"

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

    # Extract any .7z archives in the data directory (large binaries not stored in LFS).
    # Each archive produces a file with the same base name in the same directory.
    if command -v 7z &>/dev/null; then
        while IFS= read -r -d '' archive; do
            dir="$(dirname "$archive")"
            log_info "Extracting $(basename "$archive")..."
            7z e -y -o"$dir" "$archive" >/dev/null
        done < <(find "$DATA_DIR" -name '*.7z' -print0)
    else
        log_warn "7z not available; .7z archives will not be extracted (tests requiring them will fail)"
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

# Create marker file. The Commit line is what makes this data identifiable:
# tests/data has no .git of its own, so without it the copy is anonymous.
echo "# Test data marker - do not delete" > "$MARKER_FILE"
echo "# Cloned from: $REPO" >> "$MARKER_FILE"
echo "# Commit: $DATA_COMMIT" >> "$MARKER_FILE"
echo "# Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$MARKER_FILE"

# --update is the deliberate act of adopting newer media: move the pin so the
# change is a reviewable diff in the main repo, next to any goldens it forced
# to be recaptured.
if [[ "$UPDATE_PIN" -eq 1 ]]; then
    {
        echo "# gs-test-data revision this checkout's tests are tuned against."
        echo "# Goldens are byte-exact, so changing this line means re-verifying"
        echo "# every row that consumes the media it changes."
        echo "# Update with: scripts/fetch-test-data.sh --update"
        echo "$DATA_COMMIT"
    } > "$PIN_FILE"
    log_success "Pinned tests/data-version to ${DATA_COMMIT:0:12}"
fi

log_success "Test data is ready at: $DATA_DIR (revision ${DATA_COMMIT:0:12})"
