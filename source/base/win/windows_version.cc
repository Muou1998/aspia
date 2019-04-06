//
// Aspia Project
// Copyright (C) 2019 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "base/win/windows_version.h"

#include "base/logging.h"
#include "base/strings/unicode.h"
#include "base/win/file_version_info.h"
#include "base/win/registry.h"

namespace base::win {

namespace {

typedef BOOL(WINAPI *GetProductInfoPtr)(DWORD, DWORD, DWORD, DWORD, PDWORD);

// Helper to map a major.minor.x.build version (e.g. 6.1) to a Windows release.
Version majorMinorBuildToVersion(int major, int minor, int build)
{
    if ((major == 5) && (minor > 0))
    {
        // Treat XP Pro x64, Home Server, and Server 2003 R2 as Server 2003.
        return (minor == 1) ? VERSION_XP : VERSION_SERVER_2003;
    }
    else if (major == 6)
    {
        switch (minor)
        {
            case 0:
                // Treat Windows Server 2008 the same as Windows Vista.
                return VERSION_VISTA;
            case 1:
                // Treat Windows Server 2008 R2 the same as Windows 7.
                return VERSION_WIN7;
            case 2:
                // Treat Windows Server 2012 the same as Windows 8.
                return VERSION_WIN8;
            default:
                DCHECK_EQ(minor, 3);
                return VERSION_WIN8_1;
        }
    }
    else if (major == 10)
    {
        if (build < 10586)
        {
            return VERSION_WIN10;
        }
        else if (build < 14393)
        {
            return VERSION_WIN10_TH2;
        }
        else if (build < 15063)
        {
            return VERSION_WIN10_RS1;
        }
        else if (build < 16299)
        {
            return VERSION_WIN10_RS2;
        }
        else if (build < 17134)
        {
            return VERSION_WIN10_RS3;
        }
        else
        {
            return VERSION_WIN10_RS4;
        }
    }
    else if (major > 6)
    {
        NOTREACHED();
        return VERSION_WIN_LAST;
    }

    return VERSION_PRE_XP;
}

// Returns the the "UBR" value from the registry. Introduced in Windows 10, this undocumented value
// appears to be similar to a patch number.
// Returns 0 if the value does not exist or it could not be read.
int readUBR()
{
    // The values under the CurrentVersion registry hive are mirrored under
    // the corresponding Wow6432 hive.
    static constexpr wchar_t kRegKeyWindowsNTCurrentVersion[] =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

    base::win::RegistryKey key;
    if (key.open(HKEY_LOCAL_MACHINE, kRegKeyWindowsNTCurrentVersion, KEY_QUERY_VALUE) != ERROR_SUCCESS)
        return 0;

    DWORD ubr = 0;
    key.readValueDW(L"UBR", &ubr);

    return static_cast<int>(ubr);
}

} // namespace

// static
OSInfo** OSInfo::instanceStorage()
{
    // Note: we don't use the Singleton class because it depends on AtExitManager,
    // and it's convenient for other modules to use this class without it.
    static OSInfo* info = []()
    {
        _OSVERSIONINFOEXW version_info = { sizeof(version_info) };
        GetVersionExW(reinterpret_cast<_OSVERSIONINFOW*>(&version_info));

        _SYSTEM_INFO system_info = {};
        GetNativeSystemInfo(&system_info);

        DWORD os_type = 0;
        if (version_info.dwMajorVersion == 6 || version_info.dwMajorVersion == 10)
        {
            // Only present on Vista+.
            GetProductInfoPtr get_product_info =
                reinterpret_cast<GetProductInfoPtr>(::GetProcAddress(
                    GetModuleHandleW(L"kernel32.dll"), "GetProductInfo"));

            get_product_info(version_info.dwMajorVersion, version_info.dwMinorVersion,
                             0, 0, &os_type);
        }

        return new OSInfo(version_info, system_info, os_type);
    }();

    return &info;
}

// static
OSInfo* OSInfo::instance()
{
    return *instanceStorage();
}

OSInfo::OSInfo(const _OSVERSIONINFOEXW& version_info,
               const _SYSTEM_INFO& system_info,
               int os_type)
    : version_(VERSION_PRE_XP),
      architecture_(OTHER_ARCHITECTURE),
      wow64_status_(wow64StatusForProcess(GetCurrentProcess()))
{
    version_number_.major = version_info.dwMajorVersion;
    version_number_.minor = version_info.dwMinorVersion;
    version_number_.build = version_info.dwBuildNumber;
    version_number_.patch = readUBR();
    version_ = majorMinorBuildToVersion(
        version_number_.major, version_number_.minor, version_number_.build);
    service_pack_.major = version_info.wServicePackMajor;
    service_pack_.minor = version_info.wServicePackMinor;
    service_pack_str_ = base::UTF8fromUTF16(version_info.szCSDVersion);

    switch (system_info.wProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_INTEL:
            architecture_ = X86_ARCHITECTURE;
            break;

        case PROCESSOR_ARCHITECTURE_AMD64:
            architecture_ = X64_ARCHITECTURE;
            break;

        case PROCESSOR_ARCHITECTURE_IA64:
            architecture_ = IA64_ARCHITECTURE;
            break;

        default:
            break;
    }

    processors_ = system_info.dwNumberOfProcessors;
    allocation_granularity_ = system_info.dwAllocationGranularity;

    if (version_info.dwMajorVersion == 6 || version_info.dwMajorVersion == 10)
    {
        // Only present on Vista+.
        switch (os_type)
        {
            case PRODUCT_CLUSTER_SERVER:
            case PRODUCT_DATACENTER_SERVER:
            case PRODUCT_DATACENTER_SERVER_CORE:
            case PRODUCT_ENTERPRISE_SERVER:
            case PRODUCT_ENTERPRISE_SERVER_CORE:
            case PRODUCT_ENTERPRISE_SERVER_IA64:
            case PRODUCT_SMALLBUSINESS_SERVER:
            case PRODUCT_SMALLBUSINESS_SERVER_PREMIUM:
            case PRODUCT_STANDARD_SERVER:
            case PRODUCT_STANDARD_SERVER_CORE:
            case PRODUCT_WEB_SERVER:
                version_type_ = SUITE_SERVER;
                break;

            case PRODUCT_PROFESSIONAL:
            case PRODUCT_ULTIMATE:
                version_type_ = SUITE_PROFESSIONAL;
                break;

            case PRODUCT_ENTERPRISE:
            case PRODUCT_ENTERPRISE_E:
            case PRODUCT_ENTERPRISE_EVALUATION:
            case PRODUCT_ENTERPRISE_N:
            case PRODUCT_ENTERPRISE_N_EVALUATION:
            //case PRODUCT_ENTERPRISE_S:
            //case PRODUCT_ENTERPRISE_S_EVALUATION:
            //case PRODUCT_ENTERPRISE_S_N:
            //case PRODUCT_ENTERPRISE_S_N_EVALUATION:
            case PRODUCT_BUSINESS:
            case PRODUCT_BUSINESS_N:
                version_type_ = SUITE_ENTERPRISE;
                break;
            //case PRODUCT_EDUCATION:
            //case PRODUCT_EDUCATION_N:
                version_type_ = SUITE_EDUCATION;
                break;
            case PRODUCT_HOME_BASIC:
            case PRODUCT_HOME_PREMIUM:
            case PRODUCT_STARTER:
            default:
                version_type_ = SUITE_HOME;
                break;
        }
    }
    else if (version_info.dwMajorVersion == 5 && version_info.dwMinorVersion == 2)
    {
        if (version_info.wProductType == VER_NT_WORKSTATION &&
            system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        {
            version_type_ = SUITE_PROFESSIONAL;
        }
        else if (version_info.wSuiteMask & VER_SUITE_WH_SERVER)
        {
            version_type_ = SUITE_HOME;
        }
        else
        {
            version_type_ = SUITE_SERVER;
        }
    }
    else if (version_info.dwMajorVersion == 5 && version_info.dwMinorVersion == 1)
    {
        if (version_info.wSuiteMask & VER_SUITE_PERSONAL)
            version_type_ = SUITE_HOME;
        else
            version_type_ = SUITE_PROFESSIONAL;
    }
    else
    {
        // Windows is pre XP so we don't care but pick a safe default.
        version_type_ = SUITE_HOME;
    }
}

OSInfo::~OSInfo() = default;

Version OSInfo::kernel32Version() const
{
    base::Version base_version = kernel32BaseVersion();

    static const Version kernel32_version =
        majorMinorBuildToVersion(base_version.components()[0],
                                 base_version.components()[1],
                                 base_version.components()[2]);
    return kernel32_version;
}

// Retrieve a version from kernel32. This is useful because when running in compatibility mode for
// a down-level version of the OS, the file version of kernel32 will still be the "real" version.
base::Version OSInfo::kernel32BaseVersion() const
{
    static const base::Version version([]
    {
        std::unique_ptr<FileVersionInfo> file_version_info =
            FileVersionInfo::createFileVersionInfo(L"kernel32.dll");
        if (!file_version_info)
        {
            // crbug.com/912061: on some systems it seems kernel32.dll might be corrupted or not in
            // a state to get version info. In this case try kernelbase.dll as a fallback.
            file_version_info = FileVersionInfo::createFileVersionInfo(L"kernelbase.dll");
        }

        CHECK(file_version_info);

        const uint32_t major = HIWORD(file_version_info->fixed_file_info()->dwFileVersionMS);
        const uint32_t minor = LOWORD(file_version_info->fixed_file_info()->dwFileVersionMS);
        const uint32_t build = HIWORD(file_version_info->fixed_file_info()->dwFileVersionLS);
        const uint32_t patch = LOWORD(file_version_info->fixed_file_info()->dwFileVersionLS);

        return base::Version(std::vector<uint32_t>{major, minor, build, patch});
    }());

    return version;
}

std::string OSInfo::processorModelName()
{
    if (processor_model_name_.empty())
    {
        const wchar_t kProcessorNameString[] =
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";

        base::win::RegistryKey key(HKEY_LOCAL_MACHINE, kProcessorNameString, KEY_READ);

        std::wstring value;
        key.readValue(L"ProcessorNameString", &value);

        processor_model_name_ = UTF8fromUTF16(value);
    }

    return processor_model_name_;
}

// static
OSInfo::WOW64Status OSInfo::wow64StatusForProcess(HANDLE process_handle)
{
    typedef BOOL(WINAPI* IsWow64ProcessFunc)(HANDLE, PBOOL);

    IsWow64ProcessFunc is_wow64_process = reinterpret_cast<IsWow64ProcessFunc>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process"));
    if (!is_wow64_process)
        return WOW64_DISABLED;

    BOOL is_wow64 = FALSE;
    if (!(*is_wow64_process)(process_handle, &is_wow64))
        return WOW64_UNKNOWN;

    return is_wow64 ? WOW64_ENABLED : WOW64_DISABLED;
}

Version windowsVersion()
{
    return OSInfo::instance()->version();
}

} // namespace base::win
