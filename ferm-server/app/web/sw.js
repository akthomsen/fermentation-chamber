// Service worker: receives Web Push events and shows the notification, even
// when the dashboard tab is closed. Served from the web root, so it controls
// the whole origin (scope "/").

self.addEventListener("push", (event) => {
  let data = {};
  try {
    data = event.data ? event.data.json() : {};
  } catch {
    data = { body: event.data ? event.data.text() : "" };
  }
  const title = data.title || "Fermentation chamber";
  const options = {
    body: data.body || "",
    // Same tag => a repeat replaces the old notification instead of stacking;
    // renotify still buzzes the phone so a live fault isn't silently swallowed.
    tag: data.tag || "fermenter-alert",
    renotify: true,
    requireInteraction: data.severity === "critical",
    data: { url: "/" },
  };
  event.waitUntil(self.registration.showNotification(title, options));
});

self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const url = (event.notification.data && event.notification.data.url) || "/";
  // Focus an existing dashboard tab if one is open, otherwise open one.
  event.waitUntil(
    clients.matchAll({ type: "window", includeUncontrolled: true }).then((wins) => {
      for (const w of wins) {
        if ("focus" in w) return w.focus();
      }
      if (clients.openWindow) return clients.openWindow(url);
    })
  );
});
