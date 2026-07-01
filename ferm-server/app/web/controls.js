// Controls: the setpoint/actuator inputs, dirty-tracking so the ~1 Hz state echo
// can't clobber a value mid-edit, applying the fermenter/state message to the
// status cards + inputs, and sending commands. Setpoint baselines and the chart
// feed live in charts.js; the transport lives in live.js.
import { $, numberChanged, intChanged, numberOr, intInRange } from "./util.js";
import { sp, pushActuatorPoint, feedSetpoints } from "./charts.js";
import { sendCommand } from "./live.js";
import { playDoneChime } from "./audio.js";

// ---- elements --------------------------------------------------------
const tempInput = $("in-temp"), humInput = $("in-hum");
const ceilInput = $("in-ceil"), dsMaxInput = $("in-ds-max");
const hystInput = $("in-hyst"), fanAfterHeatInput = $("in-fan-after-heat");
const maxHeatInput = $("in-max-heat"), heatCooldownInput = $("in-heat-cooldown");
const fanHeatInput = $("in-fan-heat"), fanHumidInput = $("in-fan-humid"), fanAutoInput = $("in-fan-auto");
const fanInput = $("in-fan"), runHoursInput = $("in-run-hours"), runMinutesInput = $("in-run-minutes");
const powerBtn = $("power");
let halted = false;         // last reported halted state, drives the power button
let runComplete = false;    // last reported run-complete, so we chime only on the edge
let runCompleteSeeded = false; // skip the chime on the first retained state we receive
let fanMode = "auto";
let heaterOverride = "auto";
let humidOverride = "auto";
let controlSensor = "ds";

// Inputs the user has edited but not yet applied. While an input is dirty we
// stop seeding it from fermenter/state, so the ~1 Hz state echo can't clobber
// a value mid-edit. Cleared on Send commands, after which the device echo re-syncs it.
const dirty = new Set();
const dirtyFieldByKey = new Map();

function registerDirtyField(key, el) {
  const field = el && el.closest(".field");
  if (field) dirtyFieldByKey.set(key, field);
}

[
  tempInput, humInput, ceilInput, dsMaxInput, hystInput, fanAfterHeatInput,
  maxHeatInput, heatCooldownInput, fanHeatInput, fanHumidInput, fanAutoInput,
  fanInput, runHoursInput, runMinutesInput,
].forEach((el) => registerDirtyField(el.id, el));
registerDirtyField("control-sensor", $("control-sensor-seg"));
registerDirtyField("fan-mode", $("fan-seg"));
registerDirtyField("heater", $("heater-seg"));
registerDirtyField("humid", $("humid-seg"));

function currentRunMinutes() {
  return (
    intInRange(runHoursInput.value, 0, 0, 100000) * 60 +
    intInRange(runMinutesInput.value, 0, 0, 59)
  );
}

function currentFanManualPct() {
  return fanMode === "auto" ? -1 : intInRange(fanInput.value, 0, 0, 100);
}

function setDirtyIf(key, changed) {
  if (changed) dirty.add(key);
}

function recomputeDirty() {
  dirty.clear();
  setDirtyIf("in-temp", numberChanged(tempInput, sp.targetTemp));
  setDirtyIf("in-hum", numberChanged(humInput, sp.targetHumidity));
  setDirtyIf("in-ceil", numberChanged(ceilInput, sp.targetCeiling));
  setDirtyIf("in-ds-max", numberChanged(dsMaxInput, sp.dsMaxOverTarget, 0));
  setDirtyIf("in-hyst", numberChanged(hystInput, sp.hysteresis));
  setDirtyIf("in-fan-after-heat", intChanged(fanAfterHeatInput, sp.fanAfterHeatSec, 0, 0, 3600));
  setDirtyIf("in-max-heat", intChanged(maxHeatInput, sp.maxHeatMin, 1, 1, 600));
  setDirtyIf("in-heat-cooldown", intChanged(heatCooldownInput, sp.heatCooldownMin, 0, 0, 600));
  setDirtyIf("in-fan-heat", intChanged(fanHeatInput, sp.fanHeatPct, 100, 0, 100));
  setDirtyIf("in-fan-humid", intChanged(fanHumidInput, sp.fanHumidPct, 70, 0, 100));
  setDirtyIf("in-fan-auto", intChanged(fanAutoInput, sp.fanAutoPct, 50, 0, 100));
  setDirtyIf("control-sensor", controlSensor !== (sp.controlSensor || "ds"));
  const fanChanged = currentFanManualPct() !== (sp.fanManualPct == null ? -1 : sp.fanManualPct);
  setDirtyIf("fan-mode", fanChanged);
  if (fanMode !== "auto") setDirtyIf("in-fan", fanChanged);
  const runChanged = currentRunMinutes() !== Math.max(0, sp.runMinutes == null ? 0 : sp.runMinutes);
  setDirtyIf("in-run-hours", runChanged);
  setDirtyIf("in-run-minutes", runChanged);
  setDirtyIf("heater", heaterOverride !== (sp.heaterOverride || "auto"));
  setDirtyIf("humid", humidOverride !== (sp.humidOverride || "auto"));
  renderDirtyControls();
}

function renderDirtyControls() {
  document.querySelectorAll(".field.changed").forEach((el) => el.classList.remove("changed"));
  dirty.forEach((key) => dirtyFieldByKey.get(key)?.classList.add("changed"));
  const count = dirty.size;
  const apply = $("apply");
  apply.disabled = count === 0;
  apply.classList.toggle("has-changes", count > 0);
  apply.textContent = count === 0 ? "No changes" : `Send ${count} change${count === 1 ? "" : "s"}`;
}

function clearDirtyControls() {
  dirty.clear();
  renderDirtyControls();
}

[
  tempInput, humInput, ceilInput, dsMaxInput, hystInput, fanAfterHeatInput,
  maxHeatInput, heatCooldownInput, fanHeatInput, fanHumidInput, fanAutoInput,
  fanInput, runHoursInput, runMinutesInput,
].forEach((el) =>
  el.addEventListener("input", recomputeDirty));
renderDirtyControls();

function setSegmentActive(attr, value) {
  document.querySelectorAll(`[${attr}]`).forEach((b) =>
    b.classList.toggle("active", b.getAttribute(attr) === value));
}

// ---- run-time readouts -----------------------------------------------
function findField(d, keys) {
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(d, key) && d[key] != null) {
      return { key, value: d[key] };
    }
  }
  return null;
}

function minutesFromDuration(key, value) {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const k = key.toLowerCase();
  if (k.includes("ms") || (k.endsWith("time") && Math.abs(value) > 100000)) {
    return value / 60000;
  }
  if (k.includes("sec") || (k.endsWith("time") && Math.abs(value) > 1440)) {
    return value / 60;
  }
  return value;
}

function formatDurationField(entry) {
  if (!entry) return "–";
  if (typeof entry.value === "string" && entry.value.trim()) return entry.value;
  const minutes = minutesFromDuration(entry.key, entry.value);
  if (minutes == null) return "–";
  const rounded = Math.max(0, Math.round(minutes));
  const h = Math.floor(rounded / 60);
  const m = rounded % 60;
  return h > 0 ? `${h}h ${String(m).padStart(2, "0")}m` : `${m}m`;
}

function applyTimeFields(d) {
  const setEntry = findField(d, [
    "setTime", "set_time", "setMinutes", "setMin", "runtimeMinutes",
    "runLimitMinutes", "runMinutes",
  ]);
  const upEntry = findField(d, [
    "upTime", "uptime", "up_time", "uptimeMinutes", "upMinutes",
    "elapsedMinutes", "elapsedMin", "runElapsed", "runElapsedMinutes",
  ]);
  const leftEntry = findField(d, [
    "leftTime", "left_time", "leftMinutes", "leftMin", "remainingMinutes",
    "remainingMin", "runRemaining", "runRemainingMinutes",
  ]);

  $("v-time-set").textContent = formatDurationField(setEntry);
  $("v-time-up").textContent = formatDurationField(upEntry);
  $("v-time-left").textContent = formatDurationField(leftEntry);
}

// ---- state: status cards + seed controls (without fighting the user) -
export function applyState(d) {
  pushActuatorPoint(d);
  $("v-temp-set").textContent = d.targetTemp.toFixed(1);
  $("v-hum-set").textContent = d.targetHumidity.toFixed(0);
  $("v-ceil").textContent = d.targetCeiling.toFixed(1);
  $("v-ds-max").textContent = (d.dsMaxOverTarget ?? 0).toFixed(1);
  $("v-control-sensor").textContent = d.controlSensor || "ds";
  $("v-fan").textContent = d.fanDuty;
  $("dot-heater").classList.toggle("on", d.heaterOn);
  $("dot-humid").classList.toggle("on", d.humidOn);
  applyTimeFields(d);

  if (!dirty.has("heater")) {
    heaterOverride = d.heaterOverride || "auto";
    setSegmentActive("data-heater", heaterOverride);
  }

  if (!dirty.has("humid")) {
    humidOverride = d.humidOverride || "auto";
    setSegmentActive("data-humid", humidOverride);
  }
  $("v-humid-note").textContent =
    humidOverride === "off" ? "Humidifier forced off" :
      humidOverride === "on" ? "Humidifier forced on" : "";

  // Feed the threshold lines / y-ranges and redraw the sensor charts.
  feedSetpoints(d);

  // Chime once on the run-complete edge (timer reached zero). The first
  // retained state only seeds the baseline so we don't beep on page load.
  const nowComplete = !!d.runComplete;
  if (runCompleteSeeded && nowComplete && !runComplete) playDoneChime();
  runComplete = nowComplete;
  runCompleteSeeded = true;

  halted = d.halted;
  $("v-halted").textContent = halted ? "⏸ Halted" : "▶ Running";
  powerBtn.textContent = halted ? "Start" : "Stop";
  powerBtn.classList.toggle("stopped", halted);

  // Only seed an input the user has not edited since the last send.
  if (!dirty.has("in-temp")) tempInput.value = d.targetTemp;
  if (!dirty.has("in-hum")) humInput.value = d.targetHumidity;
  if (!dirty.has("in-ceil")) ceilInput.value = d.targetCeiling;
  if (!dirty.has("in-ds-max")) dsMaxInput.value = d.dsMaxOverTarget ?? 0;
  if (!dirty.has("in-hyst")) hystInput.value = d.hysteresis ?? "";
  if (!dirty.has("in-fan-after-heat")) fanAfterHeatInput.value = d.fanAfterHeatSec ?? "";
  if (!dirty.has("in-max-heat")) maxHeatInput.value = d.maxHeatMin ?? "";
  if (!dirty.has("in-heat-cooldown")) heatCooldownInput.value = d.heatCooldownMin ?? "";
  if (!dirty.has("in-fan-heat")) fanHeatInput.value = d.fanHeatPct ?? "";
  if (!dirty.has("in-fan-humid")) fanHumidInput.value = d.fanHumidPct ?? "";
  if (!dirty.has("in-fan-auto")) fanAutoInput.value = d.fanAutoPct ?? "";
  if (!dirty.has("control-sensor")) {
    controlSensor = d.controlSensor || "ds";
    setSegmentActive("data-control-sensor", controlSensor);
  }
  if (!dirty.has("fan-mode")) {
    fanMode = d.fanManualPct == null || d.fanManualPct < 0 ? "auto" : "manual";
    setSegmentActive("data-fan-mode", fanMode);
  }
  if (!dirty.has("in-fan")) {
    const fanManualPct = d.fanManualPct == null || d.fanManualPct < 0 ? d.fanDuty : d.fanManualPct;
    fanInput.value = fanManualPct;
  }
  if (!dirty.has("in-run-hours") && !dirty.has("in-run-minutes")) {
    const runMinutes = Math.max(0, d.runMinutes == null ? 0 : d.runMinutes);
    runHoursInput.value = Math.floor(runMinutes / 60);
    runMinutesInput.value = runMinutes % 60;
  }
  recomputeDirty();
}

// ---- controls --------------------------------------------------------
document.querySelectorAll(".stepper button").forEach((b) => {
  b.addEventListener("click", () => {
    const input = $(b.dataset.target);
    const step = parseFloat(b.dataset.step);
    const min = input.min === "" ? -Infinity : parseFloat(input.min);
    const max = input.max === "" ? Infinity : parseFloat(input.max);
    const next = Math.min(max, Math.max(min, parseFloat(input.value || 0) + step));
    input.value = Number.isInteger(step) ? next.toFixed(0) : next.toFixed(1);
    if (input.id === "in-fan") {
      fanMode = "manual";
      setSegmentActive("data-fan-mode", fanMode);
    }
    recomputeDirty(); // a stepper press is an edit too
  });
});

fanInput.addEventListener("input", () => {
  fanMode = "manual";
  setSegmentActive("data-fan-mode", fanMode);
  recomputeDirty();
});

document.querySelectorAll("[data-fan-mode]").forEach((b) => {
  b.addEventListener("click", () => {
    fanMode = b.dataset.fanMode;
    setSegmentActive("data-fan-mode", fanMode);
    recomputeDirty();
  });
});

document.querySelectorAll("[data-control-sensor]").forEach((b) => {
  b.addEventListener("click", () => {
    controlSensor = b.dataset.controlSensor;
    setSegmentActive("data-control-sensor", controlSensor);
    recomputeDirty();
  });
});

document.querySelectorAll("[data-heater]").forEach((b) => {
  b.addEventListener("click", () => {
    heaterOverride = b.dataset.heater;
    setSegmentActive("data-heater", heaterOverride);
    recomputeDirty();
  });
});

document.querySelectorAll("[data-humid]").forEach((b) => {
  b.addEventListener("click", () => {
    humidOverride = b.dataset.humid;
    setSegmentActive("data-humid", humidOverride);
    recomputeDirty();
  });
});

$("apply").addEventListener("click", () => {
  // Confirmation is the fermenter/state echo, not this click.
  if (dirty.size === 0) return;

  const setpoint = {};
  if (dirty.has("in-temp")) setpoint.targetTemp = numberOr(tempInput.value, sp.targetTemp);
  if (dirty.has("in-hum")) setpoint.targetHumidity = numberOr(humInput.value, sp.targetHumidity);
  if (dirty.has("in-ceil")) setpoint.targetCeiling = numberOr(ceilInput.value, sp.targetCeiling);
  if (dirty.has("in-ds-max")) setpoint.dsMaxOverTarget = numberOr(dsMaxInput.value, sp.dsMaxOverTarget);
  if (dirty.has("in-hyst")) setpoint.hysteresis = numberOr(hystInput.value, sp.hysteresis ?? 0.1);
  if (dirty.has("in-fan-after-heat")) setpoint.fanAfterHeatSec = intInRange(fanAfterHeatInput.value, sp.fanAfterHeatSec ?? 0, 0, 3600);
  if (dirty.has("in-max-heat")) setpoint.maxHeatMin = intInRange(maxHeatInput.value, sp.maxHeatMin ?? 1, 1, 600);
  if (dirty.has("in-heat-cooldown")) setpoint.heatCooldownMin = intInRange(heatCooldownInput.value, sp.heatCooldownMin ?? 0, 0, 600);
  if (dirty.has("in-fan-heat")) setpoint.fanHeatPct = intInRange(fanHeatInput.value, sp.fanHeatPct ?? 100, 0, 100);
  if (dirty.has("in-fan-humid")) setpoint.fanHumidPct = intInRange(fanHumidInput.value, sp.fanHumidPct ?? 70, 0, 100);
  if (dirty.has("in-fan-auto")) setpoint.fanAutoPct = intInRange(fanAutoInput.value, sp.fanAutoPct ?? 50, 0, 100);
  if (dirty.has("control-sensor")) setpoint.controlSensor = controlSensor;
  if (dirty.has("fan-mode") || dirty.has("in-fan")) {
    setpoint.fanManualPct = fanMode === "auto" ? -1 : intInRange(fanInput.value, 0, 0, 100);
  }
  const runtimeMinutes =
    intInRange(runHoursInput.value, 0, 0, 100000) * 60 +
    intInRange(runMinutesInput.value, 0, 0, 59);
  if (dirty.has("in-run-hours") || dirty.has("in-run-minutes")) {
    setpoint.runMinutes = runtimeMinutes;
  }

  const controls = [];
  if (dirty.has("heater")) {
    controls.push(heaterOverride === "auto" ? "heater_auto" : `heater_${heaterOverride}`);
  }
  if (dirty.has("humid")) {
    controls.push(humidOverride === "auto" ? "humidifier_auto" : `humidifier_${humidOverride}`);
  }

  const body = {};
  if (Object.keys(setpoint).length > 0) body.setpoint = setpoint;
  if (controls.length > 0) body.controls = controls;
  if (Object.keys(body).length > 0) sendCommand(body);

  clearDirtyControls(); // hand the fields back to the device's echoed truth
});

powerBtn.addEventListener("click", () => {
  // Mirrors the physical knob: start (resets run timer) vs stop.
  sendCommand({ controls: [halted ? "start" : "stop"] });
});
