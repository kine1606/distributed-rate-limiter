# Running the benchmark on AWS

The thesis measured everything on one laptop, so scaling across instances is an estimate (Chapter 5.11,
6.2). This kit measures it on separate machines: a Redis Cluster of 3 masters, 1, 2 and then 4 limiter
instances, and load generators, in one private network. One command creates the machines, one runs the
benchmark, and one deletes everything.

| Command | Does | Takes |
| --- | --- | --- |
| `aws/kit.sh check` | your credentials, region, vCPU quota, and the cost | seconds |
| `aws/kit.sh budget <email> 20` | an email when you have used 50% and 100% of 20 USD in a month | seconds, once |
| `aws/kit.sh up` | creates the machines, installs and builds on them | 15 to 25 min |
| `aws/kit.sh run` | the benchmark; results in `results/<date>_aws/` | about 20 min |
| `aws/kit.sh status` | what is up, and about what it has cost | seconds |
| `aws/kit.sh down` | deletes every machine, disk, address, network and key | 2 to 4 min |

**What it costs.** `SIZE=full` (the default) is 9 machines, 38 vCPUs, about **1.75 USD an hour** while
they exist (us-east-1 prices; Singapore is a little more). One complete session (up, run, down) is about
an hour: about 2 USD of your 100 USD credit. `SIZE=small` is 6 machines, 14 vCPUs, about 0.66 USD an hour,
for an account whose vCPU quota is low.

**What keeps it from costing more:**
- *Every machine ends itself* 4 hours after it starts (`MAX_MINUTES`), even if you forget `down`.
- *`down` deletes everything the kit made* (one CloudFormation stack), and checks that no machine is left.
- *The budget emails you* before the credits run low.
- *On the Free plan, AWS never charges your card.* The account closes when the credits are used up, and
  so do its machines. Keep an eye on the credits: a closed account is gone after 90 days.

## Once: set up your account (about 20 minutes)

### Step 1. Secure the account

1. Sign in at <https://console.aws.amazon.com> with the email you signed up with: that is the **root
   user**, which can do anything, so use it only for this step.
2. Turn on MFA for it: the account name (top right) > **Security credentials** > **Assign MFA device** >
   an authenticator app on your phone.
3. Look at your credits and plan: search **Billing and Cost Management** > **Credits** (the 100 USD), and
   **Free Tier** on the same page (Free or Paid plan).
4. Pick a region (top right, next to the account name): **Asia Pacific (Singapore) ap-southeast-1** is
   the closest to Vietnam. Everything below happens in that region; the console shows one region at a
   time, so if you ever "lose" your machines, check the region first.

### Step 2. Create a user for the command line

Never create access keys for the root user. Create a user that can do only what the kit does.

1. Search **IAM** > **Policies** > **Create policy** > **JSON**. Delete what is there, paste the whole
   content of `aws/iam-policy.json` from this repository, **Next**, name it `rl-thesis-kit`, **Create
   policy**.
2. **IAM** > **Users** > **Create user**. Name `thesis-cli`; leave "Provide user access to the AWS
   Management Console" **off**. **Next** > **Attach policies directly** > tick `rl-thesis-kit` > **Next**
   > **Create user**.
3. Open `thesis-cli` > **Security credentials** > **Create access key** > **Command Line Interface
   (CLI)** > tick the confirmation > **Next** > **Create access key**. Copy the **Access key ID** and the
   **Secret access key** now: the secret is shown only once.

Keep the two keys secret: not in the repository, a chat, a screenshot or the thesis. Whoever has them can
spend your credits. If they leak, delete the key on the same page and create another.

### Step 3. Install the AWS CLI in WSL and connect it

In your WSL terminal (no `sudo` needed; it installs into your home folder):

```bash
cd /tmp && curl -fsSLO https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip
unzip -q -o awscli-exe-linux-x86_64.zip && ./aws/install -i ~/.local/aws-cli -b ~/.local/bin --update
aws --version                  # aws-cli/2...; if "command not found": open a new terminal
aws configure                  # paste the access key ID and secret; region ap-southeast-1; output json
aws sts get-caller-identity    # shows ...:user/thesis-cli: the connection works
```

`aws configure` keeps the keys in `~/.aws/credentials`, outside the repository.

### Step 4. Set a budget alert

```bash
cd /mnt/d/GitHub/distributed-rate-limiter
aws/kit.sh budget your.email@gmail.com 20
```

It emails you when what you use (before credits) passes 10 USD and 20 USD in a month, and when it is
forecast to pass 20 USD. Setting up a budget is also one of the activities that earn more Free Tier
credits.

### Step 5. Check the vCPU quota

```bash
aws/kit.sh check
```

New accounts may start with a low quota of vCPUs. If `check` says there are too few:
- *Either* run the small size: `SIZE=small aws/kit.sh up` (and `SIZE=small` with every later command).
- *Or* ask for more: search **Service Quotas** > **AWS services** > **Amazon Elastic Compute Cloud
  (Amazon EC2)** > **Running On-Demand Standard (A, C, D, H, I, M, R, T, Z) instances** > **Request
  increase at account level** > `64`. The reason: "a graduation thesis benchmark, a few hours". A new
  account's request can take from minutes to a day or two.

## Each session: up, run, down (about an hour)

```bash
cd /mnt/d/GitHub/distributed-rate-limiter
aws/kit.sh up        # asks before it creates anything; then installs and builds (15 to 25 min)
aws/kit.sh run       # the benchmark (about 20 min); prints the table at the end
aws/kit.sh down      # ALWAYS, when you are done; ends with "nothing more is charged"
```

**What `up` does:**
1. Finds your public address: only it may connect, by SSH.
2. Creates an SSH key (kept in `~/.rl-thesis-bench/`, outside the repository) and the stack: the network,
   the firewall rules and the machines.
3. Copies the repository as it is on your disk (committed or not) to `rl-loadgen-1`, which builds the
   limiter and the load generator with `scripts/setup.sh`. The other machines get the build from it.
4. Starts Redis on the three Redis machines and forms the cluster.

If `up` stops halfway, read its message. `aws/kit.sh setup` retries the install and build, and
`aws/kit.sh down` removes what was made.

**What `run` measures:** for 1, 2 and 4 limiters, two rules, three runs each, 30 s a run:
- `customer`: the customer check of the thesis (burst then quota, two Redis scripts), Zipf over 10,000
  Pay customers.
- `api_default`: a leased rule, Zipf over 1,000,000 keys.

Before every run, Redis is emptied and the plans written again. Every load generator drives every
limiter at once. The table gives checks a second (median of the runs, and the range), the speed-up over
one limiter, and p99 latency:

```
| Rule | Limiters | Checks/s, median (range) | vs 1 limiter | p99 ms, median |
```

Send the whole `results/<date>_aws/` folder back, or just say that it is there, and the evidence and the
thesis section get written from it.

**While it is up:** `aws/kit.sh status` shows the machines and about what they have cost so far.
`aws/kit.sh ssh rl-limiter-1` opens a shell on one (the limiter's log is `rl/limiter.log`).

## When something goes wrong

| What you see | What to do |
| --- | --- |
| `the AWS CLI has no working credentials` | `aws configure` again; on the IAM page check that the access key is **Active** |
| `too few vCPUs` | `SIZE=small`, or the quota request of step 5 |
| `up` fails with `not authorized` | step 2: the policy is attached to `thesis-cli`; it was pasted whole |
| `up` fails with `Unsupported` or `InsufficientInstanceCapacity` | another zone: `aws/kit.sh down`, then `ZONE=ap-southeast-1b aws/kit.sh up` |
| `up` fails saying the plan does not allow it | Free plans do not include every service; **Billing** > **Free Tier** > **Upgrade plan** keeps the credits, but the card can then be charged beyond them: set the budget first |
| SSH stops answering | your public address changed (another Wi-Fi): `aws/kit.sh down` and `up` again |
| `setup failed` on a machine | read `~/.rl-thesis-bench/logs/<machine>.log`, then `aws/kit.sh setup` again |
| you forgot `down` | the machines ended themselves after 4 hours; run `aws/kit.sh down` anyway to remove the rest |
| you want to see what costs money | console > **Billing and Cost Management** > **Bills**, and **Credits** |

To check by hand that nothing is left: in the console, with your region selected, **EC2** > **Instances**
shows none running, and **CloudFormation** > **Stacks** has no `rl-thesis-bench`.

## Rules

- **One account.** AWS's Free Tier terms give the credits once per customer. Opening more accounts to get
  more credits can get them closed (see the terms).
- **The machines are throwaway.** Their Redis keeps nothing on disk, and `run` empties it before every
  cell. Never point the kit at a Redis that matters.
- **No keys in git.** `~/.rl-thesis-bench/` holds the SSH key, the machine addresses and the logs, in
  your Linux home: outside the repository, and readable only by you (on the Windows drive every file is
  readable by all, and ssh refuses such a key).

## The files

| File | What it is |
| --- | --- |
| `kit.sh` | the commands above |
| `stack.yaml` | the CloudFormation stack: network, firewall, machines (Ubuntu 24.04), the 4-hour timer |
| `node/redis.sh`, `node/build.sh`, `node/runtime.sh` | what runs on each kind of machine, over SSH |
| `iam-policy.json` | the permissions of the `thesis-cli` user |
| `test_local.sh` | runs `setup` and `run` on six Docker containers instead of AWS, to test the kit for free |
