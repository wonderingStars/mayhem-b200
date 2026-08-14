# mayhem-b200 usage counter (Cloudflare Worker)

Counts how many distinct installs of mayhem-b200 are in use. The desktop app
sends a random **anonymous install id** (which it generates and stores locally)
once a day; this Worker records one Analytics Engine data point per ping, and
"how many users" is `uniq(install_id)` over a time window.

## Privacy

The only identifier is a client-generated random UUID — it maps to no person,
account, email, hostname or location. The Worker stores nothing from the request
beyond what the app sends: the install id, the app version, and the OS name. It
does not read or log the client IP. The app discloses this at startup and in its
own README, and it can be turned off with `--no-telemetry`. This is opt-out,
anonymous, aggregate usage counting — nothing more.

## Deploy (to your own Cloudflare account)

```bash
cd analytics-worker
npm install            # installs wrangler
npx wrangler login     # first time only — opens a browser
npx wrangler deploy
```

`wrangler deploy` prints the Worker URL, e.g.
`https://mayhem-b200-usage.<your-subdomain>.workers.dev`. Put `<that-url>/ping`
into the app's telemetry endpoint (see `src/telemetry.cpp` in the main project;
the URL is a single constant `kTelemetryEndpoint`).

Analytics Engine needs no separate provisioning — the dataset
(`mayhem_b200_usage`) is created on first write. It is included on the Workers
free plan (writes) with the SQL query API on paid; check the current Cloudflare
pricing page.

## The app's ping

```
POST https://<worker>/ping
{ "id": "<random-uuid>", "version": "0.17.1", "os": "windows" }
-> { "ok": true, "counted": true }
```

## Querying "how many users"

Create an API token with **Account Analytics: Read**, then:

```bash
ACCOUNT_ID=<your account id>
TOKEN=<api token>

# Distinct installs seen in the last 30 days (this is the user count):
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT uniq(index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY"

# Broken down by app version:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT blob1 AS version, uniq(index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY GROUP BY version ORDER BY users DESC"

# By OS:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT blob2 AS os, uniq(index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY GROUP BY os"

# Daily active installs, last 14 days:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT toStartOfInterval(timestamp, INTERVAL '1' DAY) AS day, uniq(index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '14' DAY GROUP BY day ORDER BY day"
```

`uniq()` is approximate but accurate to well under a percent at this scale, and
Analytics Engine only samples writes at very high volume (far above a hobby
SDR app), so the counts are exact in practice. `SUM(double1)` gives total pings
if you want raw volume rather than distinct users.
