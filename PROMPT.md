# Prompt: build a server for the Printer Inventory Agent

Copy everything below the line into an AI coding assistant to have it build
a backend that correctly speaks to the existing Windows agent in this repo.
It's self-contained — the assistant doesn't need to read the agent's C++
source, only this document.

---

You are building the **server side** for a fleet of Windows agents called
"Printer Inventory Agent". Each agent runs as a Windows service on one
machine, periodically inventories the printers installed on that machine,
and reports them to you. You do not control or modify the agent — build the
server to match its behavior exactly as specified below.

## 1. HTTP report ingest endpoint

Implement `POST /api/printer-reports` (path is configurable on the agent
side, but use this by default).

**Request**

- `Content-Type: application/json`
- `Authorization: Bearer <token>` header, **only present if an operator
  configured a token on that agent** — treat it as optional per-agent, not
  guaranteed. Do not require it unless you also document that operators
  must set `auth_token` in `agent.ini`.
- Body: one JSON object per request, this exact shape:

```jsonc
{
  "hostname": "PRINT-SRV01",                // string, the reporting machine's name
  "collectedAtUtc": "2026-08-25T13:50:12.345Z", // ISO-8601 UTC, millisecond precision, always "Z" suffix
  "hostIpAddresses": [
    { "ip": "10.0.1.15", "isIPv6": false, "adapter": "Ethernet" }
    // zero or more entries; can be empty array
  ],
  "printers": [
    {
      "name": "Kyocera ECOSYS M2235dn",     // string, WMI printer queue name - use as the printer's display/unique key together with hostname
      "driverName": "Kyocera ECOSYS M2235dn KX",
      "portName": "IP_10.0.1.42",
      "portType": "TCP/IP",                 // one of: "TCP/IP" | "WSD" | "Local/USB" | "Other"
      "location": "3rd floor",              // string, may be empty
      "shared": false,
      "shareName": "",                      // string, may be empty
      "network": true,
      "workOffline": false,
      "printerStatus": 3,                   // integer, raw WMI Win32_Printer.PrinterStatus value
      "resolvedHost": "10.0.1.42",          // string OR JSON null - null means "unknown/unresolvable"
      "isIPv6": false,                      // ONLY present in the object when resolvedHost is not null - do not assume it's always present
      "pageCount": 48213,                   // integer OR JSON null - null means "no counter available"
      "pageCountSource": "snmp",            // one of: "none" | "snmp" | "registry:PrinterDriverData"
      "snmpAttempted": true,
      "snmpReachable": true,
      "snmpSysDescr": "KYOCERA ECOSYS M2235dn", // OPTIONAL key - omitted entirely (not null, not "") when empty
      "note": "registry value used: PageCount"  // OPTIONAL key - omitted entirely when empty; free-form diagnostic text, not for parsing/matching
    }
    // zero or more entries; can be empty array (e.g. a WMI failure that still sent a partial/empty report)
  ]
}
```

Important parsing notes:

- `printers` can legitimately contain **two entries for the same physical
  printer** (e.g. installed once via WSD and once via a direct TCP/IP port).
  The agent does not deduplicate — **you must**, if your data model wants
  one row per physical device. A reasonable dedup key is
  `(hostname, resolvedHost)` when `resolvedHost` is non-null, falling back
  to `(hostname, name)` otherwise.
- `snmpSysDescr` and `note` are **optional keys**, not nullable keys —
  check for key presence, don't assume `null`/`""`.
- `isIPv6` on a printer object is only present when `resolvedHost` is
  non-null. Don't fail parsing if it's absent.
- `pageCount` being `null` is normal and common (USB printers with no
  vendor counter data, unreachable network printers, etc.) — do not treat
  it as an error.
- Treat unrecognized additional JSON keys as forward-compatible and ignore
  them rather than rejecting the request (the agent may add fields later).

**Response**

- Any `2xx` status = success, from the agent's point of view. It reads the
  status code only; it does not require or parse a particular response
  body shape, but returning a small JSON acknowledgement is good practice,
  e.g. `{"status":"ok"}`.
- Any non-`2xx` is logged by the agent as a failure (with up to a 512-byte
  preview of your response body for diagnostics) and the report is
  **dropped**, not retried until the next scheduled cycle. If you want
  reliable delivery, the agent is not the place to add retry logic — retry
  responsibility sits entirely with you (e.g. respond `5xx` and the agent
  will simply try again on its own regular interval; there is no
  agent-side backoff/retry-on-failure within a single cycle).
- Reports arrive roughly once per `interval_seconds` (operator-configured,
  default 3600s/1h) per agent, plus possibly an extra out-of-cycle report
  shortly after you push a "send_report" signal over the WebSocket channel
  (see below). Don't assume a fixed schedule across agents — they are not
  synchronized with each other.

## 2. WebSocket control channel (optional push trigger)

Implement a WebSocket endpoint, e.g. `wss://your-server/api/agent-ws`, that
agents connect to **as clients** (you are the WS server). This channel
exists for exactly one purpose: letting you ask an agent to report
**right now** instead of waiting for its next scheduled interval. It is
**not** used to transmit the report itself — that always goes through the
HTTP endpoint above, as a separate follow-up request a few seconds to a
couple of minutes later.

- **Upgrade request**: may carry `Authorization: Bearer <token>` (same
  token/value as the HTTP endpoint, if the operator configured one).
  Accept the upgrade whether or not a token is present unless you've
  decided to enforce auth — that's your call, just don't require a header
  the agent isn't guaranteed to send.
- **On connect**, the agent sends one text frame identifying itself:
  ```json
  {"type": "hello", "hostname": "PRINT-SRV01"}
  ```
  Use this to map the connection to a known host in your system (match on
  `hostname` against what that agent has reported via HTTP previously; if
  you've never seen this hostname before, still keep the connection —
  don't close it just because the host is new).
- **To trigger an immediate report**, send the agent a text frame, either
  form:
  ```json
  {"type": "send_report"}
  ```
  (`"type"` also accepts `"report_now"` or `"trigger_report"`), or simply
  the bare word `send_report` (also accepts `report_now`, `trigger_report`,
  `report`) as the entire message body with no JSON wrapper at all. Support
  both so the channel is easy to test by hand with something like `wscat`.
- The agent does not send anything back over the socket in response to a
  trigger (no ack frame) — watch for the follow-up HTTP POST from that
  host instead to confirm it worked.
- **Ping/pong**: the agent's WinHTTP-based client auto-responds to ping
  control frames at the protocol level; you don't need special handling
  beyond standard WebSocket ping/pong, and a plain idle connection (no
  pings at all) also works since the agent's own receive loop just blocks
  waiting for data.
- **Reconnection**: if the connection drops for any reason, the agent
  reconnects on its own with exponential backoff (starts around 5s,
  doubles up to a 5-minute cap, resets after a connection stays up 30+
  seconds). You do not need to implement any reconnect logic on your side
  — just accept new connections as they come in and treat each new `hello`
  as (possibly) the same host reconnecting.
- **Not every agent will connect here.** This channel is opt-in per agent
  (an operator must set `[websocket] enabled=true` and a valid `url` in
  that agent's local config). Your system must work correctly for hosts
  that only ever use the plain HTTP endpoint and never open a WebSocket at
  all — treat the control channel as a pure optimization/convenience, never
  a dependency.
- **Windows 7 caveat**: some agents run on Windows 7, which cannot use this
  channel at all (missing OS support) — again, don't assume every fleet
  member will connect here.

## 3. Suggested (not mandated) data model

- `hosts` — one row per `hostname` seen, last-seen timestamp, last-known IP
  list.
- `printers` — one row per physical printer, deduplicated per the note in
  section 1, with a foreign key to `hosts`.
- `printer_report_snapshots` (or similar) — append-only history of
  `pageCount` readings over time per printer, so page-count deltas /
  trends can be computed later; don't just overwrite the latest value if
  historical reporting matters to you.
- `agent_connections` (if you implement the WebSocket channel) — tracks
  which hosts currently have an open control-channel connection, so a
  "push report now" UI action can know which hosts it can actually reach
  immediately vs. which will only pick it up on their next poll.

## 4. What to build

Pick a stack you're comfortable with (a small Node/Express, Python/FastAPI,
or similar service is plenty for this). Deliver:

1. The `POST /api/printer-reports` handler with the exact parsing rules
   from section 1 (validate shape defensively — this is an unauthenticated
   or weakly-authenticated endpoint reachable from print-server machines,
   don't trust field lengths/types blindly).
2. The WebSocket endpoint from section 2, plus some way (admin UI, CLI, or
   just an internal function) to send a `send_report` trigger to a
   specific connected host by hostname.
3. Basic persistence per the data model in section 3 (or your own
   reasonable equivalent).
4. A minimal way to view current printer inventory + page counts per host
   (a simple list endpoint/page is enough — this doesn't need to be
   polished).

Ask me for the target stack/framework and hosting environment before
writing code if I haven't already told you.
