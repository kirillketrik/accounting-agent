cmake_minimum_required(VERSION 3.20)

# Static CRT (/MT, /MTd) so the service doesn't depend on a VC++ Redistributable
# being present on old / locked-down machines (down to Windows 7 SP1).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(printer_agent CXX)

if (NOT MSVC)
    message(FATAL_ERROR "This project targets MSVC only (static CRT, Win7+ Win32 APIs).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 7 SP1 is the minimum supported OS (see brief). 0x0601 = _WIN32_WINNT_WIN7.
add_compile_definitions(
    _WIN32_WINNT=0x0601
    WINVER=0x0601
    NTDDI_VERSION=0x06010000
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

add_executable(printer_agent
    src/main.cpp

    src/common/logger.h
    src/common/logger.cpp
    src/common/config.h
    src/common/config.cpp
    src/common/json_writer.h
    src/common/string_utils.h
    src/common/string_utils.cpp

    src/model/printer_info.h

    src/wmi/wmi_client.h
    src/wmi/wmi_client.cpp
    src/wmi/printer_enumerator.h
    src/wmi/printer_enumerator.cpp

    src/snmp/snmp_client.h
    src/snmp/snmp_client.cpp
    src/snmp/ber.h
    src/snmp/ber.cpp

    src/registry/registry_reader.h
    src/registry/registry_reader.cpp

    src/pjl/pjl_client.h
    src/pjl/pjl_client.cpp

    src/network/network_info.h
    src/network/network_info.cpp

    src/http/http_client.h
    src/http/http_client.cpp

    src/ws/ws_client.h
    src/ws/ws_client.cpp

    src/collector/data_collector.h
    src/collector/data_collector.cpp
    src/collector/liveness_tracker.h
    src/collector/liveness_tracker.cpp

    src/service/service_base.h
    src/service/service_base.cpp
)

target_include_directories(printer_agent PRIVATE src)

target_link_libraries(printer_agent PRIVATE
    ws2_32       # WinSock2 (SNMP UDP sockets)
    iphlpapi     # GetAdaptersAddresses (host IP enumeration)
    wbemuuid     # WMI / COM
    ole32
    oleaut32
    winhttp      # HTTP(S) client
    advapi32     # registry + service control manager
    winspool     # OpenPrinter/StartDocPrinter/WritePrinter/ReadPrinter (PJL query)
)

# Copy the example config and the deployment scripts next to the built
# binary, so a fresh build produces a ready-to-copy folder: agent.ini is
# looked up next to the .exe at runtime, and both .bat scripts expect to sit
# alongside printer_agent.exe on the target machine.
add_custom_command(TARGET printer_agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/agent.ini"
        "$<TARGET_FILE_DIR:printer_agent>/agent.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/scripts/install_service.bat"
        "$<TARGET_FILE_DIR:printer_agent>/install_service.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/scripts/uninstall_service.bat"
        "$<TARGET_FILE_DIR:printer_agent>/uninstall_service.bat"
)
