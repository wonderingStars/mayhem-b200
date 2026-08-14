# mayhem-b200 usage counter (Cloudflare Worker)

Counts how many distinct installs of mayhem-b200 are in use. The desktop app
sends a random **anonymous install id** (which it generates and stores locally)
once a day; this Worker records one Analytics Engine data point per ping, and
"how many users" is `count(DISTINCT install_id)` over a time window.

## Privacy

The only identifier is a client-generated random id — it maps to no person,
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

First deploy needs **Analytics Engine enabled** on the account once (a one-time
toggle at `dash.cloudflare.com/<account>/workers/analytics-engine`); after that
the dataset (`mayhem_b200_usage`) is created automatically on first write.

`wrangler deploy` prints the Worker URL, e.g.
`https://mayhem-b200-usage.<your-subdomain>.workers.dev`. Put `<that-url>/ping`
into the app's telemetry endpoint: the single constant `kTelemetryEndpoint` in
`src/core/telemetry.cpp` of the main project. A stock build ships with it empty
and therefore sends nothing until you set it.

> Note: newly-deployed `*.workers.dev` routes can take a minute to start
> answering (a fresh route returns Cloudflare error 1042 until it propagates).

## The app's ping

```
POST https://<worker>/ping
{ "id": "<random-id>", "version": "0.18.0", "os": "windows" }
-> { "ok": true, "counted": true }
```

## Quick stats (`stats.ps1`)

On Windows, `stats.ps1` prints the whole picture in one command — distinct
installs, ping volume, and breakdowns by version, OS and day:

```powershell
pwsh ./stats.ps1            # last 30 days
pwsh ./stats.ps1 -Days 7
```

It reads a Cloudflare API token from `$env:CLOUDFLARE_API_TOKEN`, or failing that
from `$HOME\.mayhem-b200-cf-token`. The token needs only **Account Analytics:
Read** (add **Workers Scripts: Edit** if you also want it to `wrangler deploy`).
No token is stored in this repo.

## Querying "how many users" (raw)

Create an API token with **Account Analytics: Read**, then:

```bash
ACCOUNT_ID=<your account id>
TOKEN=<api token>

# Distinct installs seen in the last 30 days (this is the user count):
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY"

# Broken down by app version:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT blob1 AS version, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY GROUP BY version ORDER BY users DESC"

# By OS:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT blob2 AS os, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY GROUP BY os"

# Daily active installs, last 14 days:
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT toStartOfInterval(timestamp, INTERVAL '1' DAY) AS day, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '14' DAY GROUP BY day ORDER BY day"

# Total ping volume (not distinct users):
curl -s "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -d "SELECT SUM(double1) AS pings FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '30' DAY"
```

`count(DISTINCT index1)` is the exact distinct-install count — note it is
`count(DISTINCT ...)`, **not** `uniq()` (Analytics Engine's SQL rejects `uniq`).
Analytics Engine only samples writes at very high volume (far above a hobby SDR
app), so the counts are exact in practice. `SUM(double1)` gives total pings if
you want raw volume rather than distinct users.

All five queries above were run against the live dataset and return HTTP 200.
