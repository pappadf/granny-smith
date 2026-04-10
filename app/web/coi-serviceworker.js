// Service worker that adds Cross-Origin isolation headers required for
// SharedArrayBuffer (used by Emscripten pthreads / PROXY_TO_PTHREAD).
//
// GitHub Pages (and many other static hosts) do not allow setting custom
// HTTP headers.  This service worker intercepts every response and injects
// the two headers the browser needs to enable cross-origin isolation:
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: credentialless
//
// "credentialless" is used instead of "require-corp" so that cross-origin
// resources (e.g. CDN scripts) work without needing an explicit
// Cross-Origin-Resource-Policy header on the remote server.
//
// Based on the coi-serviceworker pattern:
//   https://github.com/nicolo-ribaudo/coi-serviceworker

/*global self, caches, Response, clients*/

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (e) => {
  // Only handle navigation and same-origin requests
  if (e.request.cache === "only-if-cached" && e.request.mode !== "same-origin") {
    return;
  }

  e.respondWith(
    fetch(e.request)
      .then((response) => {
        // Clone so we can modify headers
        const newHeaders = new Headers(response.headers);
        newHeaders.set("Cross-Origin-Embedder-Policy", "credentialless");
        newHeaders.set("Cross-Origin-Opener-Policy", "same-origin");

        return new Response(response.body, {
          status: response.status,
          statusText: response.statusText,
          headers: newHeaders,
        });
      })
  );
});
