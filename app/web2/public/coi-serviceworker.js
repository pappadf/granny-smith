// Service worker that adds Cross-Origin isolation headers required for
// SharedArrayBuffer (used by Emscripten pthreads / PROXY_TO_PTHREAD).
//
// GitHub Pages (and many other static hosts) do not allow setting custom
// HTTP headers.  This service worker intercepts every response and injects
// the two headers the browser needs to enable cross-origin isolation:
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: credentialless (or require-corp)
//
// "credentialless" is used where the browser supports it, so that
// cross-origin resources (e.g. CDN scripts) work without needing an explicit
// Cross-Origin-Resource-Policy header on the remote server.  Safari does not
// support it: a page served with it is never cross-origin isolated there, so
// it has no SharedArrayBuffer and the emulator's thread never starts.  The
// page says which policy to use in the registration URL (?coep=require-corp,
// see index.html); under require-corp every response this worker passes on
// also carries Cross-Origin-Resource-Policy: cross-origin.  (Cross-origin
// fetch() of media is CORS, which require-corp accepts as it is.)
//
// Based on the coi-serviceworker pattern:
//   https://github.com/nicolo-ribaudo/coi-serviceworker

/*global self, caches, Response, clients*/

const COEP =
  new URL(self.location.href).searchParams.get("coep") === "require-corp"
    ? "require-corp"
    : "credentialless";

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
        // Opaque or redirect responses have status 0 which is outside the
        // valid range for the Response constructor.  Return them unchanged
        // (e.g. Codespace auth redirects).
        if (response.status === 0) {
          return response;
        }

        // Clone so we can modify headers
        const newHeaders = new Headers(response.headers);
        newHeaders.set("Cross-Origin-Embedder-Policy", COEP);
        newHeaders.set("Cross-Origin-Opener-Policy", "same-origin");
        if (COEP === "require-corp") {
          newHeaders.set("Cross-Origin-Resource-Policy", "cross-origin");
        }

        return new Response(response.body, {
          status: response.status,
          statusText: response.statusText,
          headers: newHeaders,
        });
      })
      .catch((e) => new Response("Service worker fetch failed: " + e.message,
        { status: 502, headers: { "Content-Type": "text/plain" } }))
  );
});
