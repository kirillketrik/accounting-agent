#include "pjl_client.h"
#include "common/string_utils.h"
#include <windows.h>
#include <winspool.h>
#include <thread>
#include <future>
#include <memory>
#include <vector>
#include <cctype>

namespace {

// Universal Exit Language wrapper required around any PJL command sequence.
const char kUel[] = "\x1B%-12345X";
const char kQuery[] = "@PJL INFO PAGECOUNT\r\n";

bool ParsePageCountFromReply(const std::vector<BYTE>& reply, int64_t& outPages) {
    for (size_t i = 0; i < reply.size(); i++) {
        if (!isdigit((unsigned char)reply[i])) continue;
        size_t j = i;
        while (j < reply.size() && isdigit((unsigned char)reply[j])) j++;
        std::string digits(reinterpret_cast<const char*>(&reply[i]), j - i);
        try {
            outPages = std::stoll(digits);
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

} // namespace

bool TryGetPjlPageCount(const std::string& printerName, int timeoutMs, int64_t& outPages, Logger& logger) {
    std::wstring wName = strutil::Utf8ToWide(printerName);

    HANDLE hPrinter = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wName.c_str()), &hPrinter, nullptr)) {
        logger.Debug("PJL: OpenPrinterW failed for '" + printerName + "', error=" + std::to_string(GetLastError()));
        return false;
    }

    std::wstring docName = L"PJL Status Query";
    std::wstring dataType = L"RAW";
    DOC_INFO_1W docInfo{};
    docInfo.pDocName = const_cast<LPWSTR>(docName.c_str());
    docInfo.pOutputFile = nullptr;
    docInfo.pDatatype = const_cast<LPWSTR>(dataType.c_str());

    if (StartDocPrinterW(hPrinter, 1, reinterpret_cast<LPBYTE>(&docInfo)) == 0) {
        logger.Debug("PJL: StartDocPrinterW failed for '" + printerName + "', error=" + std::to_string(GetLastError()));
        ClosePrinter(hPrinter);
        return false;
    }

    if (!StartPagePrinter(hPrinter)) {
        logger.Debug("PJL: StartPagePrinter failed for '" + printerName + "', error=" + std::to_string(GetLastError()));
        EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);
        return false;
    }

    std::string query = std::string(kUel) + kQuery + kUel;
    DWORD written = 0;
    BOOL wrote = WritePrinter(hPrinter, (LPVOID)query.data(), (DWORD)query.size(), &written);
    if (!wrote || written != query.size()) {
        logger.Debug("PJL: WritePrinter failed/partial for '" + printerName + "', error=" + std::to_string(GetLastError()));
        EndPagePrinter(hPrinter);
        EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);
        return false;
    }

    // ReadPrinter has no timeout parameter and WinSpool documents no safe way
    // to cancel a pending call on it from another thread (unlike the WinHTTP
    // handle-close trick used elsewhere in this codebase). Bound it by
    // reading on a helper thread and only waiting up to timeoutMs for it: on
    // timeout, detach and let the abandoned thread finish the job lifecycle
    // (EndPagePrinter/EndDocPrinter/ClosePrinter) whenever ReadPrinter
    // eventually returns, or leak it harmlessly until process exit. The
    // detached path deliberately does not touch `logger` - if it's still
    // running when the service is stopping, the Logger the caller owns may
    // already be gone by the time this thread would try to log.
    auto readResult = std::make_shared<std::promise<std::vector<BYTE>>>();
    std::future<std::vector<BYTE>> future = readResult->get_future();

    std::thread reader([hPrinter, readResult]() {
        std::vector<BYTE> buffer(4096);
        DWORD bytesRead = 0;
        BOOL ok = ReadPrinter(hPrinter, buffer.data(), (DWORD)buffer.size(), &bytesRead);
        if (ok && bytesRead > 0) {
            buffer.resize(bytesRead);
        } else {
            buffer.clear();
        }
        try {
            readResult->set_value(std::move(buffer));
        } catch (...) {
            // Caller already gave up and the future was abandoned; nothing to do.
        }
        EndPagePrinter(hPrinter);
        EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);
    });

    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        logger.Debug("PJL: no reply from '" + printerName + "' within " + std::to_string(timeoutMs) +
            "ms; giving up (this printer likely doesn't support bidirectional PJL).");
        reader.detach();
        return false;
    }

    reader.join();
    std::vector<BYTE> reply = future.get();
    if (reply.empty()) {
        logger.Debug("PJL: empty/failed ReadPrinter reply for '" + printerName + "'");
        return false;
    }

    if (!ParsePageCountFromReply(reply, outPages)) {
        logger.Debug("PJL: reply from '" + printerName + "' did not contain a parseable page count");
        return false;
    }

    logger.Debug("PJL: '" + printerName + "' reported PAGECOUNT=" + std::to_string(outPages));
    return true;
}
