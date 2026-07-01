// Browser side of Web Push: one button that registers the service worker,
// asks permission, subscribes to the push service, and hands the subscription
// to the backend (which owns delivery). No VAPID private key or secret ever
// touches this page — it fetches only the public key from /api/push.
//
// Requires a secure context (HTTPS, or localhost): the Push API and service
// workers are unavailable over plain http://<ip>, so the button reports
// "unsupported" there rather than failing cryptically.
import { $ } from "./util.js";

const supported =
  "serviceWorker" in navigator && "PushManager" in window && "Notification" in window;

function urlBase64ToUint8Array(base64String) {
  const padding = "=".repeat((4 - (base64String.length % 4)) % 4);
  const base64 = (base64String + padding).replace(/-/g, "+").replace(/_/g, "/");
  const raw = atob(base64);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out;
}

async function currentSubscription() {
  const reg = await navigator.serviceWorker.getRegistration();
  return reg ? reg.pushManager.getSubscription() : null;
}

async function enable() {
  const perm = await Notification.requestPermission();
  if (perm !== "granted") throw new Error("permission " + perm);

  const reg = await navigator.serviceWorker.register("/sw.js");
  await navigator.serviceWorker.ready;

  const res = await fetch("/api/push/vapid-public-key");
  if (!res.ok) throw new Error("vapid key unavailable (" + res.status + ")");
  const { key } = await res.json();

  const sub = await reg.pushManager.subscribe({
    userVisibleOnly: true,
    applicationServerKey: urlBase64ToUint8Array(key),
  });

  const r = await fetch("/api/push/subscribe", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(sub),
  });
  if (!r.ok) throw new Error("subscribe failed (" + r.status + ")");
}

async function disable() {
  const sub = await currentSubscription();
  if (!sub) return;
  // Tell the backend first so it stops pushing, then drop the local sub.
  await fetch("/api/push/unsubscribe", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(sub),
  });
  await sub.unsubscribe();
}

export function initNotifications() {
  const btn = $("notify-btn");
  if (!btn) return;

  if (!supported) {
    btn.textContent = "Notifications unsupported";
    btn.disabled = true;
    return;
  }

  async function refresh() {
    if (Notification.permission === "denied") {
      btn.textContent = "Notifications blocked";
      btn.disabled = true;
      return;
    }
    const sub = await currentSubscription();
    btn.disabled = false;
    btn.dataset.on = sub ? "1" : "";
    btn.textContent = sub ? "Notifications on" : "Enable notifications";
  }

  btn.addEventListener("click", async () => {
    btn.disabled = true;
    try {
      if (btn.dataset.on) await disable();
      else await enable();
    } catch (e) {
      console.error("notifications:", e);
      btn.textContent = "Enable failed — retry";
    } finally {
      await refresh();
    }
  });

  refresh();
}
