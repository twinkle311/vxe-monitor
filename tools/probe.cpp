// probe.cpp — VXE/ATK COMPX HID 协议实测工具 (控制台)
// 枚举 HID 接口、定位鼠标命令通道,实测 cmd 1/3/4/16/18 并输出原始字节。
// 用法: probe.exe [--all]   (--all 时打印全部 HID 接口)
#include "../src/vxe_hid.h"
#include <cstdio>

using namespace vxe;

static void HexDump(const char* tag, const BYTE* b, int n) {
    printf("    %s (%d):", tag, n);
    for (int i = 0; i < n && i < 64; ++i) printf(" %02X", b[i]);
    printf("\n");
}

static void PrintBody(const char* tag, BYTE cmd, const BYTE body[GO_SIZE]) {
    HexDump(tag, body, GO_SIZE);
    if (cmd == CMD_BATTERY) {
        BatteryInfo b = ParseBattery(body);
        printf("    => 电量 %d%%  %s  电压 %d mV\n",
               b.level, b.charging ? "充电中" : "放电中", b.voltageMv);
    } else if (cmd == CMD_DOWNL0AD_DATA) {
        printf("    => cid=%d mid=%d connectType=%d (%S)\n",
               body[GO_DATA_OFF + 4], body[GO_DATA_OFF + 5],
               body[GO_DATA_OFF + 6], ConnectTypeName(body[GO_DATA_OFF + 6]));
    } else if (cmd == CMD_MOUSE_ONLINE) {
        printf("    => 在线=%d rfId=%d-%d-%d\n", body[GO_DATA_OFF],
               body[GO_DATA_OFF + 3], body[GO_DATA_OFF + 2], body[GO_DATA_OFF + 1]);
    } else if (cmd == CMD_CID_MID) {
        printf("    => CID=%d MID=%d uniqueId=%02X.%02X.%02X.%02X\n",
               body[GO_DATA_OFF], body[GO_DATA_OFF + 1],
               body[GO_DATA_OFF + 6], body[GO_DATA_OFF + 7],
               body[GO_DATA_OFF + 8], body[GO_DATA_OFF + 9]);
    } else if (cmd == CMD_VERSION) {
        printf("    => 版本 %x.%02x\n", body[GO_DATA_OFF], body[GO_DATA_OFF + 1]);
    }
}

// 发送一次并把所有收到的原始报文都倒出来(带命令号识别)。
static bool ProbeOnce(Transport& t, const char* title, BYTE cmd,
                      const BYTE* data, BYTE dataLen,
                      int method, int fid, DWORD waitMs) {
    printf("\n[尝试] %s (method=%s id=%d cmd=%u)\n", title,
           method == 1 ? "WriteFile/Output" : "SetFeature/Feature", fid, cmd);

    BYTE pkt[GO_SIZE];
    BuildPacket(pkt, cmd, data, dataLen);
    HexDump("TX", pkt, GO_SIZE);
    if (!SendPacket(t, pkt, method, fid)) {
        printf("    发送失败 (err=%lu)\n", GetLastError());
        return false;
    }

    bool hit = false;
    BYTE raw[512];
    for (int i = 0; i < 8; ++i) {
        int got = ReadRawOnce(t, raw, waitMs);
        if (got <= 0) break;
        HexDump("RX raw", raw, got);
        BYTE body[GO_SIZE];
        if (FindResponseBody(raw, got, cmd, body)) {
            PrintBody("RX body", cmd, body);
            hit = true;
        } else if (got > 1) {
            // 报告可能是异步通知(gb.dataReporting 等),看看像不像别的命令应答
            for (int off = 0; off <= 1; ++off) {
                BYTE c = raw[off];
                if (c == CMD_BATTERY || c == CMD_DOWNL0AD_DATA || c == CMD_MOUSE_ONLINE ||
                    c == CMD_CID_MID || c == CMD_VERSION) {
                    BYTE b2[GO_SIZE];
                    memcpy(b2, raw + off, GO_SIZE);
                    printf("    (疑似 cmd=%u 的应答,body 偏移 %d)\n", c, off);
                    PrintBody("RX body?", c, b2);
                    hit = hit || (c == cmd);
                }
            }
        }
    }
    if (!hit) printf("    未收到 cmd=%u 的应答\n", cmd);
    return hit;
}

static void Drain(Transport& t) {
    BYTE raw[512];
    while (ReadRawOnce(t, raw, 60) > 0) {}
}

// 解析 HID 报告描述符,输出每种报告 ID 的 Input/Output/Feature 字节长度
static void DumpReportMap(const WCHAR* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    BYTE desc[4096];
    memset(desc, 0, sizeof(desc));
    typedef BOOLEAN(WINAPI* PFN_HidD_GetDescriptor)(HANDLE, UCHAR, PVOID, ULONG);
    static PFN_HidD_GetDescriptor pHidD_GetDescriptor =
        (PFN_HidD_GetDescriptor)GetProcAddress(LoadLibraryW(L"hid.dll"), "HidD_GetDescriptor");
    if (!pHidD_GetDescriptor ||
        !pHidD_GetDescriptor(h, 0x22 /*HID_DT_REPORT*/, desc, sizeof(desc))) {
        printf("  (无法读取报告描述符)\n");
        CloseHandle(h);
        return;
    }
    size_t len = sizeof(desc);
    printf("\n=== 报告描述符 Report ID 映射 ===\n");
    struct Rpt { unsigned bits[3]; };
    Rpt map[32];
    int ids[32], nids = 0;
    memset(map, 0, sizeof(map));
    unsigned rptSize = 0, rptCount = 0, rptId = 0;
    size_t i = 0;
    while (i + 1 < len) {
        BYTE prefix = desc[i];
        if (prefix == 0x00) break; // 描述符结束(其后为填充 0)
        if (prefix == 0xFE) { i += 2 + desc[i + 1]; continue; } // long item
        unsigned size = prefix & 3;
        if (size == 3) size = 4;
        BYTE type = (prefix >> 2) & 3;
        BYTE tag = (prefix >> 4) & 0xF;
        unsigned data = 0;
        for (unsigned k = 0; k < size && i + 1 + k < len; ++k) data |= (unsigned)desc[i + 1 + k] << (8 * k);
        if (type == 1) { // global
            if (tag == 7) rptId = data;        // Report ID
            if (tag == 8) rptSize = data;      // Report Size
            if (tag == 9) rptCount = data;     // Report Count
        } else if (type == 0 && tag == 8) {    // main: Input/Output/Feature
            int t2 = (prefix == 0x80) ? 0 : (prefix == 0x90) ? 1 : (prefix == 0xB0) ? 2 : -1;
            if (t2 >= 0) {
                int slot = -1;
                for (int k = 0; k < nids; ++k) if (ids[k] == (int)rptId) { slot = k; break; }
                if (slot < 0 && nids < 32) { slot = nids++; ids[slot] = (int)rptId; }
                if (slot >= 0) map[slot].bits[t2] += rptSize * rptCount;
            }
        }
        i += 1 + size;
    }
    for (int k = 0; k < nids; ++k) {
        printf("  Report ID %2d: in=%u out=%u feat=%u (字节,含ID)\n", ids[k],
               (map[k].bits[0] + 7) / 8 + 1, (map[k].bits[1] + 7) / 8 + 1,
               (map[k].bits[2] + 7) / 8 + 1);
    }
    CloseHandle(h);
}

int wmain(int argc, wchar_t** argv) {
    bool showAll = (argc > 1 && wcscmp(argv[1], L"--all") == 0);
    SetConsoleOutputCP(65001);

    static HidEntry list[256];
    int n = EnumDevices(list, 256);
    printf("共发现 %d 个 HID 接口。\n", n);

    for (int i = 0; i < n; ++i) {
        const HidEntry& e = list[i];
        bool interesting = IsVxeVid(e.vid) || e.isCompx;
        if (!showAll && !interesting) continue;
        printf("[%3d] %04X:%04X  usage=%04X:%04X  in/out/feat=%u/%u/%u  %S\n",
               i, e.vid, e.pid, e.usagePage, e.usage,
               e.inputLen, e.outputLen, e.featureLen, e.product);
        if (interesting) printf("      %S\n", e.path);
    }

    int idx = FindCompxDevice(list, n);
    if (idx < 0) {
        printf("\n[!] 未找到 COMPX 命令通道 (usagePage=FF04 usage=02)。\n");
        printf("    请确认鼠标接收器已插入;若设备在上表中但 usage 不同,把上表贴出来分析。\n");
        return 1;
    }

    const HidEntry& e = list[idx];
    printf("\n=== 目标: %S\n    %04X:%04X usage=%04X:%04X in=%u out=%u feat=%u\n",
           e.product, e.vid, e.pid, e.usagePage, e.usage,
           e.inputLen, e.outputLen, e.featureLen);

    Transport t;
    if (!OpenTransport(e, &t)) {
        printf("[!] 打开设备失败 (err=%lu)。请先退出 ATK HUB / 其他驱动软件后重试。\n", GetLastError());
        return 1;
    }
    DumpReportMap(e.path);

    BYTE nonce[4] = {0x12, 0x34, 0x56, 0x78};
    bool okBattery = false;

    // 阶段 1:Output 报文 (Report ID 8,同 Web 驱动) —— cmd 1 握手 + cmd 4 电量
    if (t.outLen > 0) {
        Drain(t);
        ProbeOnce(t, "握手+连接类型", CMD_DOWNL0AD_DATA, nonce, 8, 1, OUT_REPORT_ID, 400);
        Drain(t);
        okBattery = ProbeOnce(t, "电量查询", CMD_BATTERY, nullptr, 0, 1, OUT_REPORT_ID, 400);
        if (!okBattery) {
            Drain(t);
            okBattery = ProbeOnce(t, "电量查询(报告ID=0)", CMD_BATTERY, nullptr, 0, 1, 0, 400);
        }
    } else {
        printf("\n[跳过] 该接口没有 Output 报文 (outLen=0)\n");
    }

    // 阶段 2:Feature 报文回退,依次试 ID 8 / 2 / 1
    if (!okBattery) {
        const int fids[] = {8, 2, 1};
        for (int fid : fids) {
            Drain(t);
            if (ProbeOnce(t, "电量查询", CMD_BATTERY, nullptr, 0, 2, fid, 400)) {
                okBattery = true;
                break;
            }
        }
    }

    int m = t.method ? t.method : 1;
    Drain(t);
    ProbeOnce(t, "CID/MID 型号", CMD_CID_MID, nullptr, 0, m, t.featureId, 400);
    Drain(t);
    ProbeOnce(t, "在线状态", CMD_MOUSE_ONLINE, nullptr, 0, m, t.featureId, 400);
    Drain(t);
    ProbeOnce(t, "固件版本", CMD_VERSION, nullptr, 0, m, t.featureId, 400);

    // 被动监听:命令通道 + 同设备其余带输入的集合(捕捉设备主动上报)
    printf("\n=== 被动监听 1.2s/通道 (设备主动上报) ===\n");
    BYTE raw[512];
    for (;;) {
        int got = ReadRawOnce(t, raw, 1200);
        if (got <= 0) break;
        HexDump("PUSH(cmd通道)", raw, got);
    }
    t.Close();
    for (int i = 0; i < n; ++i) {
        const HidEntry& o = list[i];
        if (o.vid != e.vid || o.pid != e.pid || o.inputLen == 0) continue;
        Transport t2;
        if (!OpenTransport(o, &t2)) continue;
        printf("  -- %S (usage=%04X:%04X in=%u)\n", o.product, o.usagePage, o.usage, o.inputLen);
        for (;;) {
            int got = ReadRawOnce(t2, raw, 1200);
            if (got <= 0) break;
            HexDump("PUSH", raw, got);
        }
        t2.Close();
    }

    printf("\n======== 结论 ========\n%s\n", okBattery
           ? "电量读取成功!协议可用 (细节见上文 method)。"
           : "未能读到电量。请把上面全部输出发回分析(检查鼠标是否开机/在范围内)。");
    return okBattery ? 0 : 2;
}
