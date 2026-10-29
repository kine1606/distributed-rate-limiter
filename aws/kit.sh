#!/usr/bin/env bash
# The AWS benchmark kit (aws/README.md): the limiter on separate machines, on demand, one command each way.
#   aws/kit.sh check                  your credentials, region and vCPU quota, and what SIZE costs
#   aws/kit.sh budget <email> [usd]   a monthly budget that emails you at 50% and 100% (default 20 USD)
#   aws/kit.sh up                     create the machines (one CloudFormation stack), then `setup`
#   aws/kit.sh setup                  install and build on them; safe to run again after a failure
#   aws/kit.sh run                    the benchmark with 1, 2 and 4 limiters; results in results/<date>_aws/
#   aws/kit.sh status                 what is up, and about what it has cost
#   aws/kit.sh ssh <name>             a shell on rl-loadgen-1, rl-limiter-2, rl-redis-1, ...
#   aws/kit.sh down                   delete everything it made; always safe: run it when you are done
# env: SIZE=full|small (default full)  MAX_MINUTES=240 (every machine ends itself then)  RUNS=3  SECONDS_RUN=30
#      ZONE=<availability zone>  KIT_STATE=<dir> (default ~/.rl-thesis-bench: the key, addresses, logs)
#      KIT_HOSTS=<file> KIT_KEY=<file>: setup and run on other machines, no AWS
#      (aws/test_local.sh does that with Docker containers)
set -euo pipefail
cd "$(dirname "$0")/.."

STACK=rl-thesis-bench
# The SSH key, the machines' addresses and the logs: in your Linux home, not the repository. Outside git,
# and on a Linux file system: on the Windows drive every file is readable by all, and ssh refuses such a key.
STATE=${KIT_STATE:-$HOME/.rl-thesis-bench}
mkdir -p "$STATE/logs" && chmod 700 "$STATE"
HOSTS=${KIT_HOSTS:-$STATE/hosts}  # a machine a line: name, SSH address (host or host:port), private address
KEY=${KIT_KEY:-$STATE/key.pem}
KNOWN=$(dirname "$HOSTS")/known_hosts
MAX_MINUTES=${MAX_MINUTES:-240} RUNS=${RUNS:-3} SECONDS_RUN=${SECONDS_RUN:-30}
# Redis type, limiter type and count, load generator type and count; their vCPUs; about what they cost an
# hour on demand in us-east-1, public addresses included (other regions cost somewhat more).
case ${SIZE:=full} in
  full) TYPES=(c7i.large c7i.xlarge 4 c7i.2xlarge 2) VCPUS=38 USD_HOUR=1.75 ;;
  small) TYPES=(c7i.large c7i.large 2 c7i.xlarge 1) VCPUS=14 USD_HOUR=0.66 ;;
  *) echo "kit: SIZE is full or small" >&2; exit 2 ;;
esac

die() { echo "kit: $*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

# ---- the machines, from the hosts file ----

names() { awk -v p="rl-$1" 'index($1, p) == 1 {print $1}' "$HOSTS"; }  # names <role>; "" for all
field() { awk -v n="$1" -v f="$2" '$1 == n {print $f}' "$HOSTS"; }
private() { field "$1" 3; }

# ssh_to <name> [ssh options...]: sets SSH_ARGS and DEST for that machine.
ssh_to() {
  local addr
  addr=$(field "$1" 2)
  [ -n "$addr" ] || die "no machine named $1 (aws/kit.sh status)"
  SSH_PORT=22
  if [[ $addr == *:* ]]; then SSH_PORT=${addr##*:} addr=${addr%:*}; fi
  SSH_ARGS=(-i "$KEY" -o StrictHostKeyChecking=accept-new -o "UserKnownHostsFile=$KNOWN" -o ConnectTimeout=10
            -o ServerAliveInterval=30 -o LogLevel=ERROR)
  DEST=ubuntu@$addr
}
on() { local n=$1; shift; ssh_to "$n"; ssh "${SSH_ARGS[@]}" -p "$SSH_PORT" "$DEST" "$@"; }  # on <name> <command>
put() { ssh_to "$1"; scp -q "${SSH_ARGS[@]}" -P "$SSH_PORT" "$2" "$DEST:$3"; }             # put <name> <file> <path>
fetch() { ssh_to "$1"; scp -q "${SSH_ARGS[@]}" -P "$SSH_PORT" "$DEST:$2" "$3"; }           # fetch <name> <path> <file>

# run_node <name> <script>: aws/node/<script>.sh on that machine, its output in $STATE/logs/<name>.log.
run_node() {
  if on "$1" 'bash -s' < "aws/node/$2.sh" > "$STATE/logs/$1.log" 2>&1; then
    echo "  $1: $(tail -1 "$STATE/logs/$1.log")"
  else
    echo "  $1: FAILED; the end of $STATE/logs/$1.log:" >&2
    tail -15 "$STATE/logs/$1.log" | sed 's/^/    /' >&2
    return 1
  fi
}
wait_all() {
  local failed=0 pid
  for pid in "$@"; do wait "$pid" || failed=1; done
  [ "$failed" = 0 ] || die "setup failed (above); fix it, then run aws/kit.sh setup again"
}

# ---- AWS ----

need_aws() {
  command -v aws >/dev/null || die "the AWS CLI is not installed (aws/README.md, step 3)"
  aws sts get-caller-identity >/dev/null 2>&1 || die "the AWS CLI has no working credentials: aws configure (aws/README.md, step 3)"
  REGION=${AWS_REGION:-${AWS_DEFAULT_REGION:-$(aws configure get region || true)}}
  [ -n "$REGION" ] || die "no region set: aws configure set region ap-southeast-1"
}

write_hosts() {
  aws ec2 describe-instances --filters "Name=tag:aws:cloudformation:stack-name,Values=$STACK" \
    "Name=instance-state-name,Values=running" \
    --query 'Reservations[].Instances[].[Tags[?Key==`Name`]|[0].Value,PublicIpAddress,PrivateIpAddress]' \
    --output text | sort > "$HOSTS"
  sed 's/^/  /' "$HOSTS"
}

cmd_check() {
  need_aws
  local quota
  quota=$(aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A \
    --query 'Quota.Value' --output text 2>/dev/null || echo unknown)
  echo "account:  $(aws sts get-caller-identity --query Arn --output text)"
  echo "region:   $REGION"
  echo "vCPUs:    SIZE=$SIZE needs $VCPUS; your quota of running on-demand standard instances is $quota"
  echo "cost:     about \$$USD_HOUR an hour while up, nothing once down"
  if [ "$quota" != unknown ] && [ "${quota%.*}" -lt "$VCPUS" ]; then
    echo "too few vCPUs: run with SIZE=small (14 vCPUs), or ask for more (aws/README.md, step 5)" >&2
    return 1
  fi
}

cmd_budget() {
  need_aws
  local email=${1:-} usd=${2:-20} account alert
  [[ $email =~ ^[^@[:space:]]+@[^@[:space:]]+\.[^@[:space:]]+$ ]] || die "usage: aws/kit.sh budget <email> [usd]"
  [[ $usd =~ ^[0-9]+$ ]] || die "the amount is whole US dollars"
  account=$(aws sts get-caller-identity --query Account --output text)
  alert='{"Notification":{"NotificationType":"%s","ComparisonOperator":"GREATER_THAN","Threshold":%s,"ThresholdType":"PERCENTAGE"},"Subscribers":[{"SubscriptionType":"EMAIL","Address":"%s"}]}'
  # Credits not deducted: the budget counts what is used, so it warns while the free credits still pay.
  # shellcheck disable=SC2059  # the format is the constant above
  aws budgets create-budget --account-id "$account" \
    --budget "{\"BudgetName\":\"rl-thesis\",\"BudgetLimit\":{\"Amount\":\"$usd\",\"Unit\":\"USD\"},\"TimeUnit\":\"MONTHLY\",\"BudgetType\":\"COST\",\"CostTypes\":{\"IncludeCredit\":false}}" \
    --notifications-with-subscribers "[$(printf "$alert" ACTUAL 50 "$email"),$(printf "$alert" ACTUAL 100 "$email"),$(printf "$alert" FORECASTED 100 "$email")]"
  echo "budget rl-thesis: $usd USD a month; email to $email at 50% and 100% used, and when forecast to pass 100%"
}

cmd_up() {
  need_aws
  if aws cloudformation describe-stacks --stack-name "$STACK" >/dev/null 2>&1; then
    die "a stack $STACK exists already: aws/kit.sh status, or aws/kit.sh down to remove it"
  fi
  cmd_check || die "nothing created"
  local ok ip
  read -r -p "Create the $SIZE machines in $REGION, about \$$USD_HOUR an hour, each ending itself after $MAX_MINUTES min? [y/N] " ok
  [ "$ok" = y ] || die "nothing created"
  ip=$(curl -fsS https://checkip.amazonaws.com | tr -d '[:space:]') || die "cannot find your public address"
  aws ec2 delete-key-pair --key-name "$STACK" >/dev/null 2>&1 || true
  rm -f "$KEY" "$KNOWN"
  (umask 077 && aws ec2 create-key-pair --key-name "$STACK" --key-type ed25519 --query KeyMaterial --output text > "$KEY")
  say "creating the machines (2 to 4 minutes)"
  aws cloudformation create-stack --stack-name "$STACK" --template-body file://aws/stack.yaml \
    --tags "Key=Project,Value=$STACK" --parameters \
    "ParameterKey=KeyName,ParameterValue=$STACK" "ParameterKey=SshCidr,ParameterValue=$ip/32" \
    "ParameterKey=RedisType,ParameterValue=${TYPES[0]}" "ParameterKey=LimiterType,ParameterValue=${TYPES[1]}" \
    "ParameterKey=LimiterCount,ParameterValue=${TYPES[2]}" "ParameterKey=LoadgenType,ParameterValue=${TYPES[3]}" \
    "ParameterKey=LoadgenCount,ParameterValue=${TYPES[4]}" "ParameterKey=MaxMinutes,ParameterValue=$MAX_MINUTES" \
    ${ZONE:+"ParameterKey=Zone,ParameterValue=$ZONE"} >/dev/null
  if ! aws cloudformation wait stack-create-complete --stack-name "$STACK"; then
    aws cloudformation describe-stack-events --stack-name "$STACK" --output text \
      --query 'StackEvents[?ResourceStatus==`CREATE_FAILED`].[LogicalResourceId,ResourceStatusReason]' >&2
    die "creating the machines failed (above); aws/kit.sh down removes what was made"
  fi
  write_hosts
  cmd_setup
}

cmd_setup() {
  [ -s "$HOSTS" ] || die "no machines: aws/kit.sh up"
  local n pid
  local -a redis limiters loadgens builds=() jobs=() masters=()
  mapfile -t redis < <(names redis-)
  mapfile -t limiters < <(names limiter-)
  mapfile -t loadgens < <(names loadgen-)
  [ "${#redis[@]}" -ge 3 ] && [ "${#limiters[@]}" -ge 1 ] && [ "${#loadgens[@]}" -ge 1 ] ||
    die "$HOSTS needs 3 rl-redis-*, an rl-limiter-* and an rl-loadgen-*"

  say "waiting for SSH on every machine"
  for n in $(names ""); do
    for _ in $(seq 60); do on "$n" true 2>/dev/null && break; sleep 5; done
    on "$n" 'cloud-init status --wait >/dev/null 2>&1; echo "DPkg::Lock::Timeout \"600\";" | sudo tee /etc/apt/apt.conf.d/90-wait-for-lock >/dev/null' ||
      die "$n does not answer SSH"
  done

  say "packing the repository at $(git describe --always --dirty) (tracked files, uncommitted edits included)"
  git ls-files -z -- . ':!docs' ':!dashboard' | tar --null -czf "$STATE/repo.tgz" -T -
  put "${loadgens[0]}" "$STATE/repo.tgz" repo.tgz

  say "building on ${loadgens[0]} (10 to 20 minutes), setting up Redis and the libraries on the others"
  run_node "${loadgens[0]}" build & builds+=($!)
  for n in "${redis[@]}"; do run_node "$n" redis & jobs+=($!); done
  for n in "${limiters[@]}" "${loadgens[@]:1}"; do run_node "$n" runtime & jobs+=($!); done
  wait_all "${builds[@]}" "${jobs[@]}"

  say "handing out the build"
  fetch "${loadgens[0]}" bench.tgz "$STATE/bench.tgz"
  jobs=()
  for n in "${limiters[@]}" "${loadgens[@]:1}"; do
    { put "$n" "$STATE/bench.tgz" bench.tgz && run_node "$n" runtime; } & jobs+=($!)
  done
  wait_all "${jobs[@]}"

  say "forming the Redis Cluster"
  for n in "${redis[@]}"; do masters+=("$(private "$n"):6379"); done
  on "${loadgens[0]}" "redis-cli --cluster create ${masters[*]} --cluster-replicas 0 --cluster-yes >/dev/null &&
    sleep 2 && redis-cli -h $(private "${redis[0]}") cluster info | grep cluster_state" || die "the Redis Cluster did not form"
  say "ready: aws/kit.sh run"
}

# One cell of the benchmark: a fresh cluster, N limiters, every load generator on every limiter at once.
cell() {
  local r=$1 n=$2 rule=$3 file keys prefix i g
  local -a targets=() pids=()
  file=$OUT/r${r}_n${n}_${rule}.jsonl
  case $rule in customer) keys=10000 prefix=cust:pay: ;; *) keys=1000000 prefix=cust: ;; esac
  # FLUSHALL on every master, refused on one that saves its data (CLAUDE.md: throwaway Redis only).
  on "$LEAD" "for m in ${REDIS_IPS[*]}; do
      [ -z \"\$(redis-cli -h \$m config get save | sed -n 2p)\" ] || { echo \"\$m saves its data: not flushed\" >&2; exit 3; }
      redis-cli -h \$m flushall >/dev/null; done
    REDIS_HOST=${REDIS_IPS[0]} REDIS_PORT=6379 rl/tools/seed_plans.sh 10000 >/dev/null" || die "resetting Redis failed"
  for ((i = 0; i < n; i++)); do
    targets+=("$(private "${LIMITERS[i]}")")
    # pkill matches the command line the server starts with, not this shell's, which contains the name too
    on "${LIMITERS[i]}" "cd rl && { pkill -f '^build/release/rate_limiter_server' || true; } && sleep 0.5 &&
      (RL_METRICS_ADDR=off nohup build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml \
       ${REDIS_IPS[0]}:6379 lua leased > limiter.log 2>&1 &)"
  done
  for i in "${targets[@]}"; do
    on "$LEAD" "timeout 30 bash -c 'until (exec 3<>/dev/tcp/$i/50051) 2>/dev/null; do sleep 0.2; done'" ||
      die "the limiter on $i did not start (aws/kit.sh ssh to it: ~/rl/limiter.log)"
  done
  : > "$file"
  for g in "${LOADGENS[@]}"; do
    for i in "${targets[@]}"; do
      on "$g" "rl/build/release/bench/loadgen $i:50051 $rule $SECONDS_RUN 4 32 $keys 5 1 0 $prefix zipf" >> "$file" &
      pids+=($!)
    done
  done
  for i in "${pids[@]}"; do wait "$i" || true; done
  for ((i = 0; i < n; i++)); do on "${LIMITERS[i]}" "pkill -f '^build/release/rate_limiter_server' || true"; done
  python3 - "$file" "$r" "$n" "$rule" <<'EOF'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1]) if line.startswith("{")]
print(f"  run {sys.argv[2]}, {sys.argv[3]} limiter(s), {sys.argv[4]}: {sum(r['checks_per_s'] for r in rows):,.0f} checks/s, "
      f"p99 {max((r['p99_ms'] for r in rows), default=0):.2f} ms, errors {sum(r['errors'] for r in rows)}, "
      f"{len(rows)} load generator processes")
EOF
}

cmd_run() {
  [ -s "$HOSTS" ] || die "no machines: aws/kit.sh up"
  local r n rule
  local -a counts=() redis
  mapfile -t redis < <(names redis-)
  mapfile -t LIMITERS < <(names limiter-)
  mapfile -t LOADGENS < <(names loadgen-)
  LEAD=${LOADGENS[0]}
  REDIS_IPS=()
  for n in "${redis[@]}"; do REDIS_IPS+=("$(private "$n")"); done
  for n in 1 2 4; do [ "$n" -le "${#LIMITERS[@]}" ] && counts+=("$n"); done
  OUT=results/$(date +%F)_aws${KIT_HOSTS:+_local}  # a test on other machines is never taken for the AWS run
  mkdir -p "$OUT"
  {
    echo "# aws/kit.sh run, $(date -u +%FT%TZ), repo $(git describe --always --dirty), RUNS=$RUNS SECONDS_RUN=$SECONDS_RUN"
    echo "# ${#redis[@]} Redis masters, ${#LIMITERS[@]} limiters, ${#LOADGENS[@]} load generators (4 x 32 in flight per limiter each)"
    if [ -z "${KIT_HOSTS:-}" ] && command -v aws >/dev/null; then
      aws ec2 describe-instances --filters "Name=tag:aws:cloudformation:stack-name,Values=$STACK" \
        "Name=instance-state-name,Values=running" --output text \
        --query 'Reservations[].Instances[].[Tags[?Key==`Name`]|[0].Value,InstanceType,Placement.AvailabilityZone]' | sort | sed 's/^/# /'
    fi
  } > "$OUT/meta.txt"
  say "the benchmark: ${counts[*]} limiter(s) x customer and api_default x $RUNS runs, $SECONDS_RUN s each"
  for r in $(seq "$RUNS"); do
    for n in "${counts[@]}"; do
      for rule in customer api_default; do cell "$r" "$n" "$rule"; done
    done
  done
  python3 - "$OUT" <<'EOF'
import collections, json, pathlib, re, statistics, sys
out = pathlib.Path(sys.argv[1])
cells = collections.defaultdict(list)  # (rule, limiters) -> [(checks/s, p99), ...] one per run
for f in sorted(out.glob("r*_n*_*.jsonl")):
    _, n, rule = re.match(r"r(\d+)_n(\d+)_(.+)\.jsonl", f.name).groups()
    rows = [json.loads(line) for line in f.open() if line.startswith("{")]
    if rows:
        cells[(rule, int(n))].append((sum(r["checks_per_s"] for r in rows), max(r["p99_ms"] for r in rows)))
lines = ["| Rule | Limiters | Checks/s, median (range) | vs 1 limiter | p99 ms, median |", "| --- | --- | --- | --- | --- |"]
for (rule, n), runs in sorted(cells.items()):
    rate = [c for c, _ in runs]
    base = statistics.median(c for c, _ in cells[(rule, 1)]) if (rule, 1) in cells else None
    ratio = f"{statistics.median(rate) / base:.2f}x" if base else "-"
    lines.append(f"| {rule} | {n} | {statistics.median(rate):,.0f} ({min(rate):,.0f} to {max(rate):,.0f}) | {ratio} | "
                 f"{statistics.median(p for _, p in runs):.2f} |")
(out / "summary.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
EOF
  say "results in $OUT (summary.md, meta.txt, one file a cell); when done: aws/kit.sh down"
}

cmd_status() {
  need_aws
  local stack first
  stack=$(aws cloudformation describe-stacks --stack-name "$STACK" --query 'Stacks[0].StackStatus' --output text 2>/dev/null) ||
    { echo "nothing is up (no stack $STACK)"; return 0; }
  echo "stack $STACK: $stack"
  aws ec2 describe-instances --filters "Name=tag:aws:cloudformation:stack-name,Values=$STACK" --output table \
    --query 'Reservations[].Instances[].[Tags[?Key==`Name`]|[0].Value,InstanceType,State.Name,PublicIpAddress,LaunchTime]'
  first=$(aws ec2 describe-instances --filters "Name=tag:aws:cloudformation:stack-name,Values=$STACK" \
    "Name=instance-state-name,Values=running" --query 'sort(Reservations[].Instances[].LaunchTime)[0]' --output text)
  if [ "$first" != None ]; then
    python3 -c "import datetime as d, sys; h = (d.datetime.now(d.timezone.utc) - d.datetime.fromisoformat(sys.argv[1])).total_seconds() / 3600
print(f'up {h:.1f} h: about \${h * float(sys.argv[2]):.2f} so far at \${sys.argv[2]} an hour (SIZE=$SIZE)')" "$first" "$USD_HOUR"
  fi
}

cmd_ssh() { ssh_to "$1"; exec ssh -t "${SSH_ARGS[@]}" -p "$SSH_PORT" "$DEST"; }

cmd_down() {
  need_aws
  local left
  say "deleting every machine, disk and the network (2 to 4 minutes)"
  aws cloudformation delete-stack --stack-name "$STACK"
  aws cloudformation wait stack-delete-complete --stack-name "$STACK" ||
    die "the deletion did not finish: see CloudFormation in the console (aws/README.md, step 8), then run down again"
  aws ec2 delete-key-pair --key-name "$STACK" >/dev/null 2>&1 || true
  rm -f "$KEY" "$HOSTS" "$KNOWN" "$STATE/repo.tgz" "$STATE/bench.tgz"
  left=$(aws ec2 describe-instances --filters "Name=tag:Project,Values=$STACK" \
    "Name=instance-state-name,Values=pending,running,stopping,stopped" --query 'length(Reservations[].Instances[])' --output text)
  [ "$left" = 0 ] || die "$left machines of $STACK still exist: see EC2 in the console"
  say "everything is deleted; nothing more is charged"
}

case ${1:-} in
  check) cmd_check ;;
  budget) shift; cmd_budget "$@" ;;
  up) cmd_up ;;
  setup) cmd_setup ;;
  run) cmd_run ;;
  status) cmd_status ;;
  ssh) [ -n "${2:-}" ] || die "usage: aws/kit.sh ssh <name>"; cmd_ssh "$2" ;;
  down) cmd_down ;;
  *) sed -n '2,14p' "$0" >&2; exit 2 ;;
esac
