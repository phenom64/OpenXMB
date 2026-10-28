#!/usr/bin/env bash
set -euo pipefail
archive=$(find dist/portable-input -name '*-linux-x86_64.tar.gz' -print -quit)
test -n "$archive"
mkdir -p "dist/portable-evidence-$DISTRO"
# Permit nested browser user namespaces only inside this test container.
# Ubuntu restricts them even for the default unconfined profile.
apparmor_profile=unconfined
if sudo aa-enabled --quiet; then
  printf 'abi <abi/4.0>,\nprofile openxmb-portable-ci-container flags=(unconfined) {\n  userns,\n}\n' \
    > "$RUNNER_TEMP/openxmb-portable-container.profile"
  sudo apparmor_parser -r "$RUNNER_TEMP/openxmb-portable-container.profile"
  apparmor_profile=openxmb-portable-ci-container
fi
docker run --rm --security-opt seccomp=unconfined --security-opt "apparmor=$apparmor_profile" \
  -e OPENXMB_PORTABLE_CI=1 -v "$PWD:/src:ro" \
  -v "$PWD/dist/portable-evidence-$DISTRO:/evidence" "$IMAGE" \
  bash /src/scripts/ci/check-linux-portable.sh "$DISTRO" "/src/$archive" /evidence \
  2>&1 | tee "dist/portable-evidence-$DISTRO/container.log"
