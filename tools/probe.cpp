// probe.cpp — VXE/ATK COMPX HID 协议实测工具 (控制台)
// 枚举 HID 接口,对每个命令通道(接收器/有线直连)实测 cmd 1/3/4/16/18 并输出原始字节。
// 用法: probe.exe [--all] [--map]   (--all 打印全部 HID 接口, --map 附报告描述符映射)
#include "../src/vxe_hid.h"
#include <cstdio>

using namespace vxe;

static void HexDump(const char* tag, const BYTE* b, int n) {
    printf("    %s (%d):", tag, n);
    for (int i = 0; i < n && i < 64; ++i) printf(" %02X", b[i]);
    printf("\n");
}

// 连接方式 ASCII 名(控制台 %S 无法输出中文宽字符,仅用于 probe 显示)
static const char* CtAscii(int ct) {
    switch (ct) {
        case 0: return "2.4G 1K";
        case 1: return "2.4G 4K";
        case 2: return "Wired 1K";
        case 3: return "Wired 8K";
        case 4: return "2.4G 2K";
        case 5: return "2.4G 8K";
        case 6: return "Wired 2K";
        case 7: return "Wired 4K";
        case 8: return "Nearlink";
        default: return "Unknown";
    }
}

static void PrintBody(const char* tag, BYTE cmd, const BYTE body[GO_SIZE]) {
    HexDump(tag, body, GO_SIZE);
    if (cmd == CMD_BATTERY) {
        BatteryInfo b = ParseBattery(body);
        printf("    => 电量 %d%%  %s  电压 %d mV  (充电字节=0x%02X)\n",
               b.level, b.charging ? "充电中" : "放电中", b.voltageMv,
               body[GO_DATA_OFF + 1]);
    } else if (cmd == CMD_DOWNL0AD_DATA) {
        printf("    => cid=%d mid=%d connectType=%d (%s)\n",
               body[GO_DATA_OFF + 4], body[GO_DATA_OFF + 5],
               body[GO_DATA_OFF + 6], CtAscii(body[GO_DATA_OFF + 6]));
    } else if (cmd == CMD_MOUSE_ONLINE) {
        printf("    => 在线=%d\n", body[GO_DATA_OFF]);
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
                      int method, int rid, DWORD waitMs) {
    printf("\n[尝试] %s (method=%s id=%d cmd=%u)\n", title,
           method == 1 ? "WriteFile/Output" : "SetFeature/Feature", rid, cmd);

    BYTE pkt[GO_SIZE];
    BuildPacket(pkt, cmd, data, dataLen);
    HexDump("TX", pkt, GO_SIZE);
    if (!SendPacket(t, pkt, method, rid)) {
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
        CloseHandle(h);
        return;
    }
    size_t len = sizeof(desc);
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
            if (tag == 7) rptId = data;
            if (tag == 8) rptSize = data;
            if (tag == 9) rptCount = data;
        } else if (type == 0 && tag == 8) { // main: Input/Output/Feature
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
    printf("  [报告映射]");
    for (int k = 0; k < nids; ++k) {
        printf("  ID %d: in=%u out=%u feat=%u", ids[k],
               (map[k].bits[0] + 7) / 8 + 1, (map[k].bits[1] + 7) / 8 + 1,
               (map[k].bits[2] + 7) / 8 + 1);
    }
    printf("\n");
    CloseHandle(h);
}

int wmain(int argc, wchar_t** argv) {
    bool showAll = false, showMap = false, rawMode = false, seqMode = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--all") == 0) showAll = true;
        if (wcscmp(argv[i], L"--map") == 0) showMap = true;
        if (wcscmp(argv[i], L"--raw") == 0) rawMode = true;
        if (wcscmp(argv[i], L"--seq") == 0) seqMode = true;
    }
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
    }

    // 对所有候选命令通道逐一实测(接收器 + 有线直连可能同时在场)
    bool anyBattery = false;
    int tested = 0;
    for (int tIdx = 0; tIdx < n; ++tIdx) {
        const HidEntry& e = list[tIdx];
        if (!IsCompxChannel(e)) continue;
        ++tested;
        printf("\n########## 命令通道 [%d] %04X:%04X  %S ##########\n",
               tIdx, e.vid, e.pid, e.product);
        printf("    in=%u out=%u feat=%u\n", e.inputLen, e.outputLen, e.featureLen);
        if (showMap) DumpReportMap(e.path);

        Transport t;
        if (!OpenTransport(e, &t)) {
            printf("    [!] 打开失败 (err=%lu)\n", GetLastError());
            continue;
        }

        BYTE nonce[4] = {0x12, 0x34, 0x56, 0x78};
        Drain(t);

        if (seqMode) {
            // 决定性测试:电池(此时醒着才成功) → cmd3 → 电池,同一句柄对比;
            // 首次电池失败则视为休眠,1.2s 后重试(最多 10 轮,观察是否周期性醒)。
            bool verdictDone = false;
            for (int round = 1; round <= 10 && !verdictDone; ++round) {
                Drain(t);
                printf("\n--- seq 第 %d 轮 ---\n", round);
                bool bat1 = ProbeOnce(t, "电量①", CMD_BATTERY, nullptr, 0, 1, OUT_REPORT_ID, 700);
                if (!bat1) {
                    printf("    >> 无应答:鼠标可能休眠/关机,1.2s 后重试\n");
                    Sleep(1200);
                    continue;
                }
                Drain(t);
                ProbeOnce(t, "cmd3 插入", CMD_MOUSE_ONLINE, nullptr, 0, 1, OUT_REPORT_ID, 700);
                Drain(t);
                bool bat2 = ProbeOnce(t, "电量②(cmd3之后)", CMD_BATTERY, nullptr, 0, 1, OUT_REPORT_ID, 700);
                printf("\n==== 判定:电量①=%s  cmd3=已发  电量②=%s => %s ====\n",
                       bat1 ? "OK" : "FAIL", bat2 ? "OK" : "FAIL",
                       bat2 ? "cmd3 不影响电量查询" : "!!! cmd3 之后电量查询被破坏(毒化实锤)");
                verdictDone = true;
            }
            if (!verdictDone) printf("\n==== 判定:10 轮内鼠标始终未应答(持续休眠/关机) ====\n");
            t.Close();
            continue;
        }

        if (rawMode) {
            printf("\n[--raw 模式:模拟程序顺序 cmd3 -> cmd4,跳过 cmd1 握手]\n");
            ProbeOnce(t, "在线状态(先发)", CMD_MOUSE_ONLINE, nullptr, 0, 1, OUT_REPORT_ID, 500);
            Drain(t);
        } else {
            ProbeOnce(t, "握手+连接类型", CMD_DOWNL0AD_DATA, nonce, 8, 1, OUT_REPORT_ID, 400);
            Drain(t);
        }
        bool ok = ProbeOnce(t, "电量查询", CMD_BATTERY, nullptr, 0, 1, OUT_REPORT_ID, 500);
        Drain(t);
        ProbeOnce(t, "在线状态", CMD_MOUSE_ONLINE, nullptr, 0, 1, OUT_REPORT_ID, 400);
        Drain(t);
        ProbeOnce(t, "CID/MID 型号", CMD_CID_MID, nullptr, 0, 1, OUT_REPORT_ID, 400);
        Drain(t);
        ProbeOnce(t, "固件版本", CMD_VERSION, nullptr, 0, 1, OUT_REPORT_ID, 400);

        printf("  -- 被动监听 0.8s --\n");
        BYTE raw[512];
        for (;;) {
            int got = ReadRawOnce(t, raw, 800);
            if (got <= 0) break;
            HexDump("PUSH", raw, got);
        }
        t.Close();
        anyBattery = anyBattery || ok;
    }

    if (!tested) {
        printf("\n[!] 未找到命令通道 (需要 VID 3554/373B 且 in/out>=17 的集合)。\n");
        return 1;
    }
    printf("\n======== 结论 ========\n%s\n", anyBattery
           ? "至少一个通道读到电量(充电字节语义见各通道输出)。"
           : "所有通道均未读到电量。请确认鼠标已开机/连线。");
    return anyBattery ? 0 : 2;
}
