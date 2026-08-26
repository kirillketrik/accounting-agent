#include <windows.h>
#include <string>
#include <cstdio>
#include "service/service_base.h"

namespace {

bool ArgEquals(const wchar_t* arg, const wchar_t* option) {
    if (arg[0] == L'-' && arg[1] == L'-') arg += 2;
    else if (arg[0] == L'-') arg += 1;
    return _wcsicmp(arg, option) == 0;
}

void PrintUsage() {
    wprintf(
        L"Printer Inventory Agent\n"
        L"\n"
        L"Usage:\n"
        L"  printer_agent.exe                run under the Service Control Manager (normal deployment)\n"
        L"  printer_agent.exe --console       run interactively in this console (Ctrl+C to stop)\n"
        L"  printer_agent.exe --install       register the Windows service (auto-start), then exit\n"
        L"  printer_agent.exe --uninstall     remove the Windows service, then exit\n"
        L"  printer_agent.exe --help          show this message\n"
        L"\n"
        L"Configuration is read from agent.ini next to this executable.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2) {
        if (ArgEquals(argv[1], L"console") || ArgEquals(argv[1], L"debug")) {
            PrinterAgentService::RunInConsole();
            return 0;
        }
        if (ArgEquals(argv[1], L"install")) {
            std::wstring error;
            if (PrinterAgentService::Install(&error)) {
                wprintf(L"Service installed successfully.\n");
                return 0;
            }
            fwprintf(stderr, L"Install failed: %s\n", error.c_str());
            return 1;
        }
        if (ArgEquals(argv[1], L"uninstall")) {
            std::wstring error;
            if (PrinterAgentService::Uninstall(&error)) {
                wprintf(L"Service uninstalled successfully.\n");
                return 0;
            }
            fwprintf(stderr, L"Uninstall failed: %s\n", error.c_str());
            return 1;
        }
        if (ArgEquals(argv[1], L"help") || ArgEquals(argv[1], L"?")) {
            PrintUsage();
            return 0;
        }
        PrintUsage();
        return 1;
    }

    // No arguments: assume we were launched by the Service Control Manager.
    PrinterAgentService::RunAsService();
    return 0;
}
