#pragma once
#include <windows.h>
#include "common/config.h"
#include "common/logger.h"
#include "collector/liveness_tracker.h"
#include "discovery/network_discovery.h"

// Runs one full collection+send cycle: enumerate printers (WMI), gather page
// counts (SNMP for TCP/IP + WSD printers, PJL/WMI-spooler/registry fallback
// tiers for USB/local ones), optionally sweep the network for printers this
// PC has no queue for (`discovery`, when [discovery] enabled), evaluate
// reachability against `tracker` to derive each printer's online/offline
// status and detect printers that disappeared from Windows entirely, gather
// this host's own IP addresses, assemble the JSON payload, and POST it to
// the configured server. Every sub-step is isolated so that one bad printer
// or an unreachable server logs a warning/error and the rest of the cycle
// still completes; this function itself never throws.
//
// `tracker` and `discovery` persist across calls (owned by the caller, e.g.
// WorkerLoop) so consecutive-failure counts, "last known printers" and
// previously discovered printers survive across cycles for the lifetime of
// the process - a single cycle has no memory of its own.
//
// `stopEvent` (optional) lets a service stop/shutdown request interrupt a
// cycle between printers and between SNMP retry waits, instead of forcing
// the SCM to wait out a potentially multi-minute in-flight cycle. A cycle
// interrupted this way only marks the printers it actually got to as "seen"
// for removal-detection purposes, so a shutdown never produces false-positive
// "removed" reports for printers it simply didn't reach yet.
void RunCollectionCycle(const AgentConfig& config, Logger& logger, LivenessTracker& tracker,
                        NetworkDiscovery& discovery, HANDLE stopEvent = nullptr);
