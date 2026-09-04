#ifndef USB_EJECT_WIN32_COMPAT_H
#define USB_EJECT_WIN32_COMPAT_H

#include <windows.h>

typedef ULONG DEVINST;
typedef DEVINST *PDEVINST;
typedef ULONG CONFIGRET;
typedef PVOID HDEVINFO;

typedef enum _PNP_VETO_TYPE {
    PNP_VetoTypeUnknown = 0,
    PNP_VetoLegacyDevice,
    PNP_VetoPendingClose,
    PNP_VetoWindowsApp,
    PNP_VetoWindowsService,
    PNP_VetoOutstandingOpen,
    PNP_VetoDevice,
    PNP_VetoDriver,
    PNP_VetoIllegalDeviceRequest,
    PNP_VetoInsufficientPower,
    PNP_VetoNonDisableable,
    PNP_VetoLegacyDriver,
    PNP_VetoInsufficientRights,
    PNP_VetoAlreadyRemoved
} PNP_VETO_TYPE;

typedef struct _SP_DEVICE_INTERFACE_DATA {
    DWORD cbSize;
    GUID InterfaceClassGuid;
    DWORD Flags;
    ULONG_PTR Reserved;
} SP_DEVICE_INTERFACE_DATA, *PSP_DEVICE_INTERFACE_DATA;

typedef struct _SP_DEVINFO_DATA {
    DWORD cbSize;
    GUID ClassGuid;
    DWORD DevInst;
    ULONG_PTR Reserved;
} SP_DEVINFO_DATA, *PSP_DEVINFO_DATA;

typedef struct _SP_DEVICE_INTERFACE_DETAIL_DATA_W {
    DWORD cbSize;
    WCHAR DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_W, *PSP_DEVICE_INTERFACE_DETAIL_DATA_W;

typedef struct _STORAGE_DEVICE_NUMBER {
    DWORD DeviceType;
    DWORD DeviceNumber;
    DWORD PartitionNumber;
} STORAGE_DEVICE_NUMBER;

typedef struct _STORAGE_PROPERTY_QUERY {
    DWORD PropertyId;
    DWORD QueryType;
    BYTE AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY;

typedef struct _STORAGE_DESCRIPTOR_HEADER {
    DWORD Version;
    DWORD Size;
} STORAGE_DESCRIPTOR_HEADER;

typedef struct _STORAGE_DEVICE_DESCRIPTOR {
    DWORD Version;
    DWORD Size;
    BYTE DeviceType;
    BYTE DeviceTypeModifier;
    BYTE RemovableMedia;
    BYTE CommandQueueing;
    DWORD VendorIdOffset;
    DWORD ProductIdOffset;
    DWORD ProductRevisionOffset;
    DWORD SerialNumberOffset;
    DWORD BusType;
    DWORD RawPropertiesLength;
    BYTE RawDeviceProperties[1];
} STORAGE_DEVICE_DESCRIPTOR;

#define CR_SUCCESS 0
#define CR_NO_SUCH_DEVNODE 0x0000000d

#define DIGCF_PRESENT 0x00000002
#define DIGCF_DEVICEINTERFACE 0x00000010

#define STORAGE_DEVICE_PROPERTY 0
#define PROPERTY_STANDARD_QUERY 0
#define BUS_TYPE_1394 4
#define BUS_TYPE_USB 7

#define IOCTL_STORAGE_CHECK_VERIFY2 0x002d0800
#define IOCTL_STORAGE_GET_DEVICE_NUMBER 0x002d1080
#define IOCTL_STORAGE_QUERY_PROPERTY 0x002d1400
#define IOCTL_STORAGE_EJECT_MEDIA 0x002d4808

extern const GUID USB_EJECT_GUID_DEVINTERFACE_DISK;

__declspec(dllimport) HDEVINFO WINAPI SetupDiGetClassDevsW(
    const GUID *, LPCWSTR, HWND, DWORD);
__declspec(dllimport) BOOL WINAPI SetupDiEnumDeviceInterfaces(
    HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD,
    PSP_DEVICE_INTERFACE_DATA);
__declspec(dllimport) BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(
    HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD,
    PSP_DEVINFO_DATA);
__declspec(dllimport) BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO);

__declspec(dllimport) CONFIGRET WINAPI CM_Get_Parent(
    PDEVINST, DEVINST, ULONG);
__declspec(dllimport) CONFIGRET WINAPI CM_Get_Device_ID_Size(
    PULONG, DEVINST, ULONG);
__declspec(dllimport) CONFIGRET WINAPI CM_Get_Device_IDW(
    DEVINST, PWSTR, ULONG, ULONG);
__declspec(dllimport) CONFIGRET WINAPI CM_Request_Device_EjectW(
    DEVINST, PNP_VETO_TYPE *, PWSTR, ULONG, ULONG);

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

__declspec(dllimport) BOOL WINAPI QueryFullProcessImageNameW(
    HANDLE, DWORD, LPWSTR, PDWORD);

typedef HANDLE SC_HANDLE;

typedef struct _SERVICE_STATUS_PROCESS {
    DWORD dwServiceType;
    DWORD dwCurrentState;
    DWORD dwControlsAccepted;
    DWORD dwWin32ExitCode;
    DWORD dwServiceSpecificExitCode;
    DWORD dwCheckPoint;
    DWORD dwWaitHint;
    DWORD dwProcessId;
    DWORD dwServiceFlags;
} SERVICE_STATUS_PROCESS;

typedef struct _ENUM_SERVICE_STATUS_PROCESSW {
    LPWSTR lpServiceName;
    LPWSTR lpDisplayName;
    SERVICE_STATUS_PROCESS ServiceStatusProcess;
} ENUM_SERVICE_STATUS_PROCESSW;

typedef struct _SERVICE_STATUS {
    DWORD dwServiceType;
    DWORD dwCurrentState;
    DWORD dwControlsAccepted;
    DWORD dwWin32ExitCode;
    DWORD dwServiceSpecificExitCode;
    DWORD dwCheckPoint;
    DWORD dwWaitHint;
} SERVICE_STATUS;

#ifndef SC_ENUM_PROCESS_INFO
#define SC_ENUM_PROCESS_INFO 0
#endif
#ifndef SC_STATUS_PROCESS_INFO
#define SC_STATUS_PROCESS_INFO 0
#endif
#ifndef SC_MANAGER_CONNECT
#define SC_MANAGER_CONNECT 0x0001
#endif
#ifndef SC_MANAGER_ENUMERATE_SERVICE
#define SC_MANAGER_ENUMERATE_SERVICE 0x0004
#endif
#ifndef SERVICE_QUERY_STATUS
#define SERVICE_QUERY_STATUS 0x0004
#endif
#ifndef SERVICE_STOP
#define SERVICE_STOP 0x0020
#endif
#ifndef SERVICE_CONTROL_STOP
#define SERVICE_CONTROL_STOP 0x00000001
#endif
#ifndef SERVICE_STOPPED
#define SERVICE_STOPPED 0x00000001
#endif
#ifndef SERVICE_STOP_PENDING
#define SERVICE_STOP_PENDING 0x00000003
#endif
#ifndef SERVICE_ACCEPT_STOP
#define SERVICE_ACCEPT_STOP 0x00000001
#endif
#ifndef SERVICE_WIN32
#define SERVICE_WIN32 0x00000030
#endif
#ifndef SERVICE_ACTIVE
#define SERVICE_ACTIVE 0x00000001
#endif

__declspec(dllimport) SC_HANDLE WINAPI OpenSCManagerW(
    LPCWSTR, LPCWSTR, DWORD);
__declspec(dllimport) BOOL WINAPI EnumServicesStatusExW(
    SC_HANDLE, int, DWORD, DWORD, LPBYTE, DWORD,
    PDWORD, PDWORD, PDWORD, LPCWSTR);
__declspec(dllimport) BOOL WINAPI CloseServiceHandle(SC_HANDLE);
__declspec(dllimport) SC_HANDLE WINAPI OpenServiceW(
    SC_HANDLE, LPCWSTR, DWORD);
__declspec(dllimport) BOOL WINAPI ControlService(
    SC_HANDLE, DWORD, SERVICE_STATUS *);
__declspec(dllimport) BOOL WINAPI QueryServiceStatusEx(
    SC_HANDLE, int, LPBYTE, DWORD, PDWORD);

#define USB_EJECT_STATIC_ASSERT(name, expression) \
    typedef char static_assert_##name[(expression) ? 1 : -1]

USB_EJECT_STATIC_ASSERT(sp_interface_data_x64,
    sizeof(SP_DEVICE_INTERFACE_DATA) == 32);
USB_EJECT_STATIC_ASSERT(sp_devinfo_data_x64,
    sizeof(SP_DEVINFO_DATA) == 32);
USB_EJECT_STATIC_ASSERT(storage_device_number,
    sizeof(STORAGE_DEVICE_NUMBER) == 12);
USB_EJECT_STATIC_ASSERT(service_status_process,
    sizeof(SERVICE_STATUS_PROCESS) == 36);

#endif
