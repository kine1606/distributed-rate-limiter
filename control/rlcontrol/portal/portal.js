"use strict";
// The merchant portal (control/README.md): one page on the management API, signed in with the merchant's
// own management key. Every value from the server is written as text, never as HTML, and the page loads
// and calls nothing but this server (its Content-Security-Policy).
const $ = (id) => document.getElementById(id);
const number = new Intl.NumberFormat();
const money = (cents, currency = "USD") =>
  new Intl.NumberFormat(undefined, {style: "currency", currency}).format(cents / 100);
const path = (id) => `/v1/customers/${encodeURIComponent(id)}`;
let key = sessionStorage.getItem("rl-key");
let last = null;      // the previous /v1/usage answer, for the rate
let timer = null;
let cursors = [""];   // the `after` of each customer page shown so far; the last is the current page
let selected = null;  // the customer whose panel is open

// ---- talking to the API ----

// One call with the merchant's key: its JSON (null for 204, or for 404 when `missing` is allowed), or the
// response itself with `raw`. A refusal throws the server's own message.
async function api(url, {method = "GET", body, headers = {}, missing = false, raw = false} = {}) {
  const init = {method, cache: "no-store", headers: {Authorization: `Bearer ${key}`, ...headers}};
  if (body !== undefined) {
    init.body = JSON.stringify(body);
    init.headers["Content-Type"] = "application/json";
  }
  const r = await fetch(url, init);
  if (r.status === 401) {
    signOut("That management key was not accepted.");
    throw new Error("not signed in");
  }
  if (r.status === 404 && missing) return null;
  if (!r.ok) throw new Error(await problem(r));
  return raw ? r : r.status === 204 ? null : r.json();
}

async function problem(r) {  // FastAPI's {"detail": ...}: a sentence, an object, or a list of field errors
  try {
    const d = (await r.json()).detail;
    if (typeof d === "string") return d;
    if (Array.isArray(d)) return d.map((e) => `${(e.loc || []).slice(1).join(".")}: ${e.msg}`).join("; ");
    return d.message || JSON.stringify(d);
  } catch {
    return `HTTP ${r.status}`;
  }
}

// A form's filled fields: numbers as numbers, empty ones left out (an API field left out is unchanged).
function fields(form) {
  const out = {};
  for (const el of form.elements) {
    if (el.name && el.value !== "") out[el.name] = el.type === "number" ? Number(el.value) : el.value;
  }
  return out;
}

// The handler of a form or button: runs `action` once at a time, and tells its outcome in `status`.
function act(status, action) {
  return async (e) => {
    e?.preventDefault();
    const control = e?.submitter || e?.currentTarget;
    if (control?.disabled !== undefined) control.disabled = true;  // a double click must not act twice
    $(status).textContent = "Working…";
    try {
      $(status).textContent = (await action(e)) || "Done.";
    } catch (err) {
      $(status).textContent = `Not done: ${err.message}`;
    } finally {
      if (control?.disabled !== undefined) control.disabled = false;
    }
  };
}

// A handler that runs only once the merchant confirms: for actions a misclick must not take.
function confirmed(question, handler) {
  return (e) => {
    if (confirm(question())) handler(e);
  };
}

// ---- building the page (text only) ----

function cell(row, ...children) {
  const td = document.createElement("td");
  td.append(...children.map((c) => (c instanceof Node ? c : String(c))));
  row.append(td);
}

function rows(tbodyId, data) {  // data: one array of cells (text or nodes) per row
  $(tbodyId).replaceChildren(...data.map((cells) => {
    const tr = document.createElement("tr");
    for (const c of cells) cell(tr, ...(Array.isArray(c) ? c : [c]));
    return tr;
  }));
}

function button(label, onClick, kind = "quiet") {
  const b = document.createElement("button");
  b.type = "button";
  b.className = kind;
  b.textContent = label;
  b.addEventListener("click", onClick);
  return b;
}

function terms(dl, pairs) {
  dl.replaceChildren(...pairs.flatMap(([name, value]) => {
    const dt = document.createElement("dt");
    const dd = document.createElement("dd");
    dt.textContent = name;
    dd.textContent = value;
    return [dt, dd];
  }));
}

const policy = (t) => ({hard: "block", grace: `free grace of ${number.format(t.allowance)}`,
                        postpaid: `bill up to ${number.format(t.allowance)} more`})[t.overage];

function duration(ms) {
  const h = Math.floor(ms / 3_600_000);
  return h >= 48 ? `${Math.round(h / 24)} days` : h >= 1 ? `${h} h` : `${Math.ceil(ms / 60_000)} min`;
}

// ---- signing in and out ----

function show(signedIn, message = "") {
  $("signin").hidden = signedIn;
  $("main").hidden = !signedIn;
  $("signout").hidden = !signedIn;
  $("signin-msg").textContent = message;
}

function signOut(message) {
  sessionStorage.removeItem("rl-key");
  key = null;
  last = null;
  selected = null;
  clearInterval(timer);
  $("customer").hidden = true;
  show(false, message);
}

// ---- overview: this month over all customers, from every limiter instance ----

// The counters count admitted calls only; two answers apart give the calls a second, timed by Redis's
// clock.
async function poll() {
  try {
    const u = await api("/v1/usage");
    if (last && u.month === last.month && u.at_ms > last.at_ms) {
      $("rate").textContent = number.format(Math.round((u.admitted - last.admitted) * 1000 / (u.at_ms - last.at_ms)));
    }
    last = u;
    $("month").textContent = `(${u.month})`;
    $("admitted").textContent = number.format(u.admitted);
    $("billed").textContent = number.format(u.billed_overage);
    $("count").textContent = number.format(u.customers);
    $("status").textContent = "";
  } catch (e) {
    if (key) $("status").textContent = `Cannot read usage (${e.message}); trying again.`;
  }
}

// ---- customers ----

async function page() {
  const q = new URLSearchParams({limit: "25"});
  if (cursors.at(-1)) q.set("after", cursors.at(-1));
  const d = await api(`/v1/customers?${q}`);
  rows("rows", d.customers.map((c) => {
    const u = c.usage;
    const bar = document.createElement("meter");
    bar.max = Math.max(u.limit, 1);
    bar.value = Math.min(u.used, bar.max);
    bar.high = bar.max * 0.8;
    bar.optimum = 0;
    bar.setAttribute("aria-label", `${c.id}: ${u.used} of ${u.limit} used`);
    return [c.id, c.plan, [bar, ` ${number.format(u.used)} / ${number.format(u.limit)}`],
            number.format(u.overage_used), u.alerts.join(", ") || "–", button("Manage", () => openCustomer(c.id))];
  }));
  $("prev").disabled = cursors.length === 1;
  $("next").disabled = !d.next;
  $("next").dataset.after = d.next || "";
}

async function openCustomer(id, focus = true) {
  const c = await api(path(id));
  selected = id;
  const t = c.terms, u = c.usage;
  $("c-id").textContent = c.id;
  if (!c.suspended) $("c-plan").elements.plan.value = c.plan;  // "Move to plan" starts from the plan it is on
  terms($("c-terms"), [
    ["Plan", c.suspended ? "suspended" : c.plan],
    ["Quota", `${number.format(t.daily)} a ${t.period}`],
    ["This period", `${number.format(u.used)} of ${number.format(u.limit)} used; resets in ${duration(u.resets_in_ms)}`],
    ["Billed overage", number.format(u.overage_used)],
    ["Burst", `${number.format(t.burst)}, refilled ${number.format(t.refill)} a second`],
    ["At the quota", policy(t)],
    ["Boost", c.boost.active ? `+${number.format(c.boost.units)} until ${new Date(c.boost.until_ms).toLocaleString()}` : "none"],
    ["Top-up", c.topup.units ? `+${number.format(c.topup.units)} this period` : "none"],
    ["Prices", `${money(t.fee)} a month; per 1,000 calls: overage ${money(t.overage_price)}, top-up ${money(t.topup_price)}`],
    ["Alerts this period", u.alerts.join(", ") || "none"],
  ]);
  const inv = await api(`${path(id)}/invoice-preview`);
  rows("c-invoice", [
    ...inv.lines.map((l) => [l.description, l.item === "fee" ? "–" : number.format(l.units), money(l.amount, inv.currency)]),
    ["Total", "", money(inv.total, inv.currency)],
  ]);
  $("customer").hidden = false;
  if (focus) $("customer").focus();
}

// After a change: the panel, the table and the totals show the new state.
async function refresh() {
  await Promise.all([selected ? openCustomer(selected, false) : null, page(), poll()]);
}

function customerAction(method, suffix, body, headers = () => ({})) {
  return act("c-status", async (e) => {
    await api(`${path(selected)}${suffix}`, {method, body: body?.(e), headers: headers()});
    await refresh();
  });
}

// ---- plans ----

async function loadPlans() {
  const plans = await api("/v1/plans");
  const names = Object.keys(plans).sort();
  for (const select of document.querySelectorAll(".plan-choice")) {
    select.replaceChildren(...names.map((n) => new Option(n, n)));
  }
  rows("plan-rows", names.map((n) => {
    const t = plans[n];
    return [n, `${number.format(t.daily)} a ${t.period}`, `${number.format(t.burst)}, ${number.format(t.refill)}/s`,
            policy(t), money(t.fee), money(t.overage_price), money(t.topup_price),
            [button("Edit", () => editPlan(n, t)), " ", button("Delete", confirmed(() => `Delete plan ${n}?`, act("plan-status", async () => {
              await api(`/v1/plans/${encodeURIComponent(n)}`, {method: "DELETE"});
              await loadPlans();
              return `Plan ${n} deleted; its customers keep their numbers.`;
            })), "danger")]];
  }));
}

function editPlan(name, t) {
  const f = $("plan-form");
  for (const el of f.elements) {
    if (el.name in t) el.value = t[el.name];
  }
  f.elements.name.value = name;
  f.elements.daily.focus();
}

// ---- capacity, invoices, webhook ----

async function loadCapacity() {
  const c = await api("/v1/capacity", {missing: true});
  const f = $("capacity-form");
  f.elements.rate.value = c ? c.rate : "";
  f.elements.burst.value = c ? c.burst : "";
  f.elements.reserve.value = c?.reserve ?? 0;
  $("capacity-status").textContent = c ? "" : "No capacity set: every call that passes its customer's limits is admitted.";
}

async function loadInvoices(month) {
  const list = await api(`/v1/invoices?month=${encodeURIComponent(month)}`);
  rows("invoice-rows", list.map((i) => [i.id, i.customer, i.plan, money(i.total, i.currency), i.status]));
  return list.length ? `${list.length} invoices for ${month}.` : `No invoices for ${month} (a month is invoiced once it has ended).`;
}

async function downloadCsv(month) {
  const r = await api(`/v1/invoices.csv?month=${encodeURIComponent(month)}`, {raw: true});
  const a = document.createElement("a");
  a.href = URL.createObjectURL(await r.blob());
  a.download = `invoices-${month}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);  // after the browser has started the download
  return `Downloaded invoices-${month}.csv.`;
}

async function loadWebhook() {
  const w = await api("/v1/webhook", {missing: true});
  $("webhook-form").elements.url.value = w ? w.url : "";
  $("webhook-status").textContent = w ? "" : "No webhook: alerts are only in each customer's usage.";
}

// ---- wiring ----

async function start() {
  show(true);
  const month = new Date();
  month.setUTCDate(1);
  month.setUTCMonth(month.getUTCMonth() - 1);  // the last month that has ended, the latest with invoices
  $("invoice-form").elements.month.value = month.toISOString().slice(0, 7);
  try {
    await Promise.all([loadPlans(), page(), loadCapacity(), loadWebhook(), poll()]);
  } catch (e) {
    if (key) $("status").textContent = `Cannot load the portal (${e.message}).`;
    return;
  }
  timer = setInterval(poll, 5000);
}

$("signin").addEventListener("submit", (e) => {
  e.preventDefault();
  key = $("key").value.trim();
  $("key").value = "";
  sessionStorage.setItem("rl-key", key);
  start();
});
$("signout").addEventListener("click", () => signOut(""));
const turn = (move) => () => {
  move();
  page().catch((e) => { $("status").textContent = `Cannot load customers (${e.message}).`; });
};
$("next").addEventListener("click", turn(() => cursors.push($("next").dataset.after)));
$("prev").addEventListener("click", turn(() => cursors.pop()));

$("add-customer").addEventListener("submit", act("c-status", async (e) => {
  const body = fields(e.target);
  await api("/v1/customers", {method: "POST", body});
  e.target.reset();
  await refresh();
  await openCustomer(body.id);
  return `Customer ${body.id} added.`;
}));
$("c-plan").addEventListener("submit", customerAction("PUT", "/plan", (e) => fields(e.target)));
$("c-limits").addEventListener("submit", customerAction("PATCH", "/limits", (e) => fields(e.target)));
$("c-boost").addEventListener("submit", customerAction("POST", "/boost", (e) => fields(e.target)));
$("c-boost-end").addEventListener("click", customerAction("DELETE", "/boost"));
$("c-topup").addEventListener("submit", customerAction("POST", "/topups", (e) => fields(e.target),
                                                       () => ({"Idempotency-Key": crypto.randomUUID()})));  // a retry buys once
$("c-suspend").addEventListener("click", confirmed(() => `Suspend ${selected}? Its calls are refused at once.`,
                                                   customerAction("POST", "/suspend")));
$("c-resume").addEventListener("click", customerAction("POST", "/resume"));

$("plan-form").addEventListener("submit", act("plan-status", async (e) => {
  const {name, ...body} = fields(e.target);
  await api(`/v1/plans/${encodeURIComponent(name)}`, {method: "PUT", body});
  await loadPlans();
  return `Plan ${name} saved. Customers move to it from their panel; those already on it keep their numbers.`;
}));
$("capacity-form").addEventListener("submit", act("capacity-status", async (e) => {
  await api("/v1/capacity", {method: "PUT", body: fields(e.target)});
  await loadCapacity();
  return "Saved; every limiter applies it at once.";
}));
$("capacity-off").addEventListener("click", act("capacity-status", async () => {
  await api("/v1/capacity", {method: "DELETE"});
  await loadCapacity();
  return "Removed.";
}));
$("invoice-form").addEventListener("submit", act("invoice-status", (e) => loadInvoices(e.target.elements.month.value)));
$("invoice-csv").addEventListener("click", act("invoice-status", () => downloadCsv($("invoice-form").elements.month.value)));
$("webhook-form").addEventListener("submit", act("webhook-status", async (e) => {
  const w = await api("/v1/webhook", {method: "PUT", body: fields(e.target)});
  return `Saved. The signing secret, shown this once: ${w.secret}`;
}));
$("webhook-off").addEventListener("click", act("webhook-status", async () => {
  await api("/v1/webhook", {method: "DELETE"});
  await loadWebhook();
  return "Removed.";
}));

if (key) start(); else show(false);
