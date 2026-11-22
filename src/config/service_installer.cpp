#include "service_installer.h"
#include "util/os_utils.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <taskschd.h>
#include <comdef.h>
#include <sddl.h>
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "advapi32.lib")
#elif !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace BareStats {

namespace fs = std::filesystem;

fs::path GetServiceStagingDirectory() {
#if defined(_WIN32)
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && *local_app_data) {
        return fs::path(local_app_data) / "BareStats";
    }
    const char* user_profile = std::getenv("USERPROFILE");
    return fs::path(user_profile ? user_profile : "C:\\") / "AppData" / "Local" / "BareStats";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "BareStats";
#elif defined(__linux__)
    if (geteuid() == 0) {
        return fs::path("/etc/barestats");
    }
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / ".local" / "share" / "barestats";
#else
    return fs::current_path();
#endif
}

std::string GenerateSystemdServiceUnit(const fs::path& exe_path, const fs::path& work_dir, bool is_root) {
    if (is_root) {
        return
            "[Unit]\n"
            "Description=BareStats Headless Satellite (Sandboxed)\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=\"" + exe_path.string() + "\"\n"
            "WorkingDirectory=\"" + work_dir.string() + "\"\n"
            "ConfigurationDirectory=barestats\n"
            "Restart=always\n"
            "RestartSec=5\n"
            "DynamicUser=yes\n"
            "ProtectSystem=strict\n"
            "ProtectHome=true\n"
            "NoNewPrivileges=true\n"
            "CapabilityBoundingSet=\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n";
    } else {
        return
            "[Unit]\n"
            "Description=BareStats Headless Satellite (User)\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=\"" + exe_path.string() + "\"\n"
            "WorkingDirectory=\"" + work_dir.string() + "\"\n"
            "Restart=always\n"
            "RestartSec=5\n\n"
            "[Install]\n"
            "WantedBy=default.target\n";
    }
}

static bool IsPathSafeForShell(const std::string& path_str) {
    const std::string prohibited = "\"'$`;&|><\n\r";
    return path_str.find_first_of(prohibited) == std::string::npos;
}

std::string BuildLaunchdBootstrapCommand(const std::string& lctl, uint32_t uid, const fs::path& plist_file) {
    if (!IsPathSafeForShell(lctl) || !IsPathSafeForShell(plist_file.string())) {
        return "";
    }
    return lctl + " bootstrap gui/" + std::to_string(uid) + " \"" + plist_file.string() + "\"";
}

std::string BuildLaunchdBootoutCommand(const std::string& lctl, uint32_t uid, const fs::path& plist_file) {
    if (!IsPathSafeForShell(lctl) || !IsPathSafeForShell(plist_file.string())) {
        return "";
    }
    return lctl + " bootout gui/" + std::to_string(uid) + " \"" + plist_file.string() + "\"";
}

#if defined(__linux__)
bool InstallSystemdService(const fs::path& exe_path, const fs::path& work_dir) {
    if (!IsPathSafeForShell(exe_path.string()) || !IsPathSafeForShell(work_dir.string())) {
        return false;
    }
    bool is_root = (geteuid() == 0);
    fs::path service_file;
    std::string unit_content = GenerateSystemdServiceUnit(exe_path, work_dir, is_root);

    if (is_root) {
        service_file = "/etc/systemd/system/barestats.service";
    } else {
        // 100% Unprivileged user-mode service
        const char* home = std::getenv("HOME");
        if (!home || !*home) return false;
        fs::path user_systemd_dir = fs::path(home) / ".config" / "systemd" / "user";
        std::error_code ec;
        fs::create_directories(user_systemd_dir, ec);
        service_file = user_systemd_dir / "barestats.service";
    }

    std::ofstream out(service_file);
    if (!out.is_open()) return false;
    out << unit_content;
    out.close();

    std::string sysctl = fs::exists("/usr/bin/systemctl") ? "/usr/bin/systemctl" :
                         (fs::exists("/bin/systemctl") ? "/bin/systemctl" : "systemctl");

    if (is_root) {
        if (std::system((sysctl + " daemon-reload").c_str()) != 0) return false;
        if (std::system((sysctl + " enable --now barestats").c_str()) != 0) return false;
    } else {
        if (std::system((sysctl + " --user daemon-reload").c_str()) != 0) return false;
        if (std::system((sysctl + " --user enable --now barestats").c_str()) != 0) return false;
        std::system("loginctl enable-linger >/dev/null 2>&1");
    }
    return true;
}

bool UninstallSystemdService() {
    bool is_root = (geteuid() == 0);
    std::string sysctl = fs::exists("/usr/bin/systemctl") ? "/usr/bin/systemctl" :
                         (fs::exists("/bin/systemctl") ? "/bin/systemctl" : "systemctl");

    if (is_root) {
        std::system((sysctl + " disable --now barestats").c_str());
        std::error_code ec;
        fs::remove("/etc/systemd/system/barestats.service", ec);
        std::system((sysctl + " daemon-reload").c_str());
    } else {
        std::system((sysctl + " --user disable --now barestats").c_str());
        const char* home = std::getenv("HOME");
        if (home && *home) {
            std::error_code ec;
            fs::remove(fs::path(home) / ".config" / "systemd" / "user" / "barestats.service", ec);
        }
        std::system((sysctl + " --user daemon-reload").c_str());
    }
    std::system("killall -q barestats >/dev/null 2>&1 || pkill -x barestats >/dev/null 2>&1 || true");
    return true;
}
#elif defined(__APPLE__)
bool InstallLaunchdDaemon(const fs::path& exe_path, const fs::path& work_dir) {
    if (!IsPathSafeForShell(exe_path.string()) || !IsPathSafeForShell(work_dir.string())) {
        return false;
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;

    fs::path agents_dir = fs::path(home) / "Library" / "LaunchAgents";
    std::error_code ec;
    fs::create_directories(agents_dir, ec);
    fs::path plist_file = agents_dir / "com.barestats.satellite.plist";
    if (!IsPathSafeForShell(plist_file.string())) {
        return false;
    }

    std::string plist_content = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.barestats.satellite</string>
    <key>ProgramArguments</key>
    <array>
        <string>)" + exe_path.string() + R"(</string>
    </array>
    <key>WorkingDirectory</key>
    <string>)" + work_dir.string() + R"(</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>)";

    std::ofstream out(plist_file);
    if (!out.is_open()) return false;
    out << plist_content;
    out.close();

    chmod(plist_file.c_str(), S_IRUSR | S_IWUSR);

    std::string lctl = fs::exists("/bin/launchctl") ? "/bin/launchctl" : "launchctl";
    uid_t uid = getuid();
    std::string bootstrap_cmd = BuildLaunchdBootstrapCommand(lctl, uid, plist_file);
    if (bootstrap_cmd.empty()) return false;
    if (std::system(bootstrap_cmd.c_str()) != 0) {
        std::string load_cmd = lctl + " load \"" + plist_file.string() + "\"";
        return std::system(load_cmd.c_str()) == 0;
    }
    return true;
}

bool UninstallLaunchdDaemon() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;

    fs::path plist_file = fs::path(home) / "Library" / "LaunchAgents" / "com.barestats.satellite.plist";
    if (!IsPathSafeForShell(plist_file.string())) {
        return false;
    }
    std::string lctl = fs::exists("/bin/launchctl") ? "/bin/launchctl" : "launchctl";
    uid_t uid = getuid();
    std::string bootout_cmd = BuildLaunchdBootoutCommand(lctl, uid, plist_file);
    if (bootout_cmd.empty()) return false;
    if (std::system(bootout_cmd.c_str()) != 0) {
        std::string unload_cmd = lctl + " unload \"" + plist_file.string() + "\"";
        std::system(unload_cmd.c_str());
    }

    std::error_code ec;
    fs::remove(plist_file, ec);
    std::system("pkill -x barestats >/dev/null 2>&1 || pkill -x BareStats >/dev/null 2>&1 || true");
    return true;
}
#elif defined(_WIN32)
static bool InstallWindowsRegistryRun(const fs::path& exe_path) {
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey);
    if (lRes != ERROR_SUCCESS) return false;

    std::wstring quoted_path = L"\"" + exe_path.wstring() + L"\"";
    lRes = RegSetValueExW(hKey, L"BareStatsSatellite", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(quoted_path.c_str()),
        static_cast<DWORD>((quoted_path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return lRes == ERROR_SUCCESS;
}

static bool UninstallWindowsRegistryRun() {
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey);
    if (lRes != ERROR_SUCCESS) return false;

    lRes = RegDeleteValueW(hKey, L"BareStatsSatellite");
    RegCloseKey(hKey);
    return (lRes == ERROR_SUCCESS || lRes == ERROR_FILE_NOT_FOUND);
}

static bool InstallWindowsTaskScheduler(const fs::path& exe_path, const fs::path& work_dir) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    ITaskService* pService = NULL;
    hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pService);
    if (FAILED(hr)) {
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        pService->Release();
        if (shouldUninit) CoUninitialize();
        return false;
    }

    ITaskDefinition* pTask = NULL;
    hr = pService->NewTask(0, &pTask);
    if (FAILED(hr)) {
        pService->Release();
        if (shouldUninit) CoUninitialize();
        return false;
    }

    // Configure Principal: 100% Unprivileged User (TASK_RUNLEVEL_LUA, TASK_LOGON_INTERACTIVE_TOKEN)
    IPrincipal* pPrincipal = NULL;
    pTask->get_Principal(&pPrincipal);
    if (pPrincipal) {
        pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        pPrincipal->put_RunLevel(TASK_RUNLEVEL_LUA);
        pPrincipal->Release();
    }

    ITaskSettings* pSettings = NULL;
    pTask->get_Settings(&pSettings);
    if (pSettings) {
        pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
        pSettings->put_Hidden(VARIANT_TRUE);
        pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        pSettings->put_StartWhenAvailable(VARIANT_TRUE);
        pSettings->Release();
    }

    ITriggerCollection* pTriggerCollection = NULL;
    pTask->get_Triggers(&pTriggerCollection);
    if (pTriggerCollection) {
        ITrigger* pTrigger = NULL;
        pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
        if (pTrigger) pTrigger->Release();
        pTriggerCollection->Release();
    }

    IActionCollection* pActionCollection = NULL;
    pTask->get_Actions(&pActionCollection);
    if (pActionCollection) {
        IAction* pAction = NULL;
        pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
        if (pAction) {
            IExecAction* pExecAction = NULL;
            pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction);
            if (pExecAction) {
                pExecAction->put_Path(_bstr_t(exe_path.wstring().c_str()));
                pExecAction->put_WorkingDirectory(_bstr_t(work_dir.wstring().c_str()));
                pExecAction->Release();
            }
            pAction->Release();
        }
        pActionCollection->Release();
    }

    ITaskFolder* pRootFolder = NULL;
    hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
    if (FAILED(hr) || !pRootFolder) {
        pTask->Release();
        pService->Release();
        if (shouldUninit) CoUninitialize();
        return false;
    }

    IRegisteredTask* pRegisteredTask = NULL;
    hr = pRootFolder->RegisterTaskDefinition(
        _bstr_t(L"BareStatsSatellite"),
        pTask,
        TASK_CREATE_OR_UPDATE,
        _variant_t(),
        _variant_t(),
        TASK_LOGON_INTERACTIVE_TOKEN,
        _variant_t(L""),
        &pRegisteredTask
    );

    if (SUCCEEDED(hr) && pRegisteredTask) {
        pRegisteredTask->Run(_variant_t(), nullptr);
        pRegisteredTask->Release();
    }

    pRootFolder->Release();
    pTask->Release();
    pService->Release();
    if (shouldUninit) CoUninitialize();

    return SUCCEEDED(hr);
}

static bool UninstallWindowsTaskScheduler() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    ITaskService* pService = NULL;
    hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pService);
    if (FAILED(hr)) {
        if (shouldUninit) CoUninitialize();
        return false;
    }

    hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        pService->Release();
        if (shouldUninit) CoUninitialize();
        return false;
    }

    ITaskFolder* pRootFolder = NULL;
    hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
    if (SUCCEEDED(hr) && pRootFolder) {
        IRegisteredTask* pRegisteredTask = NULL;
        if (SUCCEEDED(pRootFolder->GetTask(_bstr_t(L"BareStatsSatellite"), &pRegisteredTask)) && pRegisteredTask) {
            pRegisteredTask->Stop(0);
            pRegisteredTask->Release();
        }
        pRootFolder->DeleteTask(_bstr_t(L"BareStatsSatellite"), 0);
        pRootFolder->Release();
    }

    pService->Release();
    if (shouldUninit) CoUninitialize();
    return true;
}

bool InstallWindowsTask(const fs::path& exe_path, const fs::path& work_dir) {
    // Attempt unprivileged Task Scheduler registration first (TASK_RUNLEVEL_LUA)
    bool taskOk = InstallWindowsTaskScheduler(exe_path, work_dir);
    // Register unprivileged HKCU Run autostart key as primary/fallback guarantee
    bool regOk = InstallWindowsRegistryRun(exe_path);

    if (!taskOk && !regOk) {
        return false;
    }

    return true;
}

bool UninstallWindowsTask() {
    UninstallWindowsTaskScheduler();
    UninstallWindowsRegistryRun();
    std::system("taskkill /F /IM BareStats.exe /IM barestats.exe >nul 2>&1");
    return true;
}
#endif

int InstallOsService(const fs::path& source_satellite_ini) {
    if (!fs::exists(source_satellite_ini)) {
        std::cerr << "Error: satellite.ini not found. Export it from Command Center first.\n";
        return 1;
    }

    fs::path current_dir = GetExecutableDirectory();
    fs::path target_dir = GetServiceStagingDirectory();

#if defined(_WIN32)
    fs::path source_exe = current_dir / "BareStats.exe";
    if (!fs::exists(source_exe)) {
        source_exe = current_dir / "barestats.exe";
    }
    if (!fs::exists(source_exe)) {
        source_exe = current_dir / "BareStatsTests.exe";
    }
    fs::path staged_exe = target_dir / "BareStats.exe";
    fs::path staged_ini = target_dir / "satellite.ini";
#elif defined(__APPLE__)
    fs::path source_exe = current_dir / "barestats";
    fs::path staged_exe = target_dir / "barestats";
    fs::path staged_ini = target_dir / "satellite.ini";
#elif defined(__linux__)
    bool is_root = (geteuid() == 0);
    fs::path source_exe = current_dir / "barestats";
    fs::path staged_exe = is_root ? fs::path("/usr/local/bin/barestats") : (target_dir / "barestats");
    fs::path staged_ini = target_dir / "satellite.ini";
#endif

    try {
        std::error_code ec;
        fs::create_directories(target_dir, ec);
        if (fs::exists(source_exe)) {
            fs::copy_file(source_exe, staged_exe, fs::copy_options::overwrite_existing, ec);
        }
        fs::copy_file(source_satellite_ini, staged_ini, fs::copy_options::overwrite_existing);

#if !defined(_WIN32)
        chmod(staged_exe.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
#if defined(__linux__)
        if (is_root) {
            // DynamicUser=yes needs read access to configuration in /etc/barestats
            chmod(staged_ini.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            chmod(target_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        } else {
            chmod(staged_ini.c_str(), S_IRUSR | S_IWUSR);
            chmod(target_dir.c_str(), S_IRWXU);
        }
#else
        chmod(staged_ini.c_str(), S_IRUSR | S_IWUSR);
        chmod(target_dir.c_str(), S_IRWXU);
#endif
#else
        // Windows: Harden satellite.ini ACLs to restrict cluster key access to Owner and SYSTEM / Admins (VULN-02)
        PSECURITY_DESCRIPTOR pSD = NULL;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;OW)(A;;GA;;;SY)(A;;GA;;;BA)",
                SDDL_REVISION_1,
                &pSD,
                NULL)) {
            SetFileSecurityW(staged_ini.wstring().c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, pSD);
            LocalFree(pSD);
        }
#endif
    } catch (const std::exception& e) {
        std::cerr << "Error staging service files: " << e.what() << "\n";
        return 1;
    }

#if defined(__linux__)
    if (!InstallSystemdService(staged_exe, target_dir)) return 1;
#elif defined(__APPLE__)
    if (!InstallLaunchdDaemon(staged_exe, target_dir)) return 1;
#elif defined(_WIN32)
    if (!InstallWindowsTask(staged_exe, target_dir)) return 1;
#endif

    std::cout << "[BareStats] Satellite service secured, installed, and started successfully.\n";
    return 0;
}

int UninstallOsService() {
    fs::path target_dir = GetServiceStagingDirectory();

#if defined(__linux__)
    if (!UninstallSystemdService()) return 1;
#elif defined(__APPLE__)
    if (!UninstallLaunchdDaemon()) return 1;
#elif defined(_WIN32)
    if (!UninstallWindowsTask()) return 1;
#endif

    std::error_code ec;
    fs::remove(target_dir / "satellite.ini", ec);

    std::cout << "[BareStats] Satellite service uninstalled successfully.\n";
    return 0;
}

} // namespace BareStats