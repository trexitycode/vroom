#!/usr/bin/env bash

set -euo pipefail

# Move to repo root based on this script's location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION_FILE="src/utils/version.h"

log() { echo "[trexity-publish] $*"; }
fail() { echo "[trexity-publish] ERROR: $*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || fail "git is required"
command -v sed >/dev/null 2>&1 || fail "sed is required"
command -v date >/dev/null 2>&1 || fail "date is required"

[[ -f "$VERSION_FILE" ]] || fail "Cannot find $VERSION_FILE"

# Generate timestamp in format YYYYMMDDHHII (minutes)
TIMESTAMP="$(date +%Y%m%d%H%M)"

# Detect BSD vs GNU sed for in-place editing
if sed --version >/dev/null 2>&1; then
  # GNU sed
  SED_INPLACE=( -i -E )
else
  # BSD/macOS sed
  SED_INPLACE=( -i '' -E )
fi

# Extract semantic version from version.h
extract_number() {
  local name="$1"
  sed -nE "s/^constexpr[[:space:]]+unsigned[[:space:]]+${name}[[:space:]]*=[[:space:]]*([0-9]+);$/\\1/p" "$VERSION_FILE"
}

MAJOR="$(extract_number MAJOR)"; [[ -n "$MAJOR" ]] || fail "Could not read MAJOR from $VERSION_FILE"
MINOR="$(extract_number MINOR)"; [[ -n "$MINOR" ]] || fail "Could not read MINOR from $VERSION_FILE"
PATCH="$(extract_number PATCH)"; [[ -n "$PATCH" ]] || fail "Could not read PATCH from $VERSION_FILE"

VERSION="${MAJOR}.${MINOR}.${PATCH}"
TAG="trexity-${VERSION}-${TIMESTAMP}"

log "Version: $VERSION"
log "Timestamp: $TIMESTAMP"
log "Tag: $TAG"

# Update TREXITY_EDITION value
log "Updating TREXITY_EDITION in $VERSION_FILE"
sed "${SED_INPLACE[@]}" \
  "s/^(constexpr[[:space:]]+std::string_view[[:space:]]+TREXITY_EDITION[[:space:]]*=[[:space:]]*\")[0-9]{12}(\";)/\\1${TIMESTAMP}\\2/" \
  "$VERSION_FILE"

# Verify update
if ! grep -q "TREXITY_EDITION = \"${TIMESTAMP}\"" "$VERSION_FILE"; then
  fail "Failed to update TREXITY_EDITION in $VERSION_FILE"
fi

# Commit the change
log "Committing change to $VERSION_FILE"
git add "$VERSION_FILE"
git commit -m "trexity: bump edition to ${TIMESTAMP} (v${VERSION})" >/dev/null 2>&1 || {
  # If commit had nothing to commit (unlikely), surface useful info
  git status --porcelain
  fail "git commit failed"
}

# Create annotated tag
if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
  fail "Tag ${TAG} already exists"
fi

log "Creating tag ${TAG}"
git tag -a "$TAG" -m "Trexity edition ${TIMESTAMP} for v${VERSION}"

# Push commit and tag
DEFAULT_REMOTE="origin"
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"

log "Pushing commit to ${DEFAULT_REMOTE} ${CURRENT_BRANCH}"
git push "$DEFAULT_REMOTE" "$CURRENT_BRANCH"

log "Pushing tag ${TAG} to ${DEFAULT_REMOTE}"
git push "$DEFAULT_REMOTE" "$TAG"

log "Done. Published ${TAG}"
