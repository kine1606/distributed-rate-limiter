#!/usr/bin/env bash
# Tests aws/kit.sh setup and run without AWS: six Ubuntu 24.04 containers with SSH stand in for the
# machines (3 Redis, 2 limiters, 1 load generator), on ports 2201 to 2206 of 127.0.0.1. About 20 minutes,
# most of it the one build. Removes the containers at the end.
#   aws/test_local.sh     env: KEEP=1 keeps the containers (then: KIT_HOSTS=~/.rl-thesis-bench/local/hosts
#                              KIT_KEY=~/.rl-thesis-bench/local/key aws/kit.sh ssh rl-loadgen-1)
set -euo pipefail
cd "$(dirname "$0")/.."
T=${KIT_STATE:-$HOME/.rl-thesis-bench}/local  # a Linux file system: ssh refuses a key the Windows drive exposes
mkdir -p "$T" && chmod 700 "$T"
[ -f "$T/key" ] || ssh-keygen -q -t ed25519 -N "" -f "$T/key"
rm -f "$T/known_hosts"  # new containers, new host keys

docker build -q -t rl-kit-node - >/dev/null <<EOF
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openssh-server sudo >/dev/null \\
 && (id ubuntu >/dev/null 2>&1 || useradd -m -s /bin/bash ubuntu) \\
 && echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ubuntu && mkdir -p /run/sshd /home/ubuntu/.ssh \\
 && echo '$(cat "$T/key.pub")' > /home/ubuntu/.ssh/authorized_keys \\
 && chown -R ubuntu:ubuntu /home/ubuntu/.ssh && chmod 600 /home/ubuntu/.ssh/authorized_keys
CMD ["/usr/sbin/sshd", "-D"]
EOF

names=(rl-redis-1 rl-redis-2 rl-redis-3 rl-limiter-1 rl-limiter-2 rl-loadgen-1)
cleanup() {
  [ "${KEEP:-}" = 1 ] && return
  docker rm -f "${names[@]}" >/dev/null 2>&1 || true
  docker network rm rl-kit-test >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker network create rl-kit-test >/dev/null 2>&1 || true
: > "$T/hosts"
port=2201
for n in "${names[@]}"; do
  docker rm -f "$n" >/dev/null 2>&1 || true
  docker run -d --name "$n" --hostname "$n" --network rl-kit-test -p "127.0.0.1:$port:22" rl-kit-node >/dev/null
  echo "$n 127.0.0.1:$port $(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$n")" >> "$T/hosts"
  port=$((port + 1))
done

export KIT_HOSTS=$T/hosts KIT_KEY=$T/key
aws/kit.sh setup
RUNS=${RUNS:-1} SECONDS_RUN=${SECONDS_RUN:-5} aws/kit.sh run
