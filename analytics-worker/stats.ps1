# mayhem-b200 usage numbers.
#
# Prints distinct installs (users), ping volume, and breakdowns by version / OS
# / day from the Analytics Engine dataset written by the usage Worker.
#
# Auth: a Cloudflare API token with **Account Analytics: Read**. It is read from
# $env:CLOUDFLARE_API_TOKEN, else from -TokenFile (default
# $HOME\.mayhem-b200-cf-token). NO token is stored in this repo.
#
# Usage:
#   pwsh ./stats.ps1            # last 30 days
#   pwsh ./stats.ps1 -Days 7
#
# SPDX-License-Identifier: GPL-2.0-or-later
param(
  [int]$Days = 30,
  [string]$TokenFile = (Join-Path $HOME ".mayhem-b200-cf-token"),
  [string]$AccountId = "49a63dd34bc2aca87929d3ca215fa5f2"
)
$ErrorActionPreference = "Stop"

$token = $env:CLOUDFLARE_API_TOKEN
if ([string]::IsNullOrWhiteSpace($token) -and (Test-Path $TokenFile)) {
  $token = (Get-Content $TokenFile -Raw).Trim()
}
if ([string]::IsNullOrWhiteSpace($token)) {
  Write-Error "No API token. Set `$env:CLOUDFLARE_API_TOKEN or create $TokenFile"
  exit 1
}

$api = "https://api.cloudflare.com/client/v4/accounts/$AccountId/analytics_engine/sql"
function Invoke-AE([string]$sql) {
  Invoke-RestMethod -Uri $api -Method Post -ContentType "text/plain" `
    -Headers @{ Authorization = "Bearer $token" } -Body $sql
}
$W = "timestamp >= NOW() - INTERVAL '$Days' DAY"

Write-Host ""
Write-Host "mayhem-b200 usage - last $Days days" -ForegroundColor Cyan
Write-Host ("-" * 40)

$tot = (Invoke-AE "SELECT count(DISTINCT index1) AS users, SUM(double1) AS pings FROM mayhem_b200_usage WHERE $W").data[0]
Write-Host ("Distinct installs (users): {0}" -f $tot.users)
Write-Host ("Total pings:               {0}" -f $tot.pings)

Write-Host "`nBy version:"
(Invoke-AE "SELECT blob1 AS version, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE $W GROUP BY version ORDER BY users DESC").data |
  ForEach-Object { Write-Host ("  {0,-12} {1}" -f $_.version, $_.users) }

Write-Host "`nBy OS:"
(Invoke-AE "SELECT blob2 AS os, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE $W GROUP BY os ORDER BY users DESC").data |
  ForEach-Object { Write-Host ("  {0,-12} {1}" -f $_.os, $_.users) }

Write-Host "`nDaily active (last 14 days):"
(Invoke-AE "SELECT toStartOfInterval(timestamp, INTERVAL '1' DAY) AS day, count(DISTINCT index1) AS users FROM mayhem_b200_usage WHERE timestamp >= NOW() - INTERVAL '14' DAY GROUP BY day ORDER BY day").data |
  ForEach-Object { Write-Host ("  {0}  {1}" -f ($_.day -replace ' 00:00:00',''), $_.users) }
Write-Host ""
