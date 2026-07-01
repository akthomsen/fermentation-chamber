// Live data transport: the FastAPI WebSocket relay in, commands out.
// The browser no longer speaks MQTT (no broker credential in this page). The
// backend subscribes to the broker and relays telemetry/state/alert here as
// {topic, payload} frames; commands go back via POST /api/command.
import { $ } from "./util.js";

const statusEl = $("status");

function setStatus(text, up) {
  statusEl.textContent = text;
  statusEl.className = up ? "up" : "down";
}

// Open (and keep reopening) the relay socket, dispatching each frame to the
// handlers: { telemetry(d), state(d), alert(rawPayload) }.
export function connectLive(handlers) {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => setStatus("connected", true);
  ws.onclose = () => {
    setStatus("disconnected — reconnecting…", false);
    setTimeout(() => connectLive(handlers), 2000);   // backend or page offline; retry
  };
  ws.onerror = () => ws.close();      // onclose handles the retry
  ws.onmessage = (ev) => {
    let frame; try { frame = JSON.parse(ev.data); } catch { return; }
    const { topic, payload } = frame;
    // fermenter/alert is retained and cleared with an empty payload, so
    // handle it before the JSON parse (an empty body means "no fault").
    if (topic === "fermenter/alert") { handlers.alert(payload); return; }
    let d; try { d = JSON.parse(payload); } catch { return; }
    if (topic === "fermenter/telemetry") handlers.telemetry(d);
    else if (topic === "fermenter/state") handlers.state(d);
  };
}

// Send a command through the backend mediator, which validates and clamps it
// before publishing to the broker. The fermenter/state echo remains the real
// confirmation, exactly as before.
export async function sendCommand(body) {
  try {
    const r = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!r.ok) {
      setStatus(`command rejected (${r.status})`, false);
      return false;
    }
    return true;
  } catch {
    setStatus("command failed — backend offline?", false);
    return false;
  }
}
