# Printer Inventory Agent

A Windows service that inventories installed printers on a machine, reads
their lifetime page counts (SNMP for network printers; PJL/WMI-spooler/
registry fallback tiers for local/USB ones - see
[Page count sources](#page-count-sources)), tracks each printer's online/
offline reachability across cycles, and reports everything to a central
server as JSON. Targets **Windows 7 SP1 and later** (x86 and x64), runs
unattended under the Service Control Manager, and never depends on a
logged-in user.

## Contents

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Building](#building)
  - [In CLion](#in-clion)
  - [From the command line](#from-the-command-line)
  - [`scripts\build.bat`](#scriptsbuildbat)
- [Configuration (`agent.ini`)](#configuration-agentini)
- [Running](#running)
  - [Console mode (development)](#console-mode-development)
  - [As a Windows service](#as-a-windows-service)
  - [`scripts\install_service.bat`](#scriptsinstall_servicebat)
  - [`scripts\uninstall_service.bat`](#scriptsuninstall_servicebat)
- [Report payload](#report-payload)
- [Page count sources](#page-count-sources)
- [Printer status and removed-printer detection](#printer-status-and-removed-printer-detection)
- [WebSocket control channel](#websocket-control-channel)
- [Checking logs](#checking-logs)
- [Known limitations](#known-limitations)
- [Project layout](#project-layout)

## How it works

On each collection cycle the agent:

1. Enumerates installed printers via WMI (`Win32_Printer`), skipping virtual
   printers (Microsoft Print to PDF, XPS, OneNote, Fax, redirected/session
   printers, etc.).
2. Classifies each printer's port: **TCP/IP** (`Win32_TCPIPPrinterPort`),
   **WSD** (host/IP parsed out of the WMI `Location` field, including IPv6
   link-local + zone id), **Local/USB** (`USB*`/`COM*`/`LPT*`), or **Other**.
3. Reads the lifetime page count:
   - TCP/IP and WSD printers: a hand-rolled **SNMP v1 GET** over WinSock2
     against `prtMarkerLifeCount` (`1.3.6.1.2.1.43.10.2.1.4.1.1`), with
     retries and IPv4/IPv6 support.
   - Local/USB printers: best-effort scan of
     `HKLM\SYSTEM\CurrentControlSet\Control\Print\Printers\<name>\PrinterDriverData`
     for a vendor-specific counter value. This is unstructured per-vendor
     data, so it's a heuristic, not a guarantee.
4. Determines the host's own IP addresses.
5. Sends everything as one JSON document via HTTP(S) POST (WinHTTP) to the
   configured server URL, with an optional bearer token.

One bad printer, an unreachable SNMP target, or a failed HTTP POST never
crashes the agent or aborts the rest of the cycle — see
[Known limitations](#known-limitations) for what "best-effort" means in
practice.

## Requirements

- **MSVC only** (the project enforces this in `CMakeLists.txt`) — Visual
  Studio 2022 Build Tools with the "Desktop development with C++" /
  `Microsoft.VisualStudio.Workload.VCTools` workload. MinGW/g++ cannot build
  this project (no wide-char `wmain` entry point support in the way it's
  used here, and several Win32 APIs used are MSVC-header-only).
- CMake 3.20+ (CLion's bundled CMake works fine).
- Windows 10/11 SDK (any recent version; `10.0.26100.0` is confirmed
  working).
- CLion 2023.x+ if building from the IDE, with a toolchain of type **MSVC**
  configured under *Settings → Build, Execution, Deployment → Toolchains*.

The build always uses the **static CRT** (`/MT` / `/MTd`) so the built
`.exe` has no dependency on the VC++ Redistributable being installed on the
target machine.

## Building

### In CLion

1. *Settings → Build, Execution, Deployment → Toolchains*: make sure an
   **MSVC** toolchain is configured (auto-detected as "Visual Studio" once
   VS Build Tools is installed) and that its Windows SDK version points at
   one that's actually installed (check `C:\Program Files (x86)\Windows
   Kits\10\Include` for the real installed version — CLion sometimes
   auto-picks a stub SDK version that has no `Include`/`Lib` content).
2. *Settings → Build, Execution, Deployment → CMake*: profiles `Debug` and
   `Release` should both use that MSVC toolchain with generator `-G Ninja`
   (already wired up in this project's `.idea/workspace.xml`).
3. Select the **`printer_agent`** run/debug configuration from the
   configuration dropdown (**not** "C/C++ File" — that's CLion's single-file
   MinGW quick-launcher, which ignores every other `.cpp` in the project and
   will fail to link).
4. Build (Ctrl+F9) or Run.

### From the command line

From a **Developer Command Prompt for VS 2022** (or after calling
`vcvarsall.bat x64`):

```bash
cmake -G Ninja -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug --target printer_agent
```

The build copies `config/agent.ini` next to the built `.exe` automatically.

### `scripts\build.bat`

A standalone build script that doesn't need CLion open — just the same MSVC
toolchain (Visual Studio 2022 Build Tools + the C++ workload):

```powershell
scripts\build.bat            # Release build, into cmake-build-release\
scripts\build.bat Debug      # Debug build, into cmake-build-debug\
```

It auto-locates `cmake.exe`/`ninja.exe` (PATH first, falling back to
CLion's bundled copies) and Visual Studio via `vswhere.exe`, then configures
and builds with the same generator and output directories CLion itself
uses, so the two stay in sync. On success, `printer_agent.exe`, `agent.ini`,
and `install_service.bat` all end up together in the build directory —
that whole folder is what you copy to a target machine.

## Configuration (`agent.ini`)

Read from `agent.ini` next to `printer_agent.exe`. Every field has a
built-in default, so the service still runs (in a degraded/logging-only
fashion) even if the file is missing or partial. The file is **read once at
startup**, not hot-reloaded while running — restart the service after
editing it.

```ini
[server]
url=http://your-server.local:8080/api/printer-reports
auth_token=
timeout_ms=15000

[polling]
interval_seconds=3600
offline_after_consecutive_failures=3

[websocket]
enabled=false
url=wss://your-server.local:8080/api/agent-ws
retry_interval_ms=2000
max_retries_before_relax=15
relax_delay_ms=60000

[snmp]
community=public
timeout_ms=2000
retries=3
retry_delay_ms=500
port=161

[pjl]
enabled=false
timeout_ms=5000

[logging]
level=INFO
file=agent.log
max_size_kb=5120
max_files=5
```

| Section       | Key                   | Meaning                                                                 |
|---------------|------------------------|--------------------------------------------------------------------------|
| `[server]`    | `url`                  | HTTP(S) endpoint the JSON report is POSTed to.                          |
|               | `auth_token`           | Sent as `Authorization: Bearer <token>` when non-empty.                 |
|               | `timeout_ms`           | HTTP request timeout.                                                   |
| `[polling]`   | `interval_seconds`     | Seconds between collection+send cycles (floor 10s, cap 30 days).        |
|               | `offline_after_consecutive_failures` | A printer only flips to `"offline"` in reports after this many bad cycles in a row (default 3) - a single blip doesn't count. |
| `[websocket]` | `enabled`              | Turns on the optional push/control channel (see below).                 |
|               | `url`                  | `ws://` or `wss://` URL of the control channel.                         |
|               | `retry_interval_ms`    | Delay between reconnect attempts while under the failure limit below.   |
|               | `max_retries_before_relax` | Consecutive failed attempts before backing off to `relax_delay_ms`. |
|               | `relax_delay_ms`       | Wait time after hitting the limit, then the fast retries resume.        |
| `[snmp]`      | `community`            | SNMP v1 community string.                                               |
|               | `timeout_ms`           | Per-attempt SNMP timeout.                                               |
|               | `retries`               | Total attempts per OID (not extra retries on top of one).              |
|               | `retry_delay_ms`       | Delay between attempts.                                                 |
|               | `port`                 | SNMP UDP port (default 161).                                            |
| `[pjl]`       | `enabled`              | **Off by default.** Turns on a PJL status query for local/USB printers - see [Page count sources](#page-count-sources) before enabling. |
|               | `timeout_ms`           | How long to wait for the printer's reply before giving up.              |
| `[logging]`   | `level`                | `DEBUG`, `INFO`, `WARN`, or `ERROR`.                                    |
|               | `file`                 | Log file path; relative paths resolve next to the executable.           |
|               | `max_size_kb`          | Rotate once the active log file would exceed this size.                 |
|               | `max_files`             | How many rotated backups (`agent.1.log` …) to keep.                    |

## Running

### Console mode (development)

```powershell
.\printer_agent.exe --console
```

Runs the same worker loop directly in the console; Ctrl+C stops it cleanly.
Useful for iterating without installing the service each time.

### As a Windows service

Install (requires an **elevated/Administrator** prompt):

```powershell
.\printer_agent.exe --install
```

This registers the service for auto-start and configures automatic restart
on crash. Then control it with `sc.exe` (note the explicit `.exe` — in
**PowerShell**, the bare word `sc` is a built-in alias for `Set-Content`,
not `sc.exe`, and will silently do the wrong thing) or the PowerShell
service cmdlets:

```powershell
sc.exe start PrinterInventoryAgent
sc.exe query PrinterInventoryAgent
sc.exe stop PrinterInventoryAgent
```

```powershell
Start-Service PrinterInventoryAgent
Get-Service PrinterInventoryAgent
Stop-Service PrinterInventoryAgent
```

Tail the log while it runs (see [Checking logs](#checking-logs) for more):

```powershell
Get-Content .\agent.log -Tail 20 -Wait
```

Uninstall:

```powershell
.\printer_agent.exe --uninstall
```

`printer_agent.exe --help` lists all command-line options.

### `scripts\install_service.bat`

A one-click install: copy this file into the same folder as
`printer_agent.exe` and `agent.ini` (that's automatic if you built with
`scripts\build.bat`) and run it. It:

1. Relaunches itself elevated if it isn't already (UAC prompt).
2. Runs `printer_agent.exe --install`.
3. Starts the service if it isn't already running.
4. Prints `sc.exe query` status and the log file location.

This is the file to hand to whoever sets the agent up on a target machine —
they don't need CLion, the source, or to know the `sc.exe`-vs-PowerShell-
alias gotcha above.

### `scripts\uninstall_service.bat`

The reverse: run it (from anywhere — it's also copied next to
`printer_agent.exe` automatically by `scripts\build.bat`, but doesn't need
to be). It elevates itself the same way, stops the service if it's running,
waits for it to actually stop, then removes the service registration with
`sc.exe delete`. Unlike `install_service.bat`, it does **not** need
`printer_agent.exe` to be present or colocated — it only talks to the
Windows Service Control Manager by service name, so it keeps working even if
the exe was moved, rebuilt elsewhere, or already deleted. It leaves whatever
`printer_agent.exe`, `agent.ini`, and `agent.log` exist in place — delete
them yourself if you want those gone too.

## Report payload

One JSON object per collection cycle, POSTed as `application/json`:

```jsonc
{
  "hostname": "PRINT-SRV01",
  "collectedAtUtc": "2026-08-25T13:50:12.345Z",
  "hostIpAddresses": [
    { "ip": "10.0.1.15", "isIPv6": false, "adapter": "Ethernet" }
  ],
  "printers": [
    {
      "name": "Kyocera ECOSYS M2235dn",
      "driverName": "Kyocera ECOSYS M2235dn KX",
      "portName": "IP_10.0.1.42",
      "portType": "TCP/IP",           // "TCP/IP" | "WSD" | "Local/USB" | "Other"
      "location": "3rd floor",
      "shared": false,
      "shareName": "",
      "network": true,
      "workOffline": false,
      "printerStatus": 3,
      "resolvedHost": "10.0.1.42",    // null if not applicable/resolvable
      "isIPv6": false,                // only present when resolvedHost isn't null
      "pageCount": 48213,             // null if unknown
      "pageCountSource": "snmp",      // "none" | "snmp" | "registry:PrinterDriverData" | "wmi:PrintQueue.TotalPagesPrinted" | "pjl:INFO_PAGECOUNT"
      "snmpAttempted": true,
      "snmpReachable": true,
      "snmpSysDescr": "KYOCERA ECOSYS M2235dn",  // omitted when empty
      "status": "online",             // "online" | "offline" | "unknown" - see below
      "consecutiveFailures": 0,
      "note": ""                                  // omitted when empty
    }
  ],
  "removedPrinters": [
    // present (possibly empty) every cycle; one entry per printer that was
    // enumerated in a previous cycle but is no longer installed in Windows
    // at all (not merely unreachable - actually uninstalled)
    { "dedupKey": "10.0.1.42", "name": "Kyocera ECOSYS M2235dn" }
  ]
}
```

See [`PROMPT.md`](PROMPT.md) for a ready-to-use prompt that has an AI build
a server matching this exact schema.

## Page count sources

For network printers (TCP/IP, WSD), the agent tries SNMP's standard
`prtMarkerLifeCount` OID at its usual table index first, then falls back to
walking the whole table with GETNEXT for devices that index their marker(s)
differently (e.g. per-color-plane counters).

For local/USB printers, in priority order:

1. **PJL status query** (`pjl:INFO_PAGECOUNT`) - opt-in, see `[pjl]` above.
   Sends a tiny raw PJL command directly to the device and reads its own
   reply, so it reflects the printer's true lifetime page count from its own
   firmware/NVRAM - the only source here that survives a Print Spooler
   restart or a driver reinstall. **This works by submitting an actual print
   job.** A printer that doesn't understand PJL may print a garbled or blank
   page in response instead of silently ignoring it, which is why this is
   off by default: only enable it after testing one cycle and physically
   confirming your printer handles it safely.
2. **WMI spooler counter** (`wmi:PrintQueue.TotalPagesPrinted`) - works for
   any local queue with no vendor cooperation needed, but resets to 0
   whenever the Print Spooler service restarts, so it's "pages since the
   spooler last started," not a lifetime count.
3. **Registry heuristic** (`registry:PrinterDriverData`) - vendor-specific
   and undocumented; used only if the two sources above found nothing.

## Printer status and removed-printer detection

Each printer's `status` in the report reflects reachability *across several
cycles*, not just the current one - a single SNMP timeout or a momentarily
offline USB queue doesn't flip it to `"offline"` by itself. Only after
`offline_after_consecutive_failures` (default 3) consecutive bad cycles does
it flip; any single successful cycle resets the counter and flips it straight
back to `"online"`. `"unknown"` means the printer's port type has no
reachability probe at all (`PortType::Other`) - it never counts toward
offline detection either way. This state resets whenever the agent process
restarts (service restart, machine reboot) - it is not persisted to disk.

If a printer disappears from Windows' own printer list entirely between one
cycle and the next (uninstalled, not just unreachable), it stops appearing in
`printers` and instead gets one entry in `removedPrinters` on that cycle only
- the server already has everything else about that printer from its last
normal report.

## WebSocket control channel

`[websocket]` adds an optional, persistent, **one-way** control channel:
the agent connects out to the configured `ws://`/`wss://` URL and holds the
connection open. It's purely a "send a report right now" trigger — it does
not replace the HTTP report POST, and the agent never expects a report
request/response over the socket itself.

**Reconnecting:** on any drop, the agent retries every `retry_interval_ms`
(default 2s). After `max_retries_before_relax` consecutive failed attempts
(default 15, so ~30s of fast retries), it backs off for `relax_delay_ms`
(default 1 minute), then resets the counter and resumes the fast retries.
A connection that stays up 30+ seconds counts as a success and resets the
failure streak, so one later hiccup doesn't inherit a prior outage's count.

- On connect, the agent sends a small hello frame so the server can
  correlate the socket with a host before the first HTTP report arrives:
  ```json
  {"type": "hello", "hostname": "PRINT-SRV01"}
  ```
- To make the agent run an out-of-cycle collection immediately, send it a
  UTF-8 text frame that is either:
  ```json
  {"type": "send_report"}
  ```
  (also accepts `"report_now"` or `"trigger_report"` as the `type`), or
  just the bare word `send_report` (also accepts `report_now`,
  `trigger_report`, `report`) — handy for testing by hand with a tool like
  `wscat`.
- The agent then runs a normal collection cycle and POSTs the result to
  `[server] url` as usual, a few seconds to a couple of minutes later
  depending on printer count and SNMP timeouts — **not** over the
  WebSocket.
- `Authorization: Bearer <auth_token>` (same token as the HTTP report) is
  sent as a header on the WebSocket upgrade request when configured.

**Windows 7 caveat:** the WinHTTP WebSocket API was added in Windows 8.1.
On Windows 7, `winhttp.dll` doesn't export it; the agent detects this at
startup, logs a warning once, and simply runs without the control channel —
polling continues to work normally.

## Checking logs

Every run — console mode or the installed service — writes to the same
rotating log file: `agent.log` next to `printer_agent.exe` by default
(`[logging] file` in `agent.ini`; a relative path resolves next to the
executable, an absolute path is used as-is). Nothing else needs to be
running to read it — it's a plain text file.

Each line looks like:

```
[2026-08-25 14:36:13.753] [INFO] === Printer Inventory Agent starting ===
[2026-08-25 14:36:14.622] [INFO] Enumerated 4 printer(s) after filtering virtual printers.
[2026-08-25 14:36:26.451] [WARN] SNMP for 'Pantum P3010DW Series 0001': could not determine IP: DNS resolution failed for 'Pantum-4A6A29'
[2026-08-25 14:36:26.451] [ERROR] SNMP for 'NPI939951 (HP LaserJet CP1525nw)': connect() failed, WSAGetLastError=10051
[2026-08-25 14:36:54.690] [INFO] Collection cycle complete: sent report for 4 printer(s), server responded HTTP 200.
```

`[timestamp] [LEVEL] message` — local time, millisecond precision.
`WARN`/`ERROR` lines about one printer (e.g. an unreachable SNMP target)
don't stop the cycle; check the final `Collection cycle complete` /
`failed to send report` line to see whether the report as a whole made it
to the server.

### Commands

```powershell
# Watch it live (Ctrl+C to stop watching - doesn't stop the service)
Get-Content .\agent.log -Tail 20 -Wait

# Just the last 50 lines
Get-Content .\agent.log -Tail 50

# Only warnings/errors
Select-String -Path .\agent.log -Pattern '\[WARN\]|\[ERROR\]'

# Only the most recent collection cycle (from the last "starting" line to now)
Select-String -Path .\agent.log -Pattern 'Agent starting' | Select-Object -Last 1
```

If you don't know where the running service's working directory is, find
the installed exe path first:

```powershell
(Get-CimInstance Win32_Service -Filter "Name='PrinterInventoryAgent'").PathName
```

### Rotation

Once the active file would exceed `[logging] max_size_kb`, it's rotated:
`agent.log` → `agent.1.log`, the old `agent.1.log` → `agent.2.log`, and so
on up to `[logging] max_files` backups (the oldest beyond that is deleted).
If you're looking for something that isn't in `agent.log` anymore, check
`agent.1.log`, `agent.2.log`, etc. in the same folder.

### What to look for

| Log line contains…                                  | Meaning |
|-------------------------------------------------------|---------|
| `Agent starting`                                       | Service/console run began. |
| `Config: ... using built-in defaults`                  | `agent.ini` was missing/unreadable - check it's next to the `.exe` and correctly named. |
| `SNMP subsystem failed to initialize`                  | WinSock init failed - page counts for network printers will be unavailable this run. |
| `Enumerated N printer(s)`                              | WMI enumeration succeeded; `N` printers passed the virtual-printer filter. |
| `SNMP for '<name>': ...` (`WARN`/`ERROR`)              | Page-count lookup failed for one printer - see [Known limitations](#known-limitations); the rest of the cycle still runs. |
| `Page count for '<name>': source=... value=... status=...` | One line per printer per cycle - which source (if any) supplied the page count, and its current online/offline/unknown status. Grep-able across a whole fleet's logs. |
| `Printer removed: '<name>' ...`                        | That printer disappeared from Windows' own printer list since the last cycle (see [Printer status and removed-printer detection](#printer-status-and-removed-printer-detection)). |
| `Collection cycle complete: ... server responded HTTP 200` | The report was built and successfully POSTed - the normal, healthy outcome. |
| `Collection cycle: failed to send report: ...`         | The HTTP POST failed (bad URL, unreachable server, non-2xx status, wrong `auth_token`, etc.) - the report for that cycle was dropped, not retried until the next interval. |
| `WebSocket: connected to ...`                          | The optional control-channel connection is up. |
| `WebSocket: handshake failed` / `disconnected; retrying` | The control channel is down and retrying (see [WebSocket control channel](#websocket-control-channel)) - polling still works normally in the meantime. |
| `WebSocket: N failed attempts in a row; relaxing`      | Hit `max_retries_before_relax`; pausing for `relax_delay_ms` before resuming fast retries. |
| `'send report' signal received`                        | A server-pushed trigger arrived over the WebSocket; an out-of-cycle collection is starting now. |

Raise `[logging] level` to `DEBUG` (see [Configuration](#configuration-agentini))
for full per-printer/per-OID detail when diagnosing something that `INFO`
doesn't show enough of; restart the service afterward for it to take
effect, and consider setting it back to `INFO` once you're done — `DEBUG`
is noticeably more verbose.

## Known limitations

- **Local/USB page counts are best-effort.** `PrinterDriverData` registry
  contents are entirely vendor-specific and unstructured; the agent takes
  the first value whose name looks like a counter (contains "count" or
  "page"). This can be wrong or absent depending on the driver.
- **TLS 1.2 on Windows 7** requires the OS-level update (KB3140245) in
  addition to this agent's `WINHTTP_OPTION_SECURE_PROTOCOLS` opt-in — the
  agent can't add protocol support Schannel doesn't have.
- **WebSocket control channel requires Windows 8.1+** (see above); on
  Windows 7 the agent degrades gracefully to polling-only.
- **SNMP v1 only, no encryption/auth beyond the community string** — matches
  what most office printers still speak by default.
- Installing an identical physical printer twice (e.g. once via WSD, once
  via a direct TCP/IP port) intentionally produces two separate report
  entries; de-duplication is left to the server.

## Project layout

```
src/
  main.cpp                  CLI entry point (--console/--install/--uninstall/...)
  service/                  SCM registration + worker loop
  collector/                Orchestrates one full collection+send cycle
  wmi/                      Win32_Printer enumeration + WSD Location parsing
  snmp/                     Hand-rolled SNMP v1 client (ASN.1 BER + WinSock2)
  registry/                 Best-effort PrinterDriverData counter scraping
  network/                  Host IP address enumeration
  http/                     WinHTTP JSON POST client
  ws/                       WinHTTP WebSocket control-channel client
  common/                   Config (INI), logger, JSON writer, string utils
  model/                    PrinterInfo data model
config/agent.ini            Example configuration (copied next to the .exe on build)
scripts/build.bat             Standalone command-line build script
scripts/install_service.bat   Deployment script: install + start the service (copied next to the .exe on build)
scripts/uninstall_service.bat Deployment script: stop + uninstall the service (copied next to the .exe on build)
```
