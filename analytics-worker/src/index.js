// SPDX-License-Identifier: GPL-2.0-or-later
//
// mayhem-b200 usage counter — a Cloudflare Worker that counts distinct
// anonymous installs. The desktop app POSTs a random install id (that IT
// generated and stores locally) once a day; this records one Analytics Engine
// data point per ping. "How many users" is then uniq(install_id) over a time
// window — see README.md for the queries.
//
// PRIVACY, by construction:
//   - The only identifier is a client-generated random UUID. It maps to no
//     person, account, email, hostname or location.
//   - This Worker stores NOTHING from the request beyond what the app sends
//     (install id, app version, OS name). It does not read or log the client
//     IP, and Analytics Engine is not given one.
//   - There are no cookies, no fingerprinting, no cross-site anything.
// The app discloses the ping in its README and at startup and lets the user
// turn it off (--no-telemetry). This endpoint is the honest other half of that.

const MAX_ID_LEN = 64;
const MAX_FIELD_LEN = 40;

// Keep only sane, bounded ASCII — never echo arbitrary client bytes into the
// dataset. An id that fails this is dropped, not stored.
function clean(s, max) {
  if (typeof s !== "string") return "";
  const t = s.trim().slice(0, max);
  return /^[A-Za-z0-9._:+-]*$/.test(t) ? t : "";
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/") {
      return new Response("mayhem-b200 usage counter. POST /ping {id,version,os}.\n", {
        headers: { "content-type": "text/plain" },
      });
    }

    if (request.method !== "POST" || url.pathname !== "/ping") {
      return new Response("not found", { status: 404 });
    }

    let body;
    try {
      body = await request.json();
    } catch {
      return new Response(JSON.stringify({ ok: false, error: "bad json" }), {
        status: 400,
        headers: { "content-type": "application/json" },
      });
    }

    const id = clean(body && body.id, MAX_ID_LEN);
    if (!id) {
      // No usable id → nothing to count. Not an error the app should retry.
      return new Response(JSON.stringify({ ok: true, counted: false }), {
        headers: { "content-type": "application/json" },
      });
    }
    const version = clean(body && body.version, MAX_FIELD_LEN) || "unknown";
    const os = clean(body && body.os, MAX_FIELD_LEN) || "unknown";

    // index1 = the high-cardinality install id (millions of distinct values are
    // fine here, and uniq(index1) is the user count). Blobs are the low-
    // cardinality breakdowns. double1 = 1 so SUM(double1) is total pings.
    env.USAGE.writeDataPoint({
      indexes: [id],
      blobs: [version, os],
      doubles: [1],
    });

    return new Response(JSON.stringify({ ok: true, counted: true }), {
      headers: { "content-type": "application/json" },
    });
  },
};
