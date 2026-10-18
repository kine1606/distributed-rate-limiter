# The plan catalogue, in one place (docs/CUSTOMER_PLANS.md has the reasons). Sourced by
# tools/seed_plans.sh and tools/rlctl: field-value pairs of the hash {cust:N}:plan, after `plan <name>`.
declare -A PLAN=(
  [free]="daily 200 burst 2 refill 2 overage hard extra 0 failure closed"
  [pay]="daily 2000 burst 20 refill 20 overage grace extra 200 failure open"
  [pay_cap]="daily 2000 burst 20 refill 20 overage cap extra 1000 failure open"
  [enterprise]="daily 1000000 burst 2000 refill 2000 overage cap extra 100000 failure open"
  [suspended]="daily 0 burst 0 refill 0 overage hard extra 0 failure closed"
)
