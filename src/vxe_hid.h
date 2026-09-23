// vxe_hid.h — VXE / ATK (COMPX 方案) 无线鼠标 HID 协议访问层
//
// 协议来源:逆向 ATK HUB Web 驱动 (https://v3-hub.atkgear.com/) v3.2.27。
// 命令包 (go 包) 为 16 字节,经 Report ID 8 的 Output 报文发送:
//   [0]     命令号 (Za 枚举)
//   [1]     命令状态
//   [2..3]  EEPROM 地址 (u16 LE, 一般为 0)
//   [4]     数据有效长度
//   [5..14] 数据区 (baseOffset = 5)
//   [15]    校验和 = (0x55 - ((0x08 + sum(byte[0..14])) & 0xFF)) & 0xFF
// 响应经 input report 返回,body[0] 回显命令号,布局同上。
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _MSC_VER
// MinGW 下由 build 脚本链接 -lsetupapi -lhid
#else
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace vxe {

// ---- 设备标识 ----------------------------------------------------------
constexpr USHORT VID_VGN = 0x3554; // VGN / VXE (R1SE 系列 2.4G 接收器)
constexpr USHORT VID_ATK = 0x373B; // ATK
constexpr USAGE COMPX_USAGEPAGE = 0xFF04; // 厂商命令通道所在顶层集合
constexpr USAGE COMPX_USAGE = 0x0002;
constexpr BYTE OUT_REPORT_ID = 8; // WebHID sendReport(8, ...) 对应的 Output 报文 ID

// ---- 命令号 (Za 枚举) ---------------------------------------------------
constexpr BYTE CMD_DOWNL0AD_DATA = 1;    // 握手/加密问候 + 连接类型 (aD)
constexpr BYTE CMD_MOUSE_ONLINE = 3;     // 无线鼠标在线状态 (mb)
constexpr BYTE CMD_BATTERY = 4;          // 电量查询 (WS)
constexpr BYTE CMD_CID_MID = 16;         // 鼠标 CID/MID (fb)
constexpr BYTE CMD_VERSION = 18;         // 鼠标固件版本 (hb)

constexpr int GO_SIZE = 16;
constexpr int GO_DATA_OFF = 5;

// 连接方式 (la 枚举,cmd 1 响应 deviceType 字段)
enum ConnectType {
    CT_DONGLE_1K = 0, CT_DONGLE_4K = 1, CT_WIRED_1K = 2, CT_WIRED_8K = 3,
    CT_DONGLE_2K = 4, CT_DONGLE_8K = 5, CT_WIRED_2K = 6, CT_WIRED_4K = 7,
    CT_NEARLINK = 8
};

// ---- go 包构造 / 解析 ---------------------------------------------------

inline void BuildPacket(BYTE pkt[GO_SIZE], BYTE cmd, const BYTE* data, BYTE dataLen) {
    memset(pkt, 0, GO_SIZE);
    pkt[0] = cmd;
    pkt[4] = dataLen;
    if (data && dataLen) {
        if (dataLen > GO_SIZE - GO_DATA_OFF - 1) dataLen = GO_SIZE - GO_DATA_OFF - 1;
        memcpy(&pkt[GO_DATA_OFF], data, dataLen);
    }
    unsigned sum = 8;
    for (int i = 0; i < GO_SIZE - 1; ++i) sum += pkt[i];
    pkt[GO_SIZE - 1] = (BYTE)(0x55 - (sum & 0xFF));
}

inline bool VerifyChecksum(const BYTE body[GO_SIZE]) {
    unsigned sum = 8;
    for (int i = 0; i < GO_SIZE; ++i) sum += body[i];
    return (sum & 0xFF) == 0x55;
}

// 在一份 input report 原始字节中定位响应 body。
// Windows ReadFile 返回的报告首字节为 Report ID,而 WebHID 层剥掉了它,
// 因此 body 可能位于偏移 0 或 1,两种对齐都尝试;优先取校验和正确的。
inline bool FindResponseBody(const BYTE* raw, int len, BYTE cmd, BYTE body[GO_SIZE]) {
    int found = -1;
    for (int off = 0; off <= 1 && off + GO_SIZE <= len; ++off) {
        if (raw[off] != cmd) continue;
        BYTE* cand = (BYTE*)body;
        memcpy(cand, raw + off, GO_SIZE);
        if (VerifyChecksum(cand)) return true;
        if (found < 0) found = off;
    }
    if (found >= 0) {
        memcpy(body, raw + found, GO_SIZE);
        return true;
    }
    return false;
}

// ---- 设备枚举 -----------------------------------------------------------

struct HidEntry {
    WCHAR path[MAX_PATH];
    USHORT vid, pid;
    USAGE usagePage, usage;
    USHORT inputLen, outputLen, featureLen;
    WCHAR product[64];
    bool isCompx;
};

inline bool ParseVidPid(const WCHAR* path, USHORT* vid, USHORT* pid) {
    WCHAR lower[MAX_PATH];
    lstrcpynW(lower, path, MAX_PATH);
    for (WCHAR* p = lower; *p; ++p) *p = (WCHAR)towlower(*p);
    const WCHAR* v = wcsstr(lower, L"vid_");
    const WCHAR* p2 = wcsstr(lower, L"pid_");
    if (!v || !p2) return false;
    *vid = (USHORT)wcstoul(v + 4, nullptr, 16);
    *pid = (USHORT)wcstoul(p2 + 4, nullptr, 16);
    return true;
}

// 枚举全部 HID 接口;返回数量(不超过 maxOut)。
inline int EnumDevices(HidEntry* out, int maxOut) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifData); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifData, nullptr, 0, &need, nullptr);
        if (!need) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, need, nullptr, nullptr)) {
            free(detail);
            continue;
        }

        HidEntry e;
        memset(&e, 0, sizeof(e));
        lstrcpynW(e.path, detail->DevicePath, MAX_PATH);
        free(detail);

        HANDLE h = CreateFileW(e.path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        bool openedRW = (h != INVALID_HANDLE_VALUE);
        if (!openedRW) {
            h = CreateFileW(e.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            e.vid = attr.VendorID;
            e.pid = attr.ProductID;
        } else {
            ParseVidPid(e.path, &e.vid, &e.pid);
        }
        HidD_GetProductString(h, e.product, sizeof(e.product));

        HIDP_CAPS caps;
        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(h, &prep)) {
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                e.usagePage = caps.UsagePage;
                e.usage = caps.Usage;
                e.inputLen = caps.InputReportByteLength;
                e.outputLen = caps.OutputReportByteLength;
                e.featureLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(prep);
        }
        CloseHandle(h);
        e.isCompx = (e.usagePage == COMPX_USAGEPAGE && e.usage == COMPX_USAGE);

        if (count < maxOut) out[count++] = e;
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

inline bool IsVxeVid(USHORT vid) { return vid == VID_VGN || vid == VID_ATK; }

// COMPX 命令通道判定。
// 实测 (VXE Mouse 1K Dongle 3554:F58E, R1SE 长续航版):
//   col05 usage=FF02:0002 → in/out 报文各 17 字节 (1 报告ID + 16字节 go 包) ← 命令通道
//   col06 usage=FF04:0002 → 仅 8 字节 feature,无法承载 go 包
inline bool IsCompxChannel(const HidEntry& e) {
    return IsVxeVid(e.vid) && e.outputLen >= 17 && e.inputLen >= 17;
}

// 选出 COMPX 命令通道;优先 in/out 长度匹配的集合,其次按 usagePage 推断。
inline int FindCompxDevice(const HidEntry* list, int n) {
    int bySize = -1, byPage = -1, fallback = -1;
    for (int i = 0; i < n; ++i) {
        const HidEntry& e = list[i];
        if (IsCompxChannel(e)) { bySize = i; break; }
        if (IsVxeVid(e.vid) && e.usage == COMPX_USAGE &&
            (e.usagePage == 0xFF02 || e.usagePage == COMPX_USAGEPAGE) && byPage < 0) {
            byPage = i;
        }
    }
    if (bySize >= 0) return bySize;
    if (byPage >= 0) return byPage;
    for (int i = 0; i < n; ++i) {
        if (list[i].isCompx && fallback < 0) fallback = i;
    }
    return fallback;
}

// ---- 传输层 -------------------------------------------------------------

struct Transport {
    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT outLen = 0, inLen = 0, featureLen = 0;
    int method = 0;      // 0=未确定 1=WriteFile(Output 报文) 2=HidD_SetFeature
    int featureId = OUT_REPORT_ID; // method=2 时命中的 Feature Report ID

    void Close() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        method = 0;
    }
};

inline bool OpenTransport(const HidEntry& e, Transport* t) {
    t->Close();
    t->h = CreateFileW(e.path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (t->h == INVALID_HANDLE_VALUE) {
        t->h = CreateFileW(e.path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    }
    if (t->h == INVALID_HANDLE_VALUE) return false;
    t->outLen = e.outputLen;
    t->inLen = e.inputLen;
    t->featureLen = e.featureLen;
    t->method = 0;
    return true;
}

// 读取一份 input report;超时返回 0,出错返回 -1。
inline int ReadRawOnce(Transport& t, BYTE* buf, DWORD timeoutMs) {
    if (t.inLen == 0) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = ReadFile(t.h, buf, t.inLen, &got, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return -1;
        }
        DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (w != WAIT_OBJECT_0) {
            CancelIo(t.h);
            GetOverlappedResult(t.h, &ov, &got, TRUE);
            CloseHandle(ov.hEvent);
            return 0;
        }
        if (!GetOverlappedResult(t.h, &ov, &got, FALSE)) {
            CloseHandle(ov.hEvent);
            return -1;
        }
    }
    CloseHandle(ov.hEvent);
    return (int)got;
}

// 把 16 字节命令包写出去。method 1: Output 报文;method 2: Feature 报文。
// reportId 为线上报告 ID(置发送缓冲区首字节)。
inline bool SendPacket(Transport& t, const BYTE pkt[GO_SIZE], int method, int reportId) {
    if (method == 1) {
        if (t.outLen == 0) return false;
        BYTE buf[512];
        if (t.outLen > sizeof(buf)) return false;
        memset(buf, 0, t.outLen);
        buf[0] = (BYTE)reportId;
        memcpy(buf + 1, pkt, GO_SIZE);
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD wrote = 0;
        BOOL ok = WriteFile(t.h, buf, t.outLen, &wrote, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            if (!GetOverlappedResult(t.h, &ov, &wrote, TRUE)) {
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        return wrote == t.outLen;
    }
    if (method == 2) {
        if (t.featureLen == 0) return false;
        BYTE buf[512];
        if (t.featureLen > sizeof(buf)) return false;
        memset(buf, 0, t.featureLen);
        buf[0] = (BYTE)reportId;
        memcpy(buf + 1, pkt, GO_SIZE);
        return HidD_SetFeature(t.h, buf, t.featureLen) == TRUE;
    }
    return false;
}

// 发送命令并等待匹配响应。自动尝试 Output 报文(ID 8),失败再试 Feature 报文。
// 成功时 body(16 字节,布局见文件头)写入 resp。返回 true/false。
inline bool Query(Transport& t, BYTE cmd, const BYTE* data, BYTE dataLen, BYTE resp[GO_SIZE],
                  DWORD timeoutMs = 500, int retries = 2) {
    BYTE pkt[GO_SIZE];
    BuildPacket(pkt, cmd, data, dataLen);

    // 尝试顺序:(method, reportId)
    const int order[4][2] = {
        {1, OUT_REPORT_ID}, {1, 0}, {2, OUT_REPORT_ID}, {2, 2},
    };

    static const int MAXRAW = 512;
    BYTE raw[MAXRAW];

    for (int m = 0; m < 4; ++m) {
        int method = order[m][0], rid = order[m][1];
        if (method == 1 && t.outLen == 0) continue;
        if (method == 2 && t.featureLen == 0) continue;
        // 已验证过的方式优先且只用它
        if (t.method != 0 && !(method == t.method &&
                               (method == 1 || rid == t.featureId))) {
            if (m > 0) continue;
        }

        for (int attempt = 0; attempt <= retries; ++attempt) {
            if (!SendPacket(t, pkt, method, rid)) break;

            if (method == 2) {
                // Feature 方式的应答可能走 GetFeature,也可能仍从 input report 推回
                BYTE fbuf[512];
                memset(fbuf, 0, t.featureLen);
                fbuf[0] = (BYTE)rid;
                if (HidD_GetFeature(t.h, fbuf, t.featureLen)) {
                    if (FindResponseBody(fbuf, t.featureLen, cmd, resp)) {
                        t.method = 2; t.featureId = rid;
                        return true;
                    }
                }
            }
            for (;;) {
                int got = ReadRawOnce(t, raw, timeoutMs);
                if (got <= 0) break;
                if (FindResponseBody(raw, got, cmd, resp)) {
                    t.method = method;
                    if (method == 2) t.featureId = rid;
                    return true;
                }
            }
        }
    }
    return false;
}

// 便捷封装:解析后的常用字段 -----------------------------------------------

struct BatteryInfo {
    int level = 0;        // 0..100
    bool charging = false;
    int voltageMv = 0;
};

inline BatteryInfo ParseBattery(const BYTE body[GO_SIZE]) {
    BatteryInfo b;
    b.level = body[GO_DATA_OFF];
    if (b.level > 100) b.level = 100;
    b.charging = (body[GO_DATA_OFF + 1] == 1);
    // 电压为大端 u16 (Web 驱动 DataView.getUint16 默认大端);实测 R1SE: 0F EA = 4074 mV
    b.voltageMv = (body[GO_DATA_OFF + 2] << 8) | body[GO_DATA_OFF + 3];
    return b;
}

// cmd 1 (aD): 请求带 4 字节随机问候;响应 cid=body[9], mid=body[10], deviceType=body[11]
inline int QueryConnectType(Transport& t, BYTE* cidOut = nullptr, BYTE* midOut = nullptr) {
    BYTE nonce[4];
    for (int i = 0; i < 4; ++i) nonce[i] = (BYTE)(GetTickCount() >> (i * 5));
    BYTE resp[GO_SIZE];
    if (!Query(t, CMD_DOWNL0AD_DATA, nonce, 8, resp)) return -1;
    if (cidOut) *cidOut = resp[GO_DATA_OFF + 4];
    if (midOut) *midOut = resp[GO_DATA_OFF + 5];
    return resp[GO_DATA_OFF + 6];
}

inline bool QueryOnline(Transport& t) {
    BYTE resp[GO_SIZE];
    if (!Query(t, CMD_MOUSE_ONLINE, nullptr, 0, resp)) return false;
    return resp[GO_DATA_OFF] == 1;
}

inline bool QueryBattery(Transport& t, BatteryInfo* out) {
    BYTE resp[GO_SIZE];
    if (!Query(t, CMD_BATTERY, nullptr, 0, resp)) return false;
    *out = ParseBattery(resp);
    return true;
}

inline bool QueryCidMid(Transport& t, BYTE* cid, BYTE* mid, BYTE uniqueId[4]) {
    BYTE resp[GO_SIZE];
    if (!Query(t, CMD_CID_MID, nullptr, 0, resp)) return false;
    *cid = resp[GO_DATA_OFF];
    *mid = resp[GO_DATA_OFF + 1];
    if (uniqueId) memcpy(uniqueId, &resp[GO_DATA_OFF + 6], 4);
    return true;
}

// cmd 18 (hb): 版本 = "major.minor"(十六进制字符串,同 Web 驱动)
inline bool QueryVersion(Transport& t, char* out, size_t outLen) {
    BYTE resp[GO_SIZE];
    if (!Query(t, CMD_VERSION, nullptr, 0, resp)) return false;
    snprintf(out, outLen, "%x.%02x", resp[GO_DATA_OFF], resp[GO_DATA_OFF + 1]);
    return true;
}

inline const wchar_t* ConnectTypeName(int ct) {
    switch (ct) {
        case CT_DONGLE_1K: return L"2.4G 1K";
        case CT_DONGLE_2K: return L"2.4G 2K";
        case CT_DONGLE_4K: return L"2.4G 4K";
        case CT_DONGLE_8K: return L"2.4G 8K";
        case CT_WIRED_1K: return L"有线 1K";
        case CT_WIRED_2K: return L"有线 2K";
        case CT_WIRED_4K: return L"有线 4K";
        case CT_WIRED_8K: return L"有线 8K";
        case CT_NEARLINK: return L"Nearlink";
        default: return L"未知";
    }
}

} // namespace vxe
