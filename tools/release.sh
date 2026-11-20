#!/usr/bin/env bash
# Build and sign a Sonar release (docs/updates.md); with --publish, put it on GitHub.
#
#   tools/release.sh              build dist/vX.Y.Z/: the kernel, the signed manifest, SHA256SUMS
#   tools/release.sh --publish    also create the GitHub release vX.Y.Z with those files
#
# The version is OS_VERSION in kernel/include/kernel.h; bump it first.
# The signing key is $SONAR_UPDATE_SIGNING_KEY (hex; the Actions secret) or
# ~/.config/sonar/update_signing_key (tools/sonar_update.py keygen). It must
# match the public key built into the kernel (kernel/update/update_key.h),
# or devices would refuse the update.
#
# --publish needs `gh` signed in with write access to the repository
# ($SONAR_REPO, default pronzzz/sonar). It creates the tag vX.Y.Z on the
# current commit if the tag does not exist yet.
set -euo pipefail
cd "$(dirname "$0")/.."

publish=0
for a in "$@"; do
  case "$a" in
    --publish) publish=1 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

version=$(sed -n 's/^#define OS_VERSION "\(.*\)"$/\1/p' kernel/include/kernel.h)
[ -n "$version" ] || { echo "no OS_VERSION in kernel/include/kernel.h" >&2; exit 1; }
tag="v$version"
repo="${SONAR_REPO:-pronzzz/sonar}"
dist="dist/$tag"
image="sonar-$version-Image"

# The key that signs must be the key the kernel trusts.
signer=$(python3 tools/sonar_update.py pubkey)
builtin=$(sed -n 's/^#define UPDATE_PUBKEY_HEX "\(.*\)"$/\1/p' kernel/update/update_key.h)
if [ "$signer" != "$builtin" ]; then
  echo "error: the signing key ($signer) is not the key in kernel/update/update_key.h" >&2
  echo "       ($builtin); devices would reject this release." >&2
  exit 1
fi

if [ -n "$(git status --porcelain -- kernel Makefile)" ]; then
  echo "warning: kernel/ or Makefile has uncommitted changes; the release will include them" >&2
fi

echo "== building Sonar $version"
rm -rf build/release
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" BUILD=build/release build/release/Image

rm -rf "$dist"
mkdir -p "$dist"
cp build/release/Image "$dist/$image"
python3 tools/sonar_update.py manifest "$dist/$image" "$version" > "$dist/sonar-update.txt"
python3 tools/sonar_update.py verify "$dist/sonar-update.txt" "$dist/$image" \
  --header kernel/update/update_key.h
(cd "$dist" && shasum -a 256 "$image" sonar-update.txt > SHA256SUMS 2>/dev/null ||
               sha256sum "$image" sonar-update.txt > SHA256SUMS)
echo "== $dist:"
ls -l "$dist"
cat "$dist/sonar-update.txt"

[ "$publish" = 1 ] || { echo "(not published; run with --publish to create the GitHub release)"; exit 0; }

echo "== publishing $tag to github.com/$repo"
files=("$dist/$image" "$dist/sonar-update.txt" "$dist/SHA256SUMS")
if gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
  gh release upload "$tag" "${files[@]}" -R "$repo" --clobber
else
  gh release create "$tag" "${files[@]}" -R "$repo" --target "$(git rev-parse HEAD)" \
    --title "Sonar $version" --generate-notes
fi
echo "published: https://github.com/$repo/releases/tag/$tag"
