// Small shared helpers with no dependencies of their own.

export const $ = (id) => document.getElementById(id);

function almostEqual(a, b, eps = 0.001) {
  return Number.isFinite(a) && Number.isFinite(b) && Math.abs(a - b) <= eps;
}

function numberInput(el) {
  if (el.value.trim() === "") return NaN;
  const n = Number(el.value);
  return Number.isFinite(n) ? n : NaN;
}

export function numberOr(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

export function intInRange(value, fallback, min, max) {
  return Math.min(max, Math.max(min, Math.round(numberOr(value, fallback))));
}

export function numberChanged(el, baseline, fallback = null) {
  const base = baseline == null ? fallback : baseline;
  if (base == null) return el.value.trim() !== "";
  return !almostEqual(numberInput(el), base);
}

export function intChanged(el, baseline, fallback, min, max) {
  if (baseline == null && el.value.trim() === "") return false;
  return intInRange(el.value, fallback, min, max) !== baseline;
}

export function fmtTime(sec) {
  return new Date(sec * 1000).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  });
}

export function fmtValue(v, digits, unit) {
  return v == null || isNaN(v) ? "–" : `${v.toFixed(digits)} ${unit}`;
}

export function onOff(v) {
  return v >= 0.5 ? "ON" : "OFF";
}
