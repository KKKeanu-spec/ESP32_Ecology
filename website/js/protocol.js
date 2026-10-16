/* ==========================================================================
 * protocol.js —— 应用层协议组包 / 解包
 *
 * 依据《交流桩与APP通信协议 V1.4》
 *
 * ⚠️ 重要提示
 *   下面 0x59 的组包顺序按 PDF 表格文字整理，属"最佳推测"。
 *   字节序已确认是小端（PDF 尾部明确说明）。
 *   但 SSID/密码的具体编码方式（是否带长度前缀、UTF8 还是 GBK）
 *   必须用真实抓包验证。验证方法见 tools/README。
 * ========================================================================== */

'use strict';

/* --------------------------------------------------------------------------
 * 命令码定义
 * -------------------------------------------------------------------------- */
const CMD = {
    NET_SET: 0x59,   // APP -> TC   设置网络通信方式
    NET_ACK: 0x5A,   // TC  -> APP  设置网络通信方式应答
    NET_ALL: 0x5B,   // TC  -> APP  同时开启所有网络
    HW_QUERY: 0x61,   // APP -> TC   查询硬件信息
    HW_ACK: 0x62,   // TC  -> APP  查询硬件信息应答

    // ---- TinyML 控制（自定义扩展） ----
    ML_STOP: 0xE0,   // APP -> ESP32  停止周期推理
    ML_START: 0xE1,   // APP -> ESP32  启动周期推理
    ML_RUN: 0xE2,   // 双向: APP->ESP32 [float x] / ESP32->APP [float y]

    // ---- 云端服务器地址下发 ----
    SRV_SET: 0xE4,   // APP -> ESP32  [portLo][portHi][host\0]
    SRV_ACK: 0xE4,   // ESP32 -> APP  [ok][portLo][portHi]
};

/* TinyML 应答状态 */
const ML_ACK = {
    OK: 0x01,
    FAIL: 0x00,
};

/* 网络类型（0x59 载荷第 0 字节） */
const NET_TYPE = {
    ETHERNET: 0x00,
    LTE_4G: 0x01,
    WIFI: 0x02,
};

/* IP 获取方式 */
const IP_MODE = {
    STATIC: 0x00,
    DHCP: 0x01,
};

const AckCode = {
    NET_RECV_OK: 0x01,
    NET_RECV_BAD: 0x00,
};

/* --------------------------------------------------------------------------
 * 组包 / 解包
 * -------------------------------------------------------------------------- */
const Codec = {

    /* ----------------------------------------------------------------------
     * 0x59 设置网络通信方式
     *
     * 载荷布局:
     *   [0] 命令码 0x59
     *   [1] netType        网络类型 0x00=以太网 0x01=4G 0x02=WiFi
     *   [2] ipMode         0x00=静态 0x01=动态
     *   --- 以下仅 WiFi ---
     *   [3] ssidLen        SSID 字节长度
     *   [4] pwdLen         密码字节长度
     *   [5..] ssid         裸字符串（无终止符）
     *   [..]  pwd          裸字符串
     *   --- 仅静态 IP 时追加 ---
     *   ip[4] + netmask[4] + gw[4] + dns[4]   ← 位置待实测确认
     *   --- 可选：注册地址 + 密钥 ---
     *   addrLen + keyLen + addr + key
     *
     * 【实测验证 2026-10-16 通过】
     *   真实报文: 59 02 01 07 08 | "PCsemic" | "PC201922"
     *   长度 = 5 + 7 + 8 = 20，与 BLE 上报 length=20 完全吻合。
     *   确认: [3][4] 确为 ssidLen/pwdLen；SSID/密码为裸串；
     *         无终止符、无校验位；动态 IP 时不带静态 IP 参数。
     * -------------------------------------------------------------------- */
    buildNetSet(opt) {
        const enc = new TextEncoder();
        const ssid = enc.encode(opt.ssid || '');
        const pwd = enc.encode(opt.passwd || '');
        const netType = opt.netType;
        const ipMode = opt.ipMode;
        const isWifi = netType === NET_TYPE.WIFI;
        const isStatic = ipMode === IP_MODE.STATIC;

        if (isWifi) {
            if (ssid.length > 255) throw new Error('SSID 过长');
            if (pwd.length > 255) throw new Error('密码过长');
        }

        const chunks = [];
        const u8 = (v) => chunks.push(v & 0xff);

        u8(CMD.NET_SET);
        u8(netType);
        u8(ipMode);

        if (isWifi) {
            // 长度前缀
            u8(ssid.length);
            u8(pwd.length);

            // SSID + 密码（裸字符串，无终止符）—— 实测顺序确认
            ssid.forEach(u8);
            pwd.forEach(u8);

            // 静态 IP 参数（实测未覆盖，暂追加于 SSID/密码之后；
            // 若后续用静态 IP 抓包发现位置不符，只需调整此处）
            if (isStatic) {
                const ip = this._parseIPv4(opt.ip);
                const mask = this._parseIPv4(opt.netmask);
                const gw = this._parseIPv4(opt.gw);
                const dns = this._parseIPv4(opt.dns);
                ip.forEach(u8); mask.forEach(u8); gw.forEach(u8); dns.forEach(u8);
            }
        }

        // 注册地址 + 密钥（可选）
        if (opt.regAddr) {
            const addr = enc.encode(opt.regAddr);
            const key = enc.encode(opt.regKey || '');
            u8(addr.length);
            u8(key.length);
            addr.forEach(u8);
            key.forEach(u8);
        }

        return new Uint8Array(chunks);
    },

    /* ----------------------------------------------------------------------
     * 解析 0x59（可用于回显校验：自己组的包能否被正确解回）
     * -------------------------------------------------------------------- */
    parseNetSet(bytes) {
        if (bytes.length < 5 || bytes[0] !== CMD.NET_SET) return null;

        const netType = bytes[1];
        const ipMode = bytes[2];
        const out = { netType, ipMode };

        if (netType === NET_TYPE.WIFI) {
            const ssidLen = bytes[3];
            const pwdLen = bytes[4];
            let off = 5;

            out.ssid = new TextDecoder().decode(bytes.slice(off, off + ssidLen));
            off += ssidLen;
            out.passwd = new TextDecoder().decode(bytes.slice(off, off + pwdLen));
            off += pwdLen;

            if (ipMode === IP_MODE.STATIC && bytes.length >= off + 16) {
                const ip4 = (o) => `${bytes[o]}.${bytes[o + 1]}.${bytes[o + 2]}.${bytes[o + 3]}`;
                out.ip = ip4(off);
                out.netmask = ip4(off + 4);
                out.gw = ip4(off + 8);
                out.dns = ip4(off + 12);
            }
        }
        return out;
    },

    /* ----------------------------------------------------------------------
     * 0x61 查询硬件信息（无载荷）
     * -------------------------------------------------------------------- */
    buildHwQuery() {
        return new Uint8Array([CMD.HW_QUERY]);
    },

    /* ----------------------------------------------------------------------
     * TinyML 控制
     * -------------------------------------------------------------------- */
    /** 0xE1 启动周期推理 */
    buildMlStart() {
        return new Uint8Array([CMD.ML_START]);
    },

    /** 0xE0 停止周期推理 */
    buildMlStop() {
        return new Uint8Array([CMD.ML_STOP]);
    },

    /** 0xE2 单次推理 [0xE2][float x] */
    buildMlRun(x) {
        const buf = new ArrayBuffer(5);
        const dv = new DataView(buf);
        dv.setUint8(0, CMD.ML_RUN);
        dv.setFloat32(1, x, true);   // ESP32 为小端
        return new Uint8Array(buf);
    },

    /* ----------------------------------------------------------------------
     * 0xE4 下发云端服务器地址
     *   载荷: [portLo][portHi][host(UTF8) + '\0']
     * -------------------------------------------------------------------- */
    buildSrvSet(host, port) {
        const h = String(host || '').trim();
        const p = parseInt(port, 10);

        if (!h) throw new Error('服务器地址不能为空');
        if (!(p >= 1 && p <= 65535)) throw new Error('端口无效 (1~65535)');
        if (h.length > 63) throw new Error('地址过长 (最多 63 字符)');

        const hb = new TextEncoder().encode(h);
        const buf = new Uint8Array(3 + hb.length + 1);
        buf[0] = CMD.SRV_SET;
        buf[1] = p & 0xff;          // 小端端口
        buf[2] = (p >> 8) & 0xff;
        buf.set(hb, 3);
        buf[3 + hb.length] = 0;     // 终止符
        return buf;
    },

    /** 解析 0xE4 应答: [0xE4][ok][portLo][portHi] */
    parseSrvAck(bytes) {
        if (bytes.length < 4 || bytes[0] !== CMD.SRV_ACK) return null;
        return {
            ok: bytes[1] === 1,
            port: bytes[2] | (bytes[3] << 8),
        };
    },

    /** 解析 0xE2 应答: [0xE2][float y] -> {value} */
    parseMlResult(bytes) {
        if (bytes.length < 5 || bytes[0] !== CMD.ML_RUN) return null;
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        return { value: dv.getFloat32(1, true) };
    },

    /**
     * 解析 0xE2 周期推送: [0xE2][float phase][float pred]
     * 与单次推理应答的区别是长度(9 vs 5)
     */
    parseMlPush(bytes) {
        if (bytes.length < 9 || bytes[0] !== CMD.ML_RUN) return null;
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        return {
            phase: dv.getFloat32(1, true),
            pred: dv.getFloat32(5, true),
        };
    },

    /* ----------------------------------------------------------------------
     * 解析 0x5A 应答（3 字节状态）
     *   [0] 0x03 段: 0x01 收到配置 / 0x00 格式错误
     *   [1] 0x04 段: 0x01 网络连接成功 / 0x00 失败
     *   [2] 0x05 段: 0x01 MQTT 成功 / 0x00 失败
     * -------------------------------------------------------------------- */
    parseNetAck(bytes) {
        if (bytes.length < 1) return null;
        // 可能是 [cmd][seg][val] 或直接 [seg...]，两种都尝试
        let body = bytes;
        if (bytes[0] === CMD.NET_ACK) body = bytes.slice(1);
        return {
            recv: body[0],
            net: body[1],
            mqtt: body[2],
        };
    },

    /* ----------------------------------------------------------------------
     * 解析 0x62 硬件信息应答
     *   [0]      协议类型
     *   [1..2]   协议版本 (major, minor)
     *   [3..18]  硬件名称 char[16]
     *   [19..21] 硬件版本 u8[3]
     *   [22..37] 软件名称 char[16]
     *   [38..40] 软件版本 u8[3]
     * -------------------------------------------------------------------- */
    parseHwAck(bytes) {
        let body = bytes;
        if (bytes[0] === CMD.HW_ACK) body = bytes.slice(1);
        if (body.length < 41) return null;

        const str = (off, len) => {
            let s = '';
            for (let i = 0; i < len; i++) {
                const c = body[off + i];
                if (c === 0) break;               // 遇 NUL 截断
                if (c >= 32 && c < 127) s += String.fromCharCode(c);
            }
            return s;
        };

        return {
            protoType: body[0],
            protoVer: `${body[1]}.${body[2]}`,
            hwName: str(3, 16),
            hwVer: `${body[19]}.${body[20]}.${body[21]}`,
            swName: str(22, 16),
            swVer: `${body[38]}.${body[39]}.${body[40]}`,
        };
    },

    /* ----------------------------------------------------------------------
     * 工具: 解析 IPv4 字符串为 4 字节
     * -------------------------------------------------------------------- */
    _parseIPv4(s) {
        const parts = String(s || '').split('.').map(x => parseInt(x, 10));
        if (parts.length !== 4 || parts.some(n => isNaN(n) || n < 0 || n > 255)) {
            throw new Error(`IP 格式错误: ${s}`);
        }
        return parts;
    },

    /* ----------------------------------------------------------------------
     * 工具: Uint8Array 转 HEX 字符串
     * -------------------------------------------------------------------- */
    toHex(bytes) {
        return Array.from(bytes).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
    },

    /* ----------------------------------------------------------------------
     * 工具: 带 offset 的分组 HEX（便于阅读长报文）
     * -------------------------------------------------------------------- */
    toHexDump(bytes, perLine) {
        perLine = perLine || 16;
        const lines = [];
        for (let i = 0; i < bytes.length; i += perLine) {
            const slice = bytes.slice(i, i + perLine);
            const hex = Array.from(slice).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
            const asc = Array.from(slice).map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
            lines.push(`${i.toString(16).padStart(4, '0')}  ${hex.padEnd(perLine * 3 - 1)}  ${asc}`);
        }
        return lines.join('\n');
    },
};
