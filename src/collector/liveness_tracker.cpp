#include "liveness_tracker.h"

void LivenessTracker::MarkSeen(const std::string& dedupKey, const std::string& name) {
    seenThisCycle_.insert(dedupKey);
    lastKnownKeys_.insert(dedupKey);
    lastKnownNames_[dedupKey] = name;
}

void LivenessTracker::RecordThisCycle(const std::string& dedupKey, const std::string& name, bool reachable,
                                       int offlineThreshold, PrinterStatus& outStatus, int& outConsecutiveFailures) {
    MarkSeen(dedupKey, name);

    int& failures = consecutiveFailures_[dedupKey]; // default-inits to 0 on first sight
    if (reachable) {
        failures = 0;
    } else {
        failures++;
    }

    outConsecutiveFailures = failures;
    outStatus = (failures >= offlineThreshold) ? PrinterStatus::Offline : PrinterStatus::Online;
}

std::vector<RemovedPrinter> LivenessTracker::DrainRemoved() {
    std::vector<RemovedPrinter> removed;
    for (const auto& key : lastKnownKeys_) {
        if (seenThisCycle_.find(key) == seenThisCycle_.end()) {
            RemovedPrinter rp;
            rp.dedupKey = key;
            auto nameIt = lastKnownNames_.find(key);
            rp.name = (nameIt != lastKnownNames_.end()) ? nameIt->second : "";
            removed.push_back(std::move(rp));
        }
    }
    for (const auto& rp : removed) {
        lastKnownKeys_.erase(rp.dedupKey);
        consecutiveFailures_.erase(rp.dedupKey);
        lastKnownNames_.erase(rp.dedupKey);
    }
    seenThisCycle_.clear();
    return removed;
}
