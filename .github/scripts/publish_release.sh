#!/usr/bin/env bash
# Called only after all main-branch checks. GH_TOKEN is the ephemeral Actions token.
set -euo pipefail
: "${GH_REPO:?}" "${GITHUB_SHA:?}" "${GITHUB_RUN_ID:?}"
MAIN_SHA=$(gh api "repos/$GH_REPO/git/ref/heads/main" --jq '.object.sha')
if [[ "$MAIN_SHA" != "$GITHUB_SHA" ]]; then
    echo "Superseded main build; no release is published."
    exit 0
fi
gh run download "$GITHUB_RUN_ID" --repo "$GH_REPO" --name QuantPriceLive-Windows-x64 --dir release-bundle
python .github/scripts/package_release.py --bundle release-bundle --output release-output --commit "$GITHUB_SHA"
VERSION=$(python -c 'import json; print(json.load(open("release-output/release-info.json"))["version"])')
TAG="v$VERSION"
ASSET="QuantPriceLive-$VERSION-Windows-x64.zip"
CHECKSUM="$ASSET.sha256"
if TAG_SHA=$(gh api "repos/$GH_REPO/commits/$TAG" --jq '.sha' 2>/dev/null); then
    if [[ "$TAG_SHA" != "$GITHUB_SHA" ]]; then
        echo "Version $TAG already names another commit. Bump the application/package version; no overwrite."
        exit 1
    fi
fi
if ! gh release view "$TAG" --repo "$GH_REPO" --json isDraft,targetCommitish,assets > release-output/existing.json 2>/dev/null; then
    gh release create "$TAG" --repo "$GH_REPO" --draft --target "$GITHUB_SHA" \
        --title "Quant Price Live $VERSION — учебная исследовательская платформа" \
        --notes-file RELEASE_NOTES_RU.md
    gh release view "$TAG" --repo "$GH_REPO" --json isDraft,targetCommitish,assets > release-output/existing.json
fi
IS_DRAFT=$(python -c 'import json; print(str(json.load(open("release-output/existing.json"))["isDraft"]).lower())')
TARGET=$(python -c 'import json; print(json.load(open("release-output/existing.json"))["targetCommitish"])')
if [[ "$TARGET" != "$GITHUB_SHA" ]]; then
    echo "Release target differs from the tested commit; refusing to modify it."
    exit 1
fi
has_asset() {
    gh release view "$TAG" --repo "$GH_REPO" --json assets --jq '.assets[].name' | grep -Fxq -- "$1"
}
if ! has_asset "$ASSET"; then
    [[ "$IS_DRAFT" == true ]] || { echo "Published release is missing its ZIP; no modification."; exit 1; }
    gh release upload "$TAG" "release-output/$ASSET" --repo "$GH_REPO"
fi
mkdir -p release-verify
gh release download "$TAG" --repo "$GH_REPO" --pattern "$ASSET" --dir release-verify
python .github/scripts/package_release.py --verify-archive "release-verify/$ASSET" --commit "$GITHUB_SHA" --version "$VERSION"
# A retry may retain a previously uploaded, fully verified draft ZIP. Never replace public assets.
(cd release-verify && sha256sum "$ASSET") > "release-output/$CHECKSUM"
if ! has_asset "$CHECKSUM"; then
    [[ "$IS_DRAFT" == true ]] || { echo "Published release is missing its checksum; no modification."; exit 1; }
    gh release upload "$TAG" "release-output/$CHECKSUM" --repo "$GH_REPO"
fi
gh release download "$TAG" --repo "$GH_REPO" --pattern "$CHECKSUM" --dir release-verify
(cd release-verify && sha256sum --check "$CHECKSUM")
MAIN_SHA=$(gh api "repos/$GH_REPO/git/ref/heads/main" --jq '.object.sha')
[[ "$MAIN_SHA" == "$GITHUB_SHA" ]] || { echo "main moved while preparing the draft; publication stopped."; exit 1; }
if [[ "$IS_DRAFT" == true ]]; then
    gh release edit "$TAG" --repo "$GH_REPO" --draft=false --latest
fi
TAG_SHA=$(gh api "repos/$GH_REPO/commits/$TAG" --jq '.sha')
[[ "$TAG_SHA" == "$GITHUB_SHA" ]] || { echo "Published tag mismatch"; exit 1; }
echo "Verified release: https://github.com/$GH_REPO/releases/tag/$TAG"
