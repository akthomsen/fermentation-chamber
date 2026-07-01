// "Run finished" chime, synthesised with the Web Audio API (no asset to ship).
// Browsers block audio until a user gesture, so we lazily create the context and
// resume it on the first interaction with the page; by the time a run completes
// it's unlocked.
let audioCtx = null;

function unlockAudio() {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") audioCtx.resume();
  } catch { /* audio unavailable — chime just won't play */ }
}
addEventListener("pointerdown", unlockAudio, { once: false });

export function playDoneChime() {
  if (!audioCtx) return;            // never unlocked (no gesture yet)
  const now = audioCtx.currentTime;
  // A short rising three-note motif, repeated once.
  [880, 1108.73, 1318.51, 0, 880, 1318.51].forEach((freq, i) => {
    if (!freq) return;
    const t = now + i * 0.18;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = "sine";
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(0.25, t + 0.02);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.16);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start(t);
    osc.stop(t + 0.18);
  });
}
