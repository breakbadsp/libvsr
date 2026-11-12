#!/usr/bin/env bash
# Set up a GitHub Actions runner for this repository on an Arch Linux host.
# The runner runs as its own unprivileged user, gha-runner, and gets
# containers from rootless Podman through a `docker` command. No job gets root.
#
# Run as root, with a registration token on standard input:
#   gh api -X POST repos/breakbadsp/libvsr/actions/runners/registration-token \
#     --jq .token | sudo bash .github/scripts/self-hosted-runner-setup.sh
# Use RUNNER_COUNT to override the default: one slot on this shared host.
# A registration token is needed only when a slot is not registered.
# Once all slots are registered, rerun it with no token to update the host:
#   sudo bash .github/scripts/self-hosted-runner-setup.sh < /dev/null
# See docs/verification.md.
set -euo pipefail

repo_url=https://github.com/breakbadsp/libvsr
runner_user=gha-runner
runner_version=2.338.0
runner_sha256=af4b794c1bc41d73d40535e3fe092a39f9679cd8d965954c2aca25a05ca41d32

[[ $EUID -eq 0 ]] || { echo "Run this script as root." >&2; exit 1; }
runner_count=${RUNNER_COUNT:-1}
[[ $runner_count =~ ^[1-9][0-9]*$ ]] || { echo "RUNNER_COUNT must be a positive integer." >&2; exit 1; }
# Do not let two setup processes change the same registrations and services.
exec 9>/run/lock/libvsr-runner-setup.lock
flock -n 9 || { echo "Runner setup is already running." >&2; exit 1; }
token=
read -r token || true

# The host already serves triangulator. Reuse its account and rootless
# container tools without upgrading packages or changing its services.
for tool in podman python3 curl tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing host tool: $tool" >&2; exit 1; }
done

if ! id "$runner_user" >/dev/null 2>&1; then
    useradd --create-home --shell /bin/bash "$runner_user"
fi
# Rootless Podman maps container users to these subordinate IDs.
grep -q "^$runner_user:" /etc/subuid || usermod --add-subuids 200000-265535 "$runner_user"
grep -q "^$runner_user:" /etc/subgid || usermod --add-subgids 200000-265535 "$runner_user"
# Keep the user's systemd instance, and so the runner, running without a login.
loginctl enable-linger "$runner_user"

runner_home=$(getent passwd "$runner_user" | cut -d: -f6)
uid=$(id -u "$runner_user")
as_runner() {
    runuser -u "$runner_user" -- env XDG_RUNTIME_DIR="/run/user/$uid" "$@"
}

as_runner bash -s <<'EOF'
set -euo pipefail
cd "$HOME"

# Jobs call `docker`; rootless Podman answers. Bare names like almalinux:9
# resolve to Docker Hub, as they do for Docker.
mkdir -p bin .config/containers
if [[ ! -x bin/docker ]]; then
printf '#!/bin/sh\nexec podman "$@"\n' > bin/docker
chmod 755 bin/docker
fi
if [[ ! -f .config/containers/registries.conf ]]; then
cat > .config/containers/registries.conf <<'CONF'
unqualified-search-registries = ["docker.io"]
short-name-mode = "permissive"
CONF
fi
EOF

for ((slot = 1; slot <= runner_count; ++slot)); do
    # Keep the original registration, work directory, and service for slot 1.
    runner_dir=$runner_home/actions-runner-libvsr
    runner_name=$(hostnamectl hostname)-libvsr
    service=actions-runner-libvsr.service
    if (( slot > 1 )); then
        runner_dir+=-$slot
        runner_name+=-$slot
        service=actions-runner-libvsr-$slot.service
    fi
    if [[ ! -f "$runner_dir/.runner" && -z "$token" ]]; then
        echo "Pass a registration token on standard input to register slot $slot." >&2
        exit 1
    fi
    as_runner bash -s "$runner_dir" "$runner_version" "$runner_sha256" <<'EOF'
set -euo pipefail
runner_dir=$1
version=$2
sha256=$3
if [[ ! -x "$runner_dir/run.sh" ]]; then
    # Download once. Each slot needs its own runner installation and _work.
    mkdir -p "$HOME/.cache/actions-runner"
    tarball=$HOME/.cache/actions-runner/actions-runner-linux-x64-$version.tar.gz
    if [[ ! -f "$tarball" ]]; then
        curl -fsSLo "$tarball.tmp" "https://github.com/actions/runner/releases/download/v$version/${tarball##*/}"
        echo "$sha256  $tarball.tmp" | sha256sum -c -
        mv "$tarball.tmp" "$tarball"
    fi
    echo "$sha256  $tarball" | sha256sum -c -
    mkdir -p "$runner_dir"
    tar -xzf "$tarball" -C "$runner_dir"
fi
EOF
    if [[ ! -f "$runner_dir/.runner" ]]; then
        as_runner bash -c 'cd "$1"; shift; exec ./config.sh "$@"' bash "$runner_dir" --unattended \
            --url "$repo_url" --token "$token" \
            --name "$runner_name" --labels libvsr \
            --work _work
    fi
    as_runner mkdir -p "$runner_home/.config/systemd/user"
    # Write as the runner user: jobs control this directory, and root would
    # follow a symlink a job put there.
    as_runner tee "$runner_home/.config/systemd/user/$service" >/dev/null <<EOF
[Unit]
Description=GitHub Actions runner for libvsr (slot $slot)
After=network-online.target

[Service]
WorkingDirectory=$runner_dir
# Jobs inherit this PATH, so \`docker\` finds the Podman wrapper first.
Environment=PATH=$runner_home/bin:/usr/local/bin:/usr/bin
# Bound builds that do not pass -j, such as scripts/start.sh in host tests.
Environment=MAKEFLAGS=-j2
# Make run.sh pass SIGINT on to the runner, which then cancels any running
# job and exits. Whatever is left after a minute is killed.
Environment=RUNNER_MANUALLY_TRAP_SIG=1
ExecStart=$runner_dir/run.sh
Restart=always
RestartSec=10
KillMode=mixed
KillSignal=SIGINT
TimeoutStopSec=1min

[Install]
WantedBy=default.target
EOF
    systemctl --user -M "$runner_user@" daemon-reload
    systemctl --user -M "$runner_user@" enable "$service"
    # Starting an active service preserves the job it is running. Updated
    # service settings take effect after its next restart.
    systemctl --user -M "$runner_user@" start "$service"
    systemctl --user -M "$runner_user@" --no-pager status "$service"
done
