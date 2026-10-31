/* ==========================================================================
 * ble.js —— Web Bluetooth 通信封装
 *
 * 负责: 扫描/连接/断开、GATT 发现、订阅 notify、写数据、分片重组
 *
 * 重要限制（浏览器层面，不是代码问题）:
 *   1. 仅 Chrome / Edge / Opera 支持；iOS 全系不支持
 *   2. 页面必须运行在 https:// 或 http://localhost（安全上下文）
 *   3. 必须由用户点击触发 requestDevice()，不能自动连接
 * ========================================================================== */

'use strict';

/* --------------------------------------------------------------------------
 * 本设备的 UUID 定义（来自 components/ble/esp32_ble_server.c）
 * -------------------------------------------------------------------------- */
const UUID = {
    // 产品主服务
    SERVICE: 0x00010108,
    // 特征
    DATA_RX: 0xABF1,   // APP -> ESP32  写数据（WRITE_NO_RSP）
    DATA_TX: 0xABF2,   // ESP32 -> APP  通知数据
    CMD_RX: 0xABF3,   // APP -> ESP32  写命令（WRITE_NO_RSP）
    STATUS_TX: 0xABF4,   // ESP32 -> APP  通知状态
};

// 重组成标准 128 位 UUID 字符串（后 96 位为 SIG 基址）
function fullUuid(short) {
    const hex = short.toString(16).padStart(8, '0');
    return `${hex}-0000-1000-8000-00805f9b34fb`;
}

/* --------------------------------------------------------------------------
 * 事件回调（由 app.js 注册）
 * -------------------------------------------------------------------------- */
const BleEvents = {
    onStatus: null,   // (text, state) => void   state: 'idle'|'ok'|'err'
    onLog: null,      // (dir, msg) => void      dir: 'tx'|'rx'|'sys'|'err'
    onGatt: null,     // (list) => void          服务/特征列表
    onMtu: null,      // (mtu) => void
    onDisconnect: null,
    onData: null,     // (uuid, bytes) => void   收到 0xABF2 数据
    onStatusNotify: null, // (uuid, bytes) => void 收到 0xABF4 状态
    onSubState: null, // (uuid, subscribed) => void  订阅状态变化
};

function emit(dir, msg) {
    if (BleEvents.onLog) BleEvents.onLog(dir, msg);
}

function setStatus(text, state) {
    if (BleEvents.onStatus) BleEvents.onStatus(text, state);
}

/* --------------------------------------------------------------------------
 * BLE 连接器
 * -------------------------------------------------------------------------- */
class BleClient {

    constructor() {
        this.device = null;
        this.server = null;
        this.service = null;
        /** @type {Map<number, BluetoothRemoteGATTCharacteristic>} */
        this.chars = new Map();
        this.mtu = 23;            // BLE 默认最小 MTU，连接后由协商值更新
        this.connected = false;
        this._rxBuffer = new Map(); // 分片重组缓冲: totalNum -> {total, parts:[]}
        /** @type {Set<number>} 当前已订阅的特征 */
        this.subscribed = new Set();
    }

    get supported() {
        return typeof navigator !== 'undefined' && !!navigator.bluetooth;
    }

    /* ----------------------------------------------------------------------
     * 连接
     * -------------------------------------------------------------------- */
    async connect() {
        if (!this.supported) {
            throw new Error('当前浏览器不支持 Web Bluetooth。请使用 Chrome / Edge，并确保通过 localhost 或 HTTPS 访问。');
        }

        emit('sys', '正在请求设备…');
        setStatus('扫描中…', 'idle');

        // 可选服务过滤器：只列出带我们主服务的设备，避免列表太乱
        const filters = [{ services: [UUID.SERVICE] }];
        const optionalServices = [UUID.SERVICE];

        try {
            this.device = await navigator.bluetooth.requestDevice({
                filters,
                optionalServices,
            });
        } catch (e) {
            // 用户点了取消
            if (e.name === 'NotFoundError') {
                setStatus('已取消', 'idle');
                emit('sys', '用户取消了选择');
                return false;
            }
            // 某些环境不支持 filters（如无广播服务数据），退回 acceptAllDevices
            emit('sys', '按服务名过滤失败，改为显示全部设备: ' + e.message);
            this.device = await navigator.bluetooth.requestDevice({
                acceptAllDevices: true,
                optionalServices,
            });
        }

        this.device.addEventListener('gattserverdisconnected', () => {
            this._handleDisconnected();
        });

        emit('sys', `已选择设备: ${this.device.name || '(无名)'} / ${this.device.id}`);
        await this._connectGatt();
        return true;
    }

    async _connectGatt() {
        emit('sys', '正在建立 GATT 连接…');
        this.server = await this.device.gatt.connect();
        emit('sys', 'GATT 已连接，正在发现服务…');

        this.service = await this.server.getPrimaryService(UUID.SERVICE);
        emit('sys', `发现主服务 ${fullUuid(UUID.SERVICE)}`);

        // 枚举我们关心的 4 个特征
        const wanted = [
            [UUID.DATA_RX, '数据接收'],
            [UUID.DATA_TX, '数据通知'],
            [UUID.CMD_RX, '命令接收'],
            [UUID.STATUS_TX, '状态通知'],
        ];

        const found = [];
        for (const [uuid, name] of wanted) {
            try {
                const ch = await this.service.getCharacteristic(uuid);
                this.chars.set(uuid, ch);
                found.push({ uuid, name, char: ch });
                emit('sys', `特征 ${name} ${fullUuid(uuid)} 已就绪`);
            } catch (e) {
                emit('err', `特征 ${fullUuid(uuid)} 未找到: ${e.message}`);
            }
        }

        if (found.length === 0) {
            throw new Error('未找到任何目标特征，请检查固件是否正常启动 BLE');
        }

        // 枚举完整 GATT 表（用于左侧展示）
        await this._dumpGatt();

        // 订阅两个 notify 特征
        await this._subscribe(UUID.DATA_TX);
        await this._subscribe(UUID.STATUS_TX);

        this.connected = true;
        setStatus('已连接', 'ok');
        emit('sys', '连接完成 ✓');
    }

    /* ----------------------------------------------------------------------
     * 断开
     * -------------------------------------------------------------------- */
    disconnect() {
        if (this.device && this.device.gatt && this.device.gatt.connected) {
            emit('sys', '正在断开…');
            this.device.gatt.disconnect();
        }
    }

    _handleDisconnected() {
        this.connected = false;
        this.chars.clear();
        this.service = null;
        this.server = null;
        this._rxBuffer.clear();
        this.subscribed.clear();
        setStatus('已断开', 'err');
        emit('sys', '设备已断开');
        if (BleEvents.onDisconnect) BleEvents.onDisconnect();
    }

    /* ----------------------------------------------------------------------
     * 订阅 / 取消订阅通知
     * -------------------------------------------------------------------- */
    async _subscribe(uuid) {
        const ch = this.chars.get(uuid);
        if (!ch) {
            emit('err', `特征 ${fullUuid(uuid)} 不存在，无法订阅`);
            return false;
        }

        try {
            // 避免重复订阅（重复 startNotifications 会再挂一个监听器）
            if (!ch._ourListener) {
                ch._ourListener = (ev) => {
                    const dv = ev.target.value;
                    const bytes = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
                    this._onNotify(uuid, bytes);
                };
                ch.addEventListener('characteristicvaluechanged', ch._ourListener);
            }

            await ch.startNotifications();
            this.subscribed.add(uuid);
            emit('sys', `已订阅 ${fullUuid(uuid)} 通知`);
            if (BleEvents.onSubState) BleEvents.onSubState(uuid, true);
            return true;
        } catch (e) {
            emit('err', `订阅 ${fullUuid(uuid)} 失败: ${e.message}`);
            return false;
        }
    }

    async unsubscribe(uuid) {
        const ch = this.chars.get(uuid);
        if (!ch) return false;
        try {
            await ch.stopNotifications();
            this.subscribed.delete(uuid);
            emit('sys', `已取消订阅 ${fullUuid(uuid)}`);
            if (BleEvents.onSubState) BleEvents.onSubState(uuid, false);
            return true;
        } catch (e) {
            emit('err', `取消订阅失败: ${e.message}`);
            return false;
        }
    }

    async subscribe(uuid) {
        return this._subscribe(uuid);
    }

    /* ----------------------------------------------------------------------
     * 收到通知: 处理分片
     *
     * ESP32 侧长数据分片格式（esp32_gatt_send_data）:
     *   [0x23 0x23][totalNum 1B][currNum 1B][data ...]
     *   '#'  '#'   = 0x23
     * -------------------------------------------------------------------- */
    _onNotify(uuid, bytes) {
        const isFragmented = bytes.length >= 4 && bytes[0] === 0x23 && bytes[1] === 0x23;

        if (!isFragmented) {
            this._emitData(uuid, bytes);
            return;
        }

        const totalNum = bytes[2];
        const currNum = bytes[3];
        const payload = bytes.slice(4);

        emit('sys', `分片 ${currNum}/${totalNum}，本片 ${payload.length} 字节`);

        let entry = this._rxBuffer.get(totalNum);
        if (!entry || currNum === 1) {
            entry = { total: totalNum, parts: [] };
            this._rxBuffer.set(totalNum, entry);
        }
        entry.parts[currNum - 1] = payload;

        // 收齐了
        const received = entry.parts.filter(p => p).length;
        if (received === totalNum) {
            let len = 0;
            entry.parts.forEach(p => len += p.length);
            const full = new Uint8Array(len);
            let off = 0;
            entry.parts.forEach(p => { full.set(p, off); off += p.length; });
            this._rxBuffer.delete(totalNum);
            emit('sys', `分片重组完成，共 ${full.length} 字节`);
            this._emitData(uuid, full);
        }
    }

    _emitData(uuid, bytes) {
        const isData = uuid === UUID.DATA_TX;
        const label = isData ? '数据通知 0xABF2' : '状态通知 0xABF4';

        if (BleEvents.onLog) {
            BleEvents.onLog('rx', `${label} [${bytes.length}B] ${Protocol.formatBytes(bytes)}`);
        }

        if (isData) {
            if (BleEvents.onData) BleEvents.onData(uuid, bytes);
        } else {
            if (BleEvents.onStatusNotify) BleEvents.onStatusNotify(uuid, bytes);
        }
    }

    /* ----------------------------------------------------------------------
     * 发送数据
     * -------------------------------------------------------------------- */
    /**
     * 写数据（自动分片）
     *
     * 浏览器同样受 MTU-3 限制（默认 MTU 23 → 单次最多 20 字节），
     * 超过会直接抛 "value byte length exceeds" 错误。
     * 微信小程序端也是 20 字节上限，两端保持一致的分片策略，
     * 由固件的下行重组层（spp_rx_feed）负责还原。
     */
    async write(uuid, bytes) {
        const ch = this.chars.get(uuid);
        if (!ch) throw new Error(`特征 ${fullUuid(uuid)} 不可用`);

        const chunkSize = 20;
        const gapMs = 20;
        const src = bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes);

        for (let off = 0; off < src.length; off += chunkSize) {
            const part = src.subarray(off, Math.min(off + chunkSize, src.length));

            // WRITE_NO_RSP 优先（快），不支持则退回 WRITE
            if (ch.properties.writeWithoutResponse) {
                await ch.writeValueWithoutResponse(part);
            } else if (ch.properties.write) {
                await ch.writeValue(part);
            } else {
                throw new Error(`特征 ${fullUuid(uuid)} 不支持写入`);
            }

            if (off + chunkSize < src.length) {
                await new Promise(r => setTimeout(r, gapMs));
            }
        }

        if (src.length > chunkSize) {
            emit('sys', `已分片写入 ${Math.ceil(src.length / chunkSize)} 片，共 ${src.length} 字节`);
        }
    }

    /* ----------------------------------------------------------------------
     * 枚举完整 GATT 表（仅用于左侧展示）
     * -------------------------------------------------------------------- */
    async _dumpGatt() {
        const list = [];
        try {
            const services = await this.server.getPrimaryServices();
            for (const svc of services) {
                const item = { uuid: svc.uuid, chars: [] };
                try {
                    const chars = await svc.getCharacteristics();
                    for (const c of chars) {
                        item.chars.push({
                            uuid: c.uuid,
                            props: c.properties,
                            ours: [UUID.DATA_RX, UUID.DATA_TX, UUID.CMD_RX, UUID.STATUS_TX]
                                .some(u => c.uuid.toLowerCase().includes(u.toString(16).padStart(8, '0'))),
                        });
                    }
                } catch (e) { /* 某些服务不允许枚举特征 */ }
                list.push(item);
            }
        } catch (e) {
            emit('err', '枚举服务失败: ' + e.message);
        }
        if (BleEvents.onGatt) BleEvents.onGatt(list);
    }

    /* ----------------------------------------------------------------------
     * 读设备信息（0x1800 Generic Access）
     * -------------------------------------------------------------------- */
    async readDeviceInfo() {
        const out = {};
        try {
            const ga = await this.server.getPrimaryService(0x1800);
            try {
                const nameCh = await ga.getCharacteristic(0x2a00);
                const dv = await nameCh.readValue();
                out.name = new TextDecoder().decode(dv);
            } catch (e) { /* 可能不可读 */ }
        } catch (e) { /* 无 0x1800 服务 */ }
        return out;
    }
}

/* --------------------------------------------------------------------------
 * 字节工具
 * -------------------------------------------------------------------------- */
const Protocol = {
    /** 转 HEX 字符串 */
    formatBytes(bytes) {
        return Array.from(bytes).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
    },

    /** HEX 字符串 -> Uint8Array（容错：允许空格/逗号/0x 前缀） */
    parseHex(str) {
        const clean = String(str).replace(/0x/gi, '').replace(/[^0-9a-fA-F]/g, '');
        if (clean.length % 2 !== 0) throw new Error('HEX 长度必须为偶数');
        const out = new Uint8Array(clean.length / 2);
        for (let i = 0; i < out.length; i++) {
            out[i] = parseInt(clean.substr(i * 2, 2), 16);
        }
        return out;
    },

    /** 转可打印 ASCII（不可打印用 . 占位） */
    toAscii(bytes) {
        return Array.from(bytes).map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
    },
};
