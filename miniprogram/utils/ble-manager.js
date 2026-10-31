'use strict';

/* ==========================================================================
 * ble-manager.js —— 微信小程序 BLE 内核
 *
 * 对应 website/js/ble.js，接口改为 Promise + 事件，页面只管订阅。
 *
 * 负责：
 *   1. 适配器开关、扫描、按服务过滤（失败自动降级全量扫描）
 *   2. 连接、服务/特征发现、notify 订阅、MTU 协商
 *   3. 写队列 + 20 字节分片（小程序硬限制，写长帧必须切）
 *   4. 上行 0x23 0x23 分片重组（与固件 esp32_gatt_send_data 对应）
 *   5. 断线自动重连（指数退避）
 *
 * 事件：
 *   adapter     { ready, scanning }           适配器状态变化
 *   devices     [ {deviceId,name,rssi,hitSvc} ] 扫描结果（已按 RSSI 排序）
 *   scan        { scanning }                  扫描开/关
 *   connected   { deviceId, name, mtu, chars } 
 *   disconnected{ reason }                    
 *   frame       { channel, bytes, frame }     一条完整的应用层帧
 *   log         { dir, text, hex, time }      收发/系统日志
 * ========================================================================== */

const fmt = require('./format');
const protocol = require('./protocol');

/* 128 位 UUID（小程序要求完整形式，短 UUID 在 iOS 上会失败） */
const UUID = {
  SERVICE: '00010108-0000-1000-8000-00805F9B34FB',
  DATA_RX: '0000ABF1-0000-1000-8000-00805F9B34FB', // APP -> 设备（数据）
  DATA_TX: '0000ABF2-0000-1000-8000-00805F9B34FB', // 设备 -> APP（通知）
  CMD_RX: '0000ABF3-0000-1000-8000-00805F9B34FB', // APP -> 设备（命令）
  STATUS_TX: '0000ABF4-0000-1000-8000-00805F9B34FB', // 设备 -> APP（状态通知）
};

const CHUNK_SIZE = 20; // 小程序单次写入上限
const CHUNK_GAP_MS = 20; // 分片间隔，避免安卓写队列溢出
const FRAG_TIMEOUT_MS = 3000;
const RECONNECT_DELAYS = [800, 1600, 3200, 6000, 10000, 15000];

const ERR_TEXT = {
  10000: '蓝牙适配器未初始化',
  10001: '蓝牙未打开，请先打开手机蓝牙',
  10002: '没有找到指定设备',
  10003: '连接失败，设备可能已被其他手机占用',
  10004: '没有找到指定服务',
  10005: '没有找到指定特征',
  10006: '当前连接已断开',
  10007: '该特征不支持此操作',
  10008: '系统异常（写入失败，多见于特征不支持带响应写）',
  10009: '安卓系统版本过低（需 4.3+）',
  10012: '连接超时',
  10013: 'deviceId 为空',
};

function normalize(uuid) {
  return String(uuid || '').toUpperCase();
}

function normalizeErr(err) {
  const code = err && (err.errCode || err.errno || 0);
  const raw = (err && (err.errMsg || err.message)) || '未知错误';
  const e = new Error(ERR_TEXT[code] ? ERR_TEXT[code] : raw);
  e.errCode = code;
  e.raw = err;
  return e;
}

/** wx API -> Promise */
function call(api, opts) {
  return new Promise(function (resolve, reject) {
    if (typeof api !== 'function') {
      reject(new Error('当前微信版本不支持该蓝牙接口'));
      return;
    }
    const o = Object.assign({}, opts || {});
    o.success = function (res) {
      resolve(res);
    };
    o.fail = function (err) {
      reject(normalizeErr(err));
    };
    try {
      api(o);
    } catch (e) {
      reject(normalizeErr(e));
    }
  });
}

class BleManager {
  constructor() {
    this.adapterReady = false;
    this.scanning = false;
    this.devices = [];
    this.connected = false;
    this.deviceId = '';
    this.deviceName = '';
    this.serviceId = '';
    this.charIds = {};
    this.charProps = {};
    this.mtu = 23;
    this.platform = '';
    this.autoReconnect = true;
    this.logs = [];

    this._listeners = {};
    this._bound = false;
    this._queue = Promise.resolve();
    this._rxParts = {};
    this._rxTimers = {};
    this._manualClose = false;
    this._reconnectIdx = 0;
    this._reconnectTimer = null;
    this._scanTimer = null;
    this._filtered = false;
  }

  /* ------------------------------ 事件 ------------------------------ */

  on(evt, fn) {
    if (!this._listeners[evt]) this._listeners[evt] = [];
    this._listeners[evt].push(fn);
    return fn;
  }

  off(evt, fn) {
    const arr = this._listeners[evt];
    if (!arr) return;
    if (!fn) {
      delete this._listeners[evt];
      return;
    }
    const i = arr.indexOf(fn);
    if (i >= 0) arr.splice(i, 1);
  }

  _emit(evt, data) {
    const arr = this._listeners[evt];
    if (!arr || !arr.length) return;
    arr.slice().forEach(function (fn) {
      try {
        fn(data);
      } catch (e) {
        console.error('[ble] listener error on ' + evt, e);
      }
    });
  }

  _log(dir, text, bytes) {
    const item = {
      dir: dir,
      text: text,
      hex: bytes && bytes.length ? fmt.hex(bytes) : '',
      time: fmt.time(),
      ts: Date.now(),
    };
    this.logs.push(item);
    if (this.logs.length > 200) this.logs.shift();
    this._emit('log', item);
  }

  clearLogs() {
    this.logs = [];
  }

  /* ------------------------------ 适配器 ------------------------------ */

  _deviceInfo() {
    try {
      if (typeof wx.getDeviceInfo === 'function') return wx.getDeviceInfo() || {};
    } catch (e) {}
    try {
      return wx.getSystemInfoSync() || {};
    } catch (e) {}
    return {};
  }

  /**
   * 隐私授权（蓝牙属于隐私接口）
   *
   * 体验版/正式版首次调用蓝牙接口前必须让用户同意《用户隐私保护指引》，
   * 指引需在小程序后台「设置 → 服务内容声明」里勾选「蓝牙」。
   * 未配置或用户拒绝时，这里给出可读的提示，而不是让后续报"蓝牙未打开"。
   */
  _privacy() {
    return new Promise(function (resolve, reject) {
      if (typeof wx.requirePrivacyAuthorize !== 'function') {
        resolve();
        return;
      }
      wx.requirePrivacyAuthorize({
        success: function () {
          resolve();
        },
        fail: function (e) {
          const msg = String((e && e.errMsg) || '');
          if (msg.indexOf('cancel') >= 0 || msg.indexOf('deny') >= 0) {
            reject(new Error('需要同意隐私协议（含蓝牙权限）才能搜索设备，请重新点击「开始扫描」'));
            return;
          }
          // 其他情况（未配置隐私指引、旧版本等）不阻断流程
          resolve();
        },
      });
    });
  }

  _bind() {
    if (this._bound) return;
    this._bound = true;
    const self = this;

    wx.onBluetoothAdapterStateChange(function (res) {
      self.adapterReady = !!res.available;
      self.scanning = !!res.discovering;
      self._emit('adapter', { ready: self.adapterReady, scanning: self.scanning });
      if (!res.available) {
        self._log('err', '蓝牙已关闭');
        if (self.connected) self._dropConnection('蓝牙关闭');
      }
    });

    wx.onBluetoothDeviceFound(function (res) {
      self._onFound(res.devices || []);
    });

    wx.onBLEConnectionStateChange(function (res) {
      if (normalize(res.deviceId) !== normalize(self.deviceId)) return;
      if (res.connected) return;
      const was = self.connected;
      self.connected = false;
      self._clearRx();
      self._emit('disconnected', { reason: '连接已断开' });
      if (was) self._log('err', '设备已断开（reason=' + (res.reason == null ? '-' : res.reason) + '）');
      self._scheduleReconnect();
    });

    wx.onBLECharacteristicValueChange(function (res) {
      self._onValue(res);
    });
  }

  async open() {
    if (this.adapterReady) return true;
    this._bind();
    await this._privacy();
    const info = this._deviceInfo();
    this.platform = String(info.platform || '').toLowerCase();
    const err = await call(wx.openBluetoothAdapter).then(
      function () {
        return null;
      },
      function (e) {
        return e;
      }
    );
    if (err) {
      /* 开发者工具（以及部分安卓机）在适配器已打开时会返回
       * "openBluetoothAdapter:fail already opened"，这不是错误，
       * 按成功处理即可，否则会误报并中断扫描。 */
      const raw = (err.raw && (err.raw.errMsg || err.raw.message)) || err.message || '';
      if (String(raw).toLowerCase().indexOf('already') >= 0) {
        this._log('sys', '蓝牙适配器已处于打开状态');
      } else {
        this.adapterReady = false;
        this._emit('adapter', { ready: false, scanning: false });
        throw err;
      }
    }
    this.adapterReady = true;
    this._emit('adapter', { ready: true, scanning: false });
    this._log('sys', '蓝牙适配器就绪（' + (this.platform || '未知平台') + '）');
    return true;
  }

  async close() {
    await this.stopScan().catch(function () {});
    if (this.deviceId) {
      await call(wx.closeBLEConnection, { deviceId: this.deviceId }).catch(function () {});
    }
    await call(wx.closeBluetoothAdapter).catch(function () {});
    this.adapterReady = false;
    this._emit('adapter', { ready: false, scanning: false });
  }

  /* ------------------------------ 扫描 ------------------------------ */

  async startScan() {
    await this.open();
    if (this.scanning) await this.stopScan();

    this.devices = [];
    this._emit('devices', this.devices);

    const self = this;
    const filtered = await call(wx.startBluetoothDevicesDiscovery, {
      services: [UUID.SERVICE],
      allowDuplicatesKey: true,
      interval: 0,
    }).then(
      function () {
        return true;
      },
      function (e) {
        const raw = String((e.raw && e.raw.errMsg) || e.message || '').toLowerCase();
        if (raw.indexOf('already') >= 0) {
          self._log('sys', '扫描已在运行中');
          return true;
        }
        self._log('sys', '按服务过滤启动失败，改为全量扫描：' + e.message);
        return false;
      }
    );

    if (!filtered) {
      await call(wx.startBluetoothDevicesDiscovery, {
        allowDuplicatesKey: true,
        interval: 0,
      }).catch(function (e) {
        throw e;
      });
    }

    this._filtered = filtered;
    this.scanning = true;
    this._emit('scan', { scanning: true });
    this._log('sys', filtered ? '开始扫描（按服务 UUID 过滤）' : '开始扫描（全量）');

    // 过滤模式下 4 秒无结果 -> 自动降级，规避部分安卓机广播里只有 32 位 UUID 的情况
    if (filtered) {
      this._scanTimer = setTimeout(function () {
        if (self.scanning && !self.devices.length) {
          self._log('sys', '过滤模式无结果，降级为全量扫描');
          self._restartScan(false);
        }
      }, 4000);
    }
    return true;
  }

  async _restartScan(filtered) {
    await call(wx.stopBluetoothDevicesDiscovery).catch(function () {});
    await fmt.sleep(120);
    try {
      await call(
        wx.startBluetoothDevicesDiscovery,
        filtered
          ? { services: [UUID.SERVICE], allowDuplicatesKey: true, interval: 0 }
          : { allowDuplicatesKey: true, interval: 0 }
      );
      this._filtered = filtered;
    } catch (e) {
      this._log('err', '重新扫描失败：' + e.message);
    }
  }

  async stopScan() {
    if (this._scanTimer) {
      clearTimeout(this._scanTimer);
      this._scanTimer = null;
    }
    this.scanning = false;
    this._emit('scan', { scanning: false });
    await call(wx.stopBluetoothDevicesDiscovery).catch(function () {});
  }

  _onFound(list) {
    if (!list.length) return;
    let changed = false;
    const self = this;

    list.forEach(function (d) {
      const name = d.name || d.localName || '';
      const adv = (d.advertisServiceUUIDs || []).map(normalize);
      const hitSvc = adv.indexOf(UUID.SERVICE) >= 0;

      let idx = -1;
      for (let i = 0; i < self.devices.length; i++) {
        if (normalize(self.devices[i].deviceId) === normalize(d.deviceId)) {
          idx = i;
          break;
        }
      }

      if (idx >= 0) {
        const old = self.devices[idx];
        if (old.rssi !== d.RSSI || (name && name !== old.name)) {
          self.devices[idx] = {
            deviceId: old.deviceId,
            name: name || old.name,
            rssi: d.RSSI,
            hitSvc: hitSvc || old.hitSvc,
          };
          changed = true;
        }
        return;
      }

      // 全量扫描时过滤掉一大堆无名设备，只留可能有用的
      if (!name && !hitSvc) return;

      self.devices.push({
        deviceId: d.deviceId,
        name: name,
        rssi: d.RSSI,
        hitSvc: hitSvc,
      });
      changed = true;
    });

    if (!changed) return;
    this.devices.sort(function (a, b) {
      return (b.rssi || -999) - (a.rssi || -999);
    });
    if (this.devices.length > 30) this.devices = this.devices.slice(0, 30);
    this._emit('devices', this.devices);
  }

  /* ------------------------------ 连接 ------------------------------ */

  async connect(deviceId, name) {
    if (!deviceId) throw new Error('deviceId 为空');
    await this.stopScan().catch(function () {});
    this._manualClose = false;
    this._reconnectIdx = 0;
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
    return this._doConnect(deviceId, name);
  }

  async _doConnect(deviceId, name) {
    const self = this;
    this.deviceId = deviceId;
    this.deviceName = name || this.deviceName || '';
    this._log('sys', '正在连接 ' + (this.deviceName || deviceId));

    // 重连前先清掉上一轮可能残留的连接（安卓上 close 成功后立刻 create 会失败，需间隔）
    await call(wx.closeBLEConnection, { deviceId: deviceId })
      .then(function () {
        return fmt.sleep(150);
      })
      .catch(function () {});
    await call(wx.createBLEConnection, { deviceId: deviceId, timeout: 12000 });

    // MTU：仅安卓可设；iOS 由系统自动协商
    if (typeof wx.setBLEMTU === 'function' && this.platform === 'android') {
      try {
        await call(wx.setBLEMTU, { deviceId: deviceId, mtu: 247 });
      } catch (e) {
        this._log('sys', 'MTU 请求失败，按默认 23 处理（不影响功能）');
      }
    }
    if (typeof wx.getBLEMTU === 'function') {
      try {
        const r = await call(wx.getBLEMTU, { deviceId: deviceId });
        this.mtu = r.mtu || 23;
      } catch (e) {
        this.mtu = 23;
      }
    }
    this._log('sys', '已连接，MTU=' + this.mtu);

    const sres = await this._retry(function () {
      return call(wx.getBLEDeviceServices, { deviceId: deviceId });
    }, 3, 250);

    let svc = null;
    (sres.services || []).forEach(function (s) {
      if (normalize(s.uuid) === UUID.SERVICE) svc = s;
    });
    if (!svc) {
      throw new Error('未找到主服务 00010108（固件 BLE 是否已启动？）');
    }
    this.serviceId = svc.uuid;

    const cres = await this._retry(function () {
      return call(wx.getBLEDeviceCharacteristics, {
        deviceId: deviceId,
        serviceId: svc.uuid,
      });
    }, 3, 250);

    const map = {};
    (cres.characteristics || []).forEach(function (c) {
      map[normalize(c.uuid)] = c;
    });

    this.charIds = {};
    this.charProps = {};
    ['DATA_RX', 'DATA_TX', 'CMD_RX', 'STATUS_TX'].forEach(function (key) {
      const c = map[UUID[key]];
      if (c) {
        self.charIds[key] = c.uuid;
        self.charProps[key] = c.properties || {};
      }
    });

    if (!this.charIds.DATA_TX) throw new Error('未找到数据通知特征 0xABF2');
    if (!this.charIds.DATA_RX && !this.charIds.CMD_RX) {
      throw new Error('未找到可写特征 0xABF1 / 0xABF3');
    }

    ['DATA_RX', 'CMD_RX'].forEach(function (key) {
      const p = self.charProps[key];
      if (!p) return;
      if (!p.write && !p.writeNoResponse) {
        self._log('err', key + ' 特征没有写权限，发数据会失败');
      }
    });

    // 安卓上服务发现后立刻订阅偶发失败，稍等再订阅
    await fmt.sleep(120);
    const notifyKeys = ['DATA_TX', 'STATUS_TX'];
    for (let i = 0; i < notifyKeys.length; i++) {
      const key = notifyKeys[i];
      if (!this.charIds[key]) continue;
      try {
        await call(wx.notifyBLECharacteristicValueChange, {
          deviceId: deviceId,
          serviceId: this.serviceId,
          characteristicId: this.charIds[key],
          state: true,
        });
        this._log('sys', '已订阅 ' + key + ' 通知');
      } catch (e) {
        this._log('err', '订阅 ' + key + ' 失败：' + e.message);
      }
    }

    this.connected = true;
    this._reconnectIdx = 0;
    this._emit('connected', {
      deviceId: this.deviceId,
      name: this.deviceName,
      mtu: this.mtu,
      chars: Object.keys(this.charIds),
    });
    this._log('sys', '连接完成 ✓');
    return true;
  }

  async _retry(fn, times, gap) {
    let last = null;
    for (let i = 0; i <= times; i++) {
      try {
        return await fn();
      } catch (e) {
        last = e;
        if (i < times) await fmt.sleep(gap || 200);
      }
    }
    throw last;
  }

  async disconnect() {
    this._manualClose = true;
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
    const id = this.deviceId;
    this.connected = false;
    this._clearRx();
    if (id) {
      await call(wx.closeBLEConnection, { deviceId: id }).catch(function () {});
    }
    this.serviceId = '';
    this.charIds = {};
    this.charProps = {};
    this._emit('disconnected', { reason: '用户主动断开' });
    this._log('sys', '已断开连接');
  }

  _dropConnection(reason) {
    this.connected = false;
    this._clearRx();
    this._emit('disconnected', { reason: reason });
  }

  _scheduleReconnect() {
    const self = this;
    if (!this.connected && (this._manualClose || !this.autoReconnect)) return;
    if (this._manualClose || !this.autoReconnect || !this.deviceId) return;
    if (this._reconnectTimer) return;

    const idx = this._reconnectIdx;
    if (idx >= RECONNECT_DELAYS.length) {
      this._log('err', '自动重连次数已用尽，请手动连接');
      return;
    }
    const delay = RECONNECT_DELAYS[idx];
    this._reconnectIdx += 1;
    this._log('sys', (delay / 1000).toFixed(1) + 's 后第 ' + this._reconnectIdx + ' 次重连…');

    this._reconnectTimer = setTimeout(function () {
      self._reconnectTimer = null;
      const id = self.deviceId;
      const name = self.deviceName;
      self
        ._doConnect(id, name)
        .then(function () {
          self._reconnectIdx = 0;
        })
        .catch(function (e) {
          self._log('err', '重连失败：' + e.message);
          self._scheduleReconnect();
        });
    }, delay);
  }

  /* ------------------------------ 写入 ------------------------------ */

  /**
   * 发送一帧应用层数据（自动分片）
   * @param {Uint8Array} bytes
   * @param {{channel?:'DATA_RX'|'CMD_RX'}} [opts]
   */
  send(bytes, opts) {
    const self = this;
    if (!this.connected) return Promise.reject(new Error('设备未连接'));
    if (!bytes || !bytes.length) return Promise.reject(new Error('数据为空'));

    const o = opts || {};
    const channel = o.channel || (bytes.length <= CHUNK_SIZE ? 'CMD_RX' : 'DATA_RX');
    const task = function () {
      return self._writeChunks(bytes, channel);
    };
    // 串行执行，避免并发写导致安卓 write 忙
    this._queue = this._queue.then(task, task);
    return this._queue;
  }

  async _writeChunks(bytes, channel) {
    let charId = this.charIds[channel];
    if (!charId) charId = this.charIds.DATA_RX || this.charIds.CMD_RX;
    if (!charId) throw new Error('没有可用的写特征');

    for (let off = 0; off < bytes.length; off += CHUNK_SIZE) {
      const part = bytes.subarray(off, Math.min(off + CHUNK_SIZE, bytes.length));
      let ok = false;
      let lastErr = null;
      for (let attempt = 0; attempt < 2 && !ok; attempt++) {
        try {
          await this._writeRaw(charId, part);
          ok = true;
        } catch (e) {
          lastErr = e;
          await fmt.sleep(40);
        }
      }
      if (!ok) throw lastErr;
      if (off + CHUNK_SIZE < bytes.length) await fmt.sleep(CHUNK_GAP_MS);
    }

    this._log('tx', channel === 'DATA_RX' ? 'DATA' : 'CMD', bytes);
  }

  _writeRaw(charId, part) {
    const ab = new ArrayBuffer(part.length);
    new Uint8Array(ab).set(part);
    return call(wx.writeBLECharacteristicValue, {
      deviceId: this.deviceId,
      serviceId: this.serviceId,
      characteristicId: charId,
      value: ab,
    });
  }

  /* ------------------------------ 接收 ------------------------------ */

  _onValue(res) {
    const cid = normalize(res.characteristicId);
    const dataTx = normalize(this.charIds.DATA_TX || '');
    const statusTx = normalize(this.charIds.STATUS_TX || '');
    const isData = cid === dataTx;
    const isStatus = cid === statusTx;
    if (!isData && !isStatus) return;

    const bytes = new Uint8Array(res.value);
    if (!bytes.length) return;
    const channel = isData ? 'data' : 'status';

    this._log('rx', isData ? 'DATA' : 'STATUS', bytes);

    // 固件长数据分片：[0x23 0x23][total][curr][payload...]
    if (bytes.length >= 4 && bytes[0] === 0x23 && bytes[1] === 0x23) {
      this._onFragment(bytes, channel);
      return;
    }
    this._emit('frame', {
      channel: channel,
      bytes: bytes,
      frame: protocol.parseFrame(bytes),
    });
  }

  _onFragment(bytes, channel) {
    const total = bytes[2];
    const curr = bytes[3];
    if (!total || !curr || curr > total) return;
    const self = this;
    const key = channel + ':' + total;

    if (!this._rxParts[key] || curr === 1) {
      this._rxParts[key] = { total: total, parts: [] };
    }
    this._rxParts[key].parts[curr - 1] = bytes.subarray(4);

    if (this._rxTimers[key]) clearTimeout(this._rxTimers[key]);
    this._rxTimers[key] = setTimeout(function () {
      delete self._rxParts[key];
      delete self._rxTimers[key];
      self._log('err', '分片重组超时（' + total + ' 片），已丢弃');
    }, FRAG_TIMEOUT_MS);

    const parts = this._rxParts[key].parts;
    let received = 0;
    let len = 0;
    for (let i = 0; i < parts.length; i++) {
      if (parts[i]) {
        received += 1;
        len += parts[i].length;
      }
    }
    this._log('sys', '分片 ' + curr + '/' + total + '，已收 ' + received + ' 片');
    if (received < total) return;

    const full = new Uint8Array(len);
    let off = 0;
    for (let i = 0; i < parts.length; i++) {
      if (!parts[i]) continue;
      full.set(parts[i], off);
      off += parts[i].length;
    }
    delete this._rxParts[key];
    if (this._rxTimers[key]) {
      clearTimeout(this._rxTimers[key]);
      delete this._rxTimers[key];
    }
    this._log('sys', '分片重组完成，共 ' + full.length + ' 字节');
    this._emit('frame', {
      channel: channel,
      bytes: full,
      frame: protocol.parseFrame(full),
    });
  }

  _clearRx() {
    const self = this;
    Object.keys(this._rxTimers).forEach(function (k) {
      clearTimeout(self._rxTimers[k]);
    });
    this._rxParts = {};
    this._rxTimers = {};
  }

  /* ------------------------------ 便捷方法 ------------------------------ */

  /** 查询硬件信息 0x61 */
  queryHw() {
    return this.send(protocol.buildHwQuery());
  }

  /** 配网 0x59 */
  setWifi(opt) {
    return this.send(protocol.buildNetSet(opt));
  }

  /** 下发服务器地址 0xE4 */
  setServer(host, port) {
    return this.send(protocol.buildSrvSet(host, port));
  }

  startMl() {
    return this.send(protocol.buildMlStart());
  }

  stopMl() {
    return this.send(protocol.buildMlStop());
  }

  runMl(x) {
    return this.send(protocol.buildMlRun(x));
  }

  getState() {
    return {
      adapterReady: this.adapterReady,
      scanning: this.scanning,
      connected: this.connected,
      deviceId: this.deviceId,
      deviceName: this.deviceName,
      mtu: this.mtu,
      platform: this.platform,
    };
  }
}

module.exports = new BleManager();
module.exports.UUID = UUID;
module.exports.CHUNK_SIZE = CHUNK_SIZE;
