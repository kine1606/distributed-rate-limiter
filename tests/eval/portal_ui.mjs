// The merchant portal end to end in headless Chrome, over the DevTools protocol: signs in, uses every
// action, checks what the page then shows, and takes screenshots (light, dark, phone width). Node 22 or
// later (its WebSocket); Chrome from CHROME, else Windows' default install. One line per check; exit 1 on
// a failure. Needs a running management API and a merchant seeded by tests/eval/portal_seed.py:
//   node tests/eval/portal_ui.mjs <portal url> <management key> <screenshot folder>
import { spawn } from "node:child_process";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
const [, , url, key, outDir] = process.argv;
const chrome = spawn(process.env.CHROME || "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  ["--headless=new", `--remote-debugging-port=${process.env.CDP_PORT || 9334}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), "portal-"))}`,
   "--window-size=1280,900", "about:blank"], {stdio: "ignore"});
process.on("exit", () => chrome.kill());  // also when a step throws
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let targets;
for (let i = 0; i < 50; i++) { try { targets = await (await fetch(`http://127.0.0.1:${process.env.CDP_PORT || 9334}/json`)).json(); break; } catch { await sleep(200); } }
const ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r));
let id = 0; const waiting = new Map(); const errors = []; const dialogs = [];
ws.addEventListener("message", (e) => {
  const m = JSON.parse(e.data);
  if (m.method === "Page.javascriptDialogOpening") {  // the portal confirms a suspension: accept
    dialogs.push(m.params.message);
    ws.send(JSON.stringify({id: ++id, method: "Page.handleJavaScriptDialog", params: {accept: true}}));
  }
  if (m.method === "Runtime.exceptionThrown") errors.push(m.params.exceptionDetails.exception?.description || "exception");
  if (m.method === "Log.entryAdded" && m.params.entry.level === "error" && m.params.entry.source !== "network") errors.push(m.params.entry.text);
  if (waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
});
const send = (method, params = {}) => new Promise((r) => { const n = ++id; waiting.set(n, r); ws.send(JSON.stringify({id: n, method, params})); });
const js = async (expr) => (await send("Runtime.evaluate", {expression: expr, awaitPromise: true, returnByValue: true})).result.result?.value;
const shot = async (name) => writeFileSync(join(outDir, name), Buffer.from((await send("Page.captureScreenshot", {format: "png", captureBeyondViewport: true})).result.data, "base64"));
let failed = 0;
const check = (name, ok, got) => { console.log(`${ok ? "ok  " : "FAIL"} ${name}${ok ? "" : ` — got: ${got}`}`); if (!ok) failed++; };
const text = (sel) => js(`document.querySelector(${JSON.stringify(sel)})?.textContent ?? ""`);
const submit = async (form, values) => {
  await js(`(() => { const f = document.getElementById(${JSON.stringify(form)});
    for (const [k, v] of Object.entries(${JSON.stringify(values)})) f.elements[k].value = v;
    f.requestSubmit(); })()`);
  await sleep(1200);
};
const click = async (sel) => { await js(`document.querySelector(${JSON.stringify(sel)}).click()`); await sleep(1200); };

await send("Page.enable"); await send("Runtime.enable"); await send("Log.enable");
await send("Page.navigate", {url});
await sleep(1500);
check("signed out: the form, no data", (await js(`!document.getElementById("signin").hidden && document.getElementById("main").hidden`)), "");
await js(`sessionStorage.setItem("rl-key", ${JSON.stringify(key)}); location.reload()`);
await sleep(3500);
check("signed in: the sign-in form is gone", await js(`document.getElementById("signin").hidden`), "");
check("customers listed", (await js(`document.querySelectorAll("#rows tr").length`)) > 0, await js(`document.querySelectorAll("#rows tr").length`));
check("plans listed", (await text("#plan-rows")).includes("pay"), await text("#plan-rows"));
await shot("p4_overview.png");

await click("#rows tr:first-child button");
check("panel opens on the customer", (await text("#c-id")) === "cust-00", await text("#c-id"));
check("move-to-plan starts from its plan", (await js(`document.getElementById("c-plan").elements.plan.value`)) === "pay", "");
check("invoice preview has the fee", (await text("#c-invoice")).includes("plan pay"), await text("#c-invoice"));
await submit("c-boost", {units: "500", minutes: "30"});
check("boost shown", (await text("#c-terms")).includes("+500 until"), await text("#c-terms"));
await submit("c-topup", {units: "1000"});
check("top-up shown", (await text("#c-terms")).includes("+1,000 this period"), await text("#c-terms"));
check("top-up on the invoice", (await text("#c-invoice")).includes("prepaid top-up"), await text("#c-invoice"));
await submit("c-limits", {overage: "postpaid", allowance: "100"});
check("custom limits shown", (await text("#c-terms")).includes("bill up to 100 more"), await text("#c-terms"));
await submit("c-limits", {daily: "", overage: "", allowance: ""});
check("nothing to change is refused, not ignored", (await text("#c-status")).startsWith("Not done"), await text("#c-status"));
await click("#c-suspend");
check("suspended, after a confirmation", (await text("#c-terms")).includes("Plansuspended") && dialogs.length === 1,
      `${await text("#c-terms")} | dialogs: ${dialogs}`);
await click("#c-resume");
check("resumed", (await text("#c-terms")).includes("Planpay"), await text("#c-terms"));
await shot("p4_customer.png");

await submit("plan-form", {name: "gold", daily: "5000", period: "month", burst: "200", refill: "100", overage: "postpaid", allowance: "1000", fee: "4900", overage_price: "30", topup_price: "25"});
check("plan saved and listed", (await text("#plan-rows")).includes("gold"), await text("#plan-status"));
await submit("add-customer", {id: "new-1", plan: "gold"});
check("customer added, its panel open", (await text("#c-id")) === "new-1", await text("#c-status"));
await submit("capacity-form", {rate: "1000", reserve: "20"});
check("capacity saved", (await text("#capacity-status")).startsWith("Saved"), await text("#capacity-status"));
await submit("webhook-form", {url: "http://127.0.0.1:9/hook"});
check("a private webhook address is refused", (await text("#webhook-status")).includes("non-public"), await text("#webhook-status"));
await submit("invoice-form", {});
check("an empty month says so", (await text("#invoice-status")).includes("No invoices"), await text("#invoice-status"));
await click("#invoice-csv");
check("the CSV download goes through", (await text("#invoice-status")).startsWith("Downloaded"), await text("#invoice-status"));
check("no script errors", errors.length === 0, errors.join(" | "));

for (const scheme of ["light", "dark"]) {
  await send("Emulation.setEmulatedMedia", {features: [{name: "prefers-color-scheme", value: scheme}]});
  await sleep(300);
  await shot(`p4_full_${scheme}.png`);
}
await send("Emulation.setDeviceMetricsOverride", {width: 390, height: 844, deviceScaleFactor: 1, mobile: true});
await sleep(500);
check("no sideways scroll at phone width", await js(`document.documentElement.scrollWidth <= window.innerWidth`), await js(`document.documentElement.scrollWidth`));
await shot("p4_phone.png");
ws.close();
process.exit(failed ? 1 : 0);
