// Charts: the five uPlot instances, their data buffers, history backfill, and
// the History field that governs every chart's time window. `uPlot` is a global
// from the CDN <script> loaded before this module.
import { $, fmtTime, fmtValue, onOff } from "./util.js";

// One field governs every chart: how many hours back to show. It sets both the
// visible sliding window and how much history we backfill / retain.
const historyInput = $("in-history-hours");
let historyHours = Math.min(168, Math.max(1, Math.round(Number(historyInput.value) || 6)));
let windowSec = historyHours * 3600;

let xs = [], tIn = [], tAmb = [], hum = [];
let xsAct = [], fanDuty = [], heaterDuty = [], humidDuty = [];

const GRID = { stroke: "#2a2f3a" };
function chartWidth(cardId) { return Math.max(280, $(cardId).clientWidth - 26); }

// Both chart groups share one sliding x-range: [latest - windowSec, latest].
// This is what makes the plot scroll instead of staying pinned to session
// start, and windowSec is what the History field controls.
function xRange() {
  const last = xs.length ? xs[xs.length - 1] : Date.now() / 1000;
  return [last - windowSec, last];
}
function xRangeAct() {
  const last = xsAct.length ? xsAct[xsAct.length - 1] : Date.now() / 1000;
  return [last - windowSec, last];
}
const xScale = { time: true, range: () => xRange() };
const xScaleAct = { time: true, range: () => xRangeAct() };
const xAxis = () => ({ stroke: "#8b929f", grid: GRID, ticks: GRID });

// Latest setpoints from fermenter/state; the threshold lines read these, and the
// controls module reads them as edit baselines.
export const sp = {
  targetTemp: null, targetHumidity: null, targetCeiling: null, dsMaxOverTarget: null,
  hysteresis: null, fanAfterHeatSec: null, maxHeatMin: null, heatCooldownMin: null,
  fanHeatPct: null, fanHumidPct: null, fanAutoPct: null,
  controlSensor: null, fanManualPct: null, runMinutes: null,
  heaterOverride: null, humidOverride: null,
};

// y-range that always includes a (dynamic) target value, so the setpoint
// line stays on-screen even while the reading is still far from target.
function rangeWith(getTarget) {
  return (u, dataMin, dataMax) => {
    const t = getTarget();
    let lo = dataMin, hi = dataMax;
    if (lo == null || hi == null) {            // no data yet
      if (t == null || isNaN(t)) return [0, 1];
      lo = hi = t;
    }
    if (t != null && !isNaN(t)) { lo = Math.min(lo, t); hi = Math.max(hi, t); }
    if (lo === hi) { lo -= 1; hi += 1; }
    const pad = (hi - lo) * 0.1;
    return [lo - pad, hi + pad];
  };
}

// uPlot has no built-in threshold line; this draw-hook plugin renders
// horizontal lines at given scale values (the setpoints). Identification is
// left to the legend below the chart, not text drawn on the line.
function hLines(getLines) {
  return {
    hooks: {
      draw: (u) => {
        const { ctx } = u, { left, top, width, height } = u.bbox;
        ctx.save();
        ctx.lineWidth = 1;
        for (const ln of getLines()) {
          if (ln.value == null || isNaN(ln.value)) continue;
          const y = u.valToPos(ln.value, ln.scale, true);
          if (y < top || y > top + height) continue; // off-screen, skip
          ctx.beginPath();
          ctx.setLineDash(ln.dash || []);
          ctx.strokeStyle = ln.color;
          ctx.moveTo(left, y); ctx.lineTo(left + width, y); ctx.stroke();
        }
        ctx.restore();
      }
    }
  };
}

function cursorReadout(elId, rows) {
  return {
    hooks: {
      setCursor: (u) => {
        const idx = u.cursor.idx;
        const el = $(elId);
        if (idx == null || idx < 0 || idx >= xs.length) {
          el.innerHTML = '<span class="empty">Hover plot for readings</span>';
          return;
        }
        const values = rows.map((r) =>
          `<span><strong style="color:${r.color}">${r.label}</strong> ${fmtValue(r.data[idx], r.digits, r.unit)}</span>`);
        el.innerHTML = `<span class="time">${fmtTime(xs[idx])}</span>${values.join("")}`;
      }
    }
  };
}

function cursorReadoutAct(elId, rows) {
  return {
    hooks: {
      setCursor: (u) => {
        const idx = u.cursor.idx;
        const el = $(elId);
        if (idx == null || idx < 0 || idx >= xsAct.length) {
          el.innerHTML = '<span class="empty">Hover plot for readings</span>';
          return;
        }
        const values = rows.map((r) =>
          `<span><strong style="color:${r.color}">${r.label}</strong> ${r.format ? r.format(r.data[idx]) : fmtValue(r.data[idx], r.digits, r.unit)
          }</span>`);
        el.innerHTML = `<span class="time">${fmtTime(xsAct[idx])}</span>${values.join("")}`;
      }
    }
  };
}

function actuatorPlot(cardId, readoutId, label, color, data, unit, digits, maxValue, format) {
  const yAxis = { scale: "y", stroke: "#8b929f", grid: GRID, ticks: GRID };
  if (format) {
    yAxis.splits = () => [0, 1];
    yAxis.values = (u, vals) => vals.map(format);
  } else {
    yAxis.values = (u, vals) => vals.map((v) => `${Math.round(v)}`);
  }

  return new uPlot({
    width: chartWidth(cardId), height: 150,
    legend: { show: false },
    scales: {
      x: xScaleAct,
      y: { range: () => [-0.05 * maxValue, maxValue * 1.05] },
    },
    series: [
      {},
      { label, stroke: color, scale: "y", width: 2, points: { show: false } },
    ],
    axes: [
      xAxis(),
      yAxis,
    ],
    plugins: [
      cursorReadoutAct(readoutId, [{ label, color, data, digits, unit, format }]),
    ],
  }, [xsAct, data], $(`chart-${cardId.replace("-plot-card", "")}`));
}

const uTemp = new uPlot({
  width: chartWidth("temp-card"), height: 260,
  legend: { show: false }, // using the custom legend strip below the chart
  scales: { x: xScale, C: { range: rangeWith(() => sp.targetTemp) } },
  series: [
    {},
    { label: "Inside °C", stroke: "#d85a30", scale: "C", width: 2 },
    { label: "Ambient °C", stroke: "#e0b020", scale: "C", width: 2 },
  ],
  axes: [xAxis(), { scale: "C", stroke: "#8b929f", grid: GRID, ticks: GRID }],
  plugins: [
    hLines(() => [
      { scale: "C", value: sp.targetTemp, color: "#d85a30", dash: [6, 4] },
      { scale: "C", value: sp.targetCeiling, color: "#b32424", dash: [2, 4] },
    ]),
    cursorReadout("readout-temp", [
      { label: "Inside", color: "#d85a30", data: tIn, digits: 2, unit: "°C" },
      { label: "Ambient", color: "#e0b020", data: tAmb, digits: 2, unit: "°C" },
    ]),
  ],
}, [xs, tIn, tAmb], $("chart-temp"));

const uHum = new uPlot({
  width: chartWidth("hum-card"), height: 200,
  legend: { show: false },
  scales: { x: xScale, "%": { range: rangeWith(() => sp.targetHumidity) } },
  series: [
    {},
    { label: "Humidity %", stroke: "#4a90e2", scale: "%", width: 2 },
  ],
  axes: [xAxis(), { scale: "%", stroke: "#8b929f", grid: GRID, ticks: GRID }],
  plugins: [
    hLines(() => [
      { scale: "%", value: sp.targetHumidity, color: "#4a90e2", dash: [6, 4] },
    ]),
    cursorReadout("readout-hum", [
      { label: "Humidity", color: "#4a90e2", data: hum, digits: 1, unit: "%" },
    ]),
  ],
}, [xs, hum], $("chart-hum"));

const uFan = actuatorPlot("fan-plot-card", "readout-fan", "Fan", "#8bc34a", fanDuty, "%", 0, 100);
const uHeater = actuatorPlot("heater-plot-card", "readout-heater", "Heater", "#d85a30", heaterDuty, "", 0, 1, onOff);
const uHumid = actuatorPlot("humid-plot-card", "readout-humid", "Humidifier", "#4a90e2", humidDuty, "", 0, 1, onOff);

function resizeCharts() {
  uTemp.setSize({ width: chartWidth("temp-card"), height: 260 });
  uHum.setSize({ width: chartWidth("hum-card"), height: 200 });
  uFan.setSize({ width: chartWidth("fan-plot-card"), height: 150 });
  uHeater.setSize({ width: chartWidth("heater-plot-card"), height: 150 });
  uHumid.setSize({ width: chartWidth("humid-plot-card"), height: 150 });
}

addEventListener("resize", resizeCharts);

document.querySelectorAll("[data-plot-toggle]").forEach((button) => {
  button.addEventListener("click", () => {
    const body = $(button.dataset.plotToggle);
    const collapsed = body.classList.toggle("hidden");
    button.setAttribute("aria-expanded", String(!collapsed));
    button.textContent = collapsed ? "Show" : "Hide";
    if (!collapsed) requestAnimationFrame(resizeCharts);
  });
});

// ---- point buffers ---------------------------------------------------
// Drop points older than the visible window (plus a little slack so the
// left edge isn't blank) from the front of each parallel array. Bounding by
// time rather than count keeps memory tied to the chosen History span.
function dropOld(x, ...series) {
  const cutoff = Date.now() / 1000 - windowSec - 30;
  let n = 0;
  while (n < x.length && x[n] < cutoff) n++;
  if (n) { x.splice(0, n); series.forEach((s) => s.splice(0, n)); }
}

// Replace an array's contents in place, so uPlot series / cursor-readout
// closures that captured the array reference keep pointing at live data.
function refill(arr, values) {
  arr.length = 0;
  for (let i = 0; i < values.length; i++) arr.push(values[i]);
}

// ---- telemetry: live points ------------------------------------------
export function pushPoint(d) {
  xs.push(Date.now() / 1000);
  tIn.push(d.dsTemp); tAmb.push(d.bmeTemp); hum.push(d.humidity);
  dropOld(xs, tIn, tAmb, hum);
  uTemp.setData([xs, tIn, tAmb]);
  uHum.setData([xs, hum]);
  $("v-temp").textContent = d.dsTemp.toFixed(2);
  $("v-hum").textContent = d.humidity.toFixed(1);
}

export function pushActuatorPoint(d) {
  xsAct.push(Date.now() / 1000);
  fanDuty.push(d.fanDuty ?? 0);
  heaterDuty.push(d.heaterOn ? 1 : 0);
  humidDuty.push(d.humidOn ? 1 : 0);
  dropOld(xsAct, fanDuty, heaterDuty, humidDuty);
  uFan.setData([xsAct, fanDuty]);
  uHeater.setData([xsAct, heaterDuty]);
  uHumid.setData([xsAct, humidDuty]);
}

// Feed the latest setpoints in and redraw so the threshold lines track a changed
// setpoint immediately (the y-range also keys off these, so a redraw re-frames).
export function feedSetpoints(d) {
  sp.targetTemp = d.targetTemp;
  sp.targetHumidity = d.targetHumidity;
  sp.targetCeiling = d.targetCeiling;
  sp.dsMaxOverTarget = d.dsMaxOverTarget;
  sp.hysteresis = d.hysteresis;
  sp.fanAfterHeatSec = d.fanAfterHeatSec;
  sp.maxHeatMin = d.maxHeatMin;
  sp.heatCooldownMin = d.heatCooldownMin;
  sp.fanHeatPct = d.fanHeatPct;
  sp.fanHumidPct = d.fanHumidPct;
  sp.fanAutoPct = d.fanAutoPct;
  sp.controlSensor = d.controlSensor || "ds";
  sp.fanManualPct = d.fanManualPct == null ? -1 : d.fanManualPct;
  sp.runMinutes = Math.max(0, d.runMinutes == null ? 0 : d.runMinutes);
  sp.heaterOverride = d.heaterOverride || "auto";
  sp.humidOverride = d.humidOverride || "auto";
  uTemp.redraw(true, true); uHum.redraw(true, true); // recalc axes -> re-frame
}

// Load the selected window of history into every chart. Both the sensor
// charts (telemetry) and the actuator charts (state) are backfilled; live
// MQTT points append on top. Arrays are refilled in place so uPlot keeps
// its references. Booleans (heater/humid on) become 1/0 for plotting.
async function backfill() {
  try {
    const h = await (await fetch(`/api/history?hours=${historyHours}`)).json();
    const tel = h.telemetry || {}, st = h.state || {};
    refill(xs, tel.t || []);
    refill(tIn, tel.dsTemp || []);
    refill(tAmb, tel.bmeTemp || []);
    refill(hum, tel.humidity || []);
    refill(xsAct, st.t || []);
    refill(fanDuty, (st.fanDuty || []).map((v) => v ?? 0));
    refill(heaterDuty, (st.heaterOn || []).map((v) => (v ? 1 : 0)));
    refill(humidDuty, (st.humidOn || []).map((v) => (v ? 1 : 0)));
    uTemp.setData([xs, tIn, tAmb]);
    uHum.setData([xs, hum]);
    uFan.setData([xsAct, fanDuty]);
    uHeater.setData([xsAct, heaterDuty]);
    uHumid.setData([xsAct, humidDuty]);
  } catch (_) { /* API unreachable — live data still works */ }
}
backfill();   // runs once at startup; live MQTT points append on top

// Changing the History field reframes and refetches every chart.
historyInput.addEventListener("change", () => {
  historyHours = Math.min(168, Math.max(1, Math.round(Number(historyInput.value) || historyHours)));
  historyInput.value = historyHours;
  windowSec = historyHours * 3600;
  backfill();
});
