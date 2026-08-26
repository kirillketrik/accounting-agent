#pragma once
#include <string>
#include <map>
#include <set>
#include <vector>
#include "model/printer_info.h"

// Tracks each printer's reachability across collection cycles for the
// lifetime of one agent process run (resets on service restart, same as the
// websocket reconnect-streak counter in ws_client.cpp - deliberately not
// persisted to disk).
//
// Usage per cycle, in RunCollectionCycle:
//   for each printer: tracker.RecordThisCycle(dedupKey, reachable, threshold, p.status, p.consecutiveFailures);
//   after the loop:   std::vector<std::string> removed = tracker.DrainRemoved();
//
// Not thread-safe; only ever touched from the single worker-loop thread that
// runs collection cycles.
// One printer that vanished from Windows' printer enumeration between one
// cycle and the next (not merely unreachable - actually gone).
struct RemovedPrinter {
    std::string dedupKey;
    std::string name; // last known display name, for a human-readable report
};

class LivenessTracker {
public:
    // dedupKey should be p.resolvedHost when non-empty, else p.name - this
    // matches the accounting backend's own dedup key (PrinterRepository.
    // find_by_dedup_key), so the agent and server agree on printer identity.
    //
    // `reachable` is this cycle's raw signal (SNMP success, or WMI
    // PrinterStatus for local/USB - see data_collector.cpp). A printer only
    // flips to Offline once its consecutive-failure count reaches
    // `offlineThreshold`; any success resets the count to 0 and flips it
    // straight back to Online.
    void RecordThisCycle(const std::string& dedupKey, const std::string& name, bool reachable, int offlineThreshold,
                          PrinterStatus& outStatus, int& outConsecutiveFailures);

    // For printers with no reachability signal at all (PortType::Other -
    // nothing we know how to probe). Still counts as "seen" for removal
    // detection, but never touches the failure counter or flips status away
    // from Unknown, since there's nothing to base Online/Offline on.
    void MarkSeen(const std::string& dedupKey, const std::string& name);

    // Call once per cycle, after every printer actually visited this cycle
    // has been recorded above. Returns printers that were known from a
    // previous cycle but were not recorded this time (i.e. the printer
    // disappeared from Windows' enumeration entirely - uninstalled, not just
    // unreachable). Resets the "seen this cycle" bookkeeping for the next cycle.
    std::vector<RemovedPrinter> DrainRemoved();

private:
    std::map<std::string, int> consecutiveFailures_;
    std::map<std::string, std::string> lastKnownNames_;
    std::set<std::string> lastKnownKeys_;
    std::set<std::string> seenThisCycle_;
};
