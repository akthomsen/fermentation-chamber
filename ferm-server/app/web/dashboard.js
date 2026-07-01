// Entry point: pull the modules together and start the live relay. Importing
// charts.js / controls.js runs their setup (chart creation, history backfill,
// control wiring) as a side effect; this file only wires the transport to them.
import { $ } from "./util.js";
import { pushPoint } from "./charts.js";
import { applyState } from "./controls.js";
import { connectLive } from "./live.js";
import { initNotifications } from "./notifications.js";

// ---- alert: active fault banner from the twin -------------------------
const alertEl = $("alert");
function applyAlert(raw) {
  let a = null;
  if (raw) { try { a = JSON.parse(raw); } catch { a = null; } }
  if (!a || a.active === false) { alertEl.classList.remove("show"); return; }
  alertEl.querySelector(".code").textContent = a.code || "ALERT";
  alertEl.querySelector(".msg").textContent = a.message || "";
  alertEl.classList.toggle("warn", a.severity === "warning");
  alertEl.classList.add("show");
}

connectLive({ telemetry: pushPoint, state: applyState, alert: applyAlert });

// Register the "Enable notifications" button (Web Push opt-in).
initNotifications();
