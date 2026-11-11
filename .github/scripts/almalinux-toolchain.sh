#!/usr/bin/env bash
# Run only inside the disposable AlmaLinux image.
set -euo pipefail
case ${ALMA_VERSION:?set ALMA_VERSION} in
  8) development_repo=powertools ;;
  9) development_repo=crb ;;
  *) echo 'ALMA_VERSION must be 8 or 9.' >&2; exit 2 ;;
esac
# Regional mirrors can fail DNS on the CI host. Use the official HTTPS
# repository from the image configuration and keep package signature checks.
sed -i -e 's|^mirrorlist=|#mirrorlist=|' \
  -e 's|^# *baseurl=https://repo.almalinux.org/|baseurl=https://repo.almalinux.org/|' \
  /etc/yum.repos.d/almalinux*.repo
dnf install -y dnf-plugins-core
dnf config-manager --set-enabled "$development_repo"
dnf install -y gcc-toolset-14-gcc-c++ gcc-toolset-14-libstdc++-devel \
  gcc-toolset-14-libasan-devel gcc-toolset-14-libubsan-devel \
  clang compiler-rt cmake ninja-build gtest-devel git tar gzip
dnf clean all
