'use strict';

/* ==========================================================================
 * protocol.js —— 应用层协议编解码（小程序版）
 *
 * 依据《交流桩与APP通信协议 V1.4》+ 固件 components/ble/ble_app.c 实际实现。
 * 与 website/js/protocol.js 保持同一份语义，可直接对照。
 *
 * 与浏览器版的差异（都是小程序环境限制导致的）：
 *   1. 不用 TextEncoder / TextDecoder，手写 UTF-8 编解码
 *   2. 不用 class / 可选链，兼容低版本基础库
 *   3. 增加 parseFrame()：按命令码自动分发，页面只订阅一次
 *
 * 字节序：小端（协议文档明确说明，固件也用 memcpy 直接拷 float）。
 * ========================================================================== */

/* ------------------------------ 命令码 ------------------------------ */
const CMD = {
  NET_SET: 0x59, // APP -> 设备   设置网络通信方式
  NET_ACK: 0x5a, // 设备 -> APP   设置网络应答
  NET_ALL: 0x5b, // 设备 -> APP   同时开启所有网络
  HW_QUERY: 0x61, // APP -> 设备   查询硬件信息
  HW_ACK: 0x62, // 设备 -> APP   硬件信息应答

  ML_STOP: 0xe0, // APP -> 设备   停止周期推理
  ML_START: 0xe1, // APP -> 设备   启动周期推理
  ML_RUN: 0xe2, // 双向          单次推理 / 结果推送

  SRV_SET: 0xe4, // APP -> 设备   下发云端服务器地址
  SRV_ACK: 0xe4, // 设备 -> APP   应答 [ok][portLo][portHi]
};

const CMD_NAME = {};
CMD_NAME[0x59] = '设置网络';
CMD_NAME[0x5a] = '网络应答';
CMD_NAME[0x5b] = '开启全部网络';
CMD_NAME[0x61] = '查询硬件信息';
CMD_NAME[0x62] = '硬件信息应答';
CMD_NAME[0xe0] = '停止推理';
CMD_NAME[0xe1] = '启动推理';
CMD_NAME[0xe2] = '推理';
CMD_NAME[0xe3] = '推理状态';
CMD_NAME[0xe4] = '服务器地址';

const ACK = { OK: 0x01, FAIL: 0x00 };

/* 网络类型（0x59 载荷第 0 字节） */
const NET_TYPE = { ETHERNET: 0x00, LTE_4G: 0x01, WIFI: 0x02 };
/* IP 获取方式 */
const IP_MODE = { STATIC: 0x00, DHCP: 0x01 };

/* --------------------------- UTF-8 工具 --------------------------- */

function utf8Encode(str) {
  const out = [];
  const s = String(str == null ? '' : str);
  for (let i = 0; i < s.length; i++) {
    let c = s.charCodeAt(i);
    if (c < 0x80) {
      out.push(c);
    } else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      // 代理对 -> 4 字节（emoji / 生僻字）
      const c2 = s.charCodeAt(i + 1);
      if (c2 >= 0xdc00 && c2 <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
        out.push(
          0xf0 | (cp >> 18),
          0x80 | ((cp >> 12) & 0x3f),
          0x80 | ((cp >> 6) & 0x3f),
          0x80 | (cp & 0x3f)
        );
        i++;
        continue;
      }
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return new Uint8Array(out);
}

function utf8Decode(bytes, start, len) {
  const s = start || 0;
  const end = len == null ? bytes.length : Math.min(bytes.length, s + len);
  let out = '';
  let i = s;
  while (i < end) {
    const b = bytes[i];
    if (b < 0x80) {
      out += String.fromCharCode(b);
      i += 1;
    } else if (b >= 0xc0 && b < 0xe0 && i + 1 < end) {
      out += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f));
      i += 2;
    } else if (b >= 0xe0 && b < 0xf0 && i + 2 < end) {
      out += String.fromCharCode(
        ((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f)
      );
      i += 3;
    } else if (b >= 0xf0 && i + 3 < end) {
      const cp =
        ((b & 0x07) << 18) |
        ((bytes[i + 1] & 0x3f) << 12) |
        ((bytes[i + 2] & 0x3f) << 6) |
        (bytes[i + 3] & 0x3f);
      const u = cp - 0x10000;
      out += String.fromCharCode(0xd800 + (u >> 10), 0xdc00 + (u & 0x3ff));
      i += 4;
    } else {
      i += 1;
    }
  }
  return out;
}

/* --------------------------- 小工具 --------------------------- */

function readFloatLE(bytes, off) {
  const u =
    ((bytes[off + 3] << 24) |
      (bytes[off + 2] << 16) |
      (bytes[off + 1] << 8) |
      bytes[off]) >>>
    0; // 小端字节 -> 32 位无符号
  const ab = new ArrayBuffer(4);
  const dv = new DataView(ab);
  dv.setUint32(0, u, false);
  return dv.getFloat32(0, false);
}

function floatBytesLE(x) {
  const ab = new ArrayBuffer(4);
  new DataView(ab).setFloat32(0, x, true); // little-endian
  return new Uint8Array(ab);
}

function cstr(bytes, off, len) {
  let s = '';
  for (let i = 0; i < len && off + i < bytes.length; i++) {
    const c = bytes[off + i];
    if (c === 0) break;
    if (c >= 32 && c < 127) s += String.fromCharCode(c);
  }
  return s;
}

function ip4(bytes, off) {
  return bytes[off] + '.' + bytes[off + 1] + '.' + bytes[off + 2] + '.' + bytes[off + 3];
}

function parseIPv4(str) {
  const parts = String(str || '')
    .split('.')
    .map(function (x) {
      return parseInt(x, 10);
    });
  if (
    parts.length !== 4 ||
    parts.some(function (n) {
      return isNaN(n) || n < 0 || n > 255;
    })
  ) {
    throw new Error('IP 格式错误: ' + str);
  }
  return parts;
}

/* --------------------------- 组包 --------------------------- */

/**
 * 0x59 设置网络通信方式
 *   [0]      0x59
 *   [1]      netType  0x02 = WiFi
 *   [2]      ipMode   0x00 静态 / 0x01 动态
 *   [3]      ssidLen（字节数）
 *   [4]      pwdLen （字节数）
 *   [5..]    SSID 裸串
 *   [.]      密码裸串
 *   --- 仅静态 IP 时追加（当前固件忽略这 16 字节，见 README 说明）---
 *   ip[4] + mask[4] + gw[4] + dns[4]
 */
function buildNetSet(opt) {
  const o = opt || {};
  const netType = o.netType == null ? NET_TYPE.WIFI : o.netType;
  const ipMode = o.ipMode == null ? IP_MODE.DHCP : o.ipMode;
  const ssid = utf8Encode(o.ssid || '');
  const pwd = utf8Encode(o.passwd || '');
  const isWifi = netType === NET_TYPE.WIFI;
  const isStatic = isWifi && ipMode === IP_MODE.STATIC;

  if (isWifi) {
    if (!ssid.length) throw new Error('SSID 不能为空');
    if (ssid.length > 32) throw new Error('SSID 过长（最多 32 字节，中文 1 字 = 3 字节）');
    if (pwd.length > 64) throw new Error('密码过长（最多 64 字节）');
  }

  const chunks = [];
  const u8 = function (v) {
    chunks.push(v & 0xff);
  };

  u8(CMD.NET_SET);
  u8(netType);
  u8(ipMode);
  if (isWifi) {
    u8(ssid.length);
    u8(pwd.length);
    for (let i = 0; i < ssid.length; i++) u8(ssid[i]);
    for (let i = 0; i < pwd.length; i++) u8(pwd[i]);
  }

  if (isStatic) {
    parseIPv4(o.ip).forEach(u8);
    parseIPv4(o.netmask).forEach(u8);
    parseIPv4(o.gw).forEach(u8);
    parseIPv4(o.dns || o.gw).forEach(u8);
  }

  // 注册地址 + 密钥（协议可选段，固件未实现，保留组包能力）
  if (o.regAddr) {
    const addr = utf8Encode(o.regAddr);
    const key = utf8Encode(o.regKey || '');
    u8(addr.length);
    u8(key.length);
    for (let i = 0; i < addr.length; i++) u8(addr[i]);
    for (let i = 0; i < key.length; i++) u8(key[i]);
  }

  return new Uint8Array(chunks);
}

/** 解析 0x59（自检/回显用） */
function parseNetSet(bytes) {
  if (!bytes || bytes.length < 5 || bytes[0] !== CMD.NET_SET) return null;
  const netType = bytes[1];
  const ipMode = bytes[2];
  const out = { netType: netType, ipMode: ipMode };
  if (netType !== NET_TYPE.WIFI) return out;

  const ssidLen = bytes[3];
  const pwdLen = bytes[4];
  let off = 5;
  out.ssid = utf8Decode(bytes, off, ssidLen);
  off += ssidLen;
  out.passwd = utf8Decode(bytes, off, pwdLen);
  off += pwdLen;

  if (ipMode === IP_MODE.STATIC && bytes.length >= off + 16) {
    out.ip = ip4(bytes, off);
    out.netmask = ip4(bytes, off + 4);
    out.gw = ip4(bytes, off + 8);
    out.dns = ip4(bytes, off + 12);
  }
  return out;
}

/** 0x61 查询硬件信息（无载荷） */
function buildHwQuery() {
  return new Uint8Array([CMD.HW_QUERY]);
}

/** 0xE1 / 0xE0 */
function buildMlStart() {
  return new Uint8Array([CMD.ML_START]);
}

function buildMlStop() {
  return new Uint8Array([CMD.ML_STOP]);
}

/** 0xE2 单次推理 [0xE2][float x 小端] */
function buildMlRun(x) {
  const out = new Uint8Array(5);
  out[0] = CMD.ML_RUN;
  out.set(floatBytesLE(Number(x) || 0), 1);
  return out;
}

/**
 * 0xE4 下发云端服务器地址
 *   [0] 0xE4 / [1] portLo / [2] portHi / [3..] host + '\0'
 */
function buildSrvSet(host, port) {
  const h = String(host || '').trim();
  const p = parseInt(port, 10);
  if (!h) throw new Error('服务器地址不能为空');
  if (!(p >= 1 && p <= 65535)) throw new Error('端口无效（1~65535）');
  if (h.length > 63) throw new Error('地址过长（最多 63 字符）');

  const hb = utf8Encode(h);
  const out = new Uint8Array(3 + hb.length + 1);
  out[0] = CMD.SRV_SET;
  out[1] = p & 0xff; // 小端
  out[2] = (p >> 8) & 0xff;
  out.set(hb, 3);
  out[3 + hb.length] = 0; // 终止符
  return out;
}

/* --------------------------- 解包 --------------------------- */

/** 0x5A: [0x5A][收到配置][网络状态][MQTT状态] */
function parseNetAck(bytes) {
  if (!bytes || !bytes.length) return null;
  let body = bytes;
  if (bytes[0] === CMD.NET_ACK) body = bytes.subarray(1);
  return {
    recv: body[0],
    net: body[1],
    mqtt: body[2],
    recvOk: body[0] === ACK.OK,
    netOk: body[1] === ACK.OK,
    mqttOk: body[2] === ACK.OK,
  };
}

/**
 * 0x62 硬件信息应答（固件 ble_app_hw_info 实际布局，去掉命令码后 41 字节）
 *   [0]      协议类型 0x11
 *   [1..2]   协议版本 major, minor
 *   [3..18]  硬件名 char[16]
 *   [19..21] 硬件版本
 *   [22..37] 软件名 char[16]
 *   [38..40] 软件版本
 */
function parseHwAck(bytes) {
  if (!bytes || !bytes.length) return null;
  let body = bytes;
  if (bytes[0] === CMD.HW_ACK) body = bytes.subarray(1);
  if (body.length < 41) return null;

  return {
    protoType: body[0],
    protoVer: body[1] + '.' + body[2],
    hwName: cstr(body, 3, 16),
    hwVer: body[19] + '.' + body[20] + '.' + body[21],
    swName: cstr(body, 22, 16),
    swVer: body[38] + '.' + body[39] + '.' + body[40],
  };
}

/**
 * 0xE2 的三种形态：
 *   2 字节  [0xE2][ok]                     启停/失败应答
 *   5 字节  [0xE2][float y]                单次推理结果
 *   9 字节  [0xE2][float phase][float pred] 周期推送
 */
function parseMl(bytes) {
  if (!bytes || !bytes.length || bytes[0] !== CMD.ML_RUN) return null;
  if (bytes.length >= 9) {
    return {
      kind: 'push',
      phase: readFloatLE(bytes, 1),
      pred: readFloatLE(bytes, 5),
    };
  }
  if (bytes.length >= 5) {
    return { kind: 'result', value: readFloatLE(bytes, 1) };
  }
  if (bytes.length >= 2) {
    return { kind: 'ack', ok: bytes[1] === ACK.OK };
  }
  return null;
}

/** 0xE4 应答: [0xE4][ok][portLo][portHi] */
function parseSrvAck(bytes) {
  if (!bytes || bytes.length < 2) return null;
  let body = bytes;
  if (bytes[0] === CMD.SRV_SET) body = bytes.subarray(1);
  return {
    ok: body[0] === ACK.OK,
    port: body.length >= 3 ? body[1] | (body[2] << 8) : 0,
  };
}

/** 0xE0 / 0xE1 应答: [cmd][ok] */
function parseAck(bytes) {
  if (!bytes || bytes.length < 2) return null;
  return { cmd: bytes[0], ok: bytes[1] === ACK.OK };
}

/**
 * 按命令码自动解包。页面只需要订阅一次 frame 事件。
 * 返回 { cmd, name, ...payload } 或 null（长度不足/未知命令）
 */
function parseFrame(bytes) {
  if (!bytes || !bytes.length) return null;
  const cmd = bytes[0];
  const base = { cmd: cmd, name: CMD_NAME[cmd] || '未知(0x' + cmd.toString(16).toUpperCase() + ')' };

  switch (cmd) {
    case CMD.NET_ACK: {
      const r = parseNetAck(bytes);
      return r ? Object.assign(base, r) : null;
    }
    case CMD.HW_ACK: {
      const r = parseHwAck(bytes);
      return r ? Object.assign(base, r) : null;
    }
    case CMD.ML_RUN: {
      const r = parseMl(bytes);
      return r ? Object.assign(base, r) : null;
    }
    case CMD.SRV_SET: {
      const r = parseSrvAck(bytes);
      return r ? Object.assign(base, r) : null;
    }
    case CMD.ML_STOP:
    case CMD.ML_START: {
      const r = parseAck(bytes);
      return r ? Object.assign(base, r) : null;
    }
    default:
      return base;
  }
}

/** 一句话描述一帧，用于日志 */
function describe(frame) {
  if (!frame) return '';
  switch (frame.cmd) {
    case CMD.NET_ACK:
      return (
        '配置' +
        (frame.recvOk ? '已收到' : '格式错误') +
        ' / 网络' +
        (frame.netOk ? '已连接' : '未连接') +
        ' / MQTT' +
        (frame.mqttOk ? '已连接' : '未连接')
      );
    case CMD.HW_ACK:
      return (
        frame.hwName +
        ' ' +
        frame.hwVer +
        ' | ' +
        frame.swName +
        ' ' +
        frame.swVer +
        ' | 协议 ' +
        frame.protoType +
        ' v' +
        frame.protoVer
      );
    case CMD.ML_RUN:
      if (frame.kind === 'push') {
        return '推送 phase=' + frame.phase.toFixed(4) + ' pred=' + frame.pred.toFixed(4);
      }
      if (frame.kind === 'result') return '单次推理结果 y=' + frame.value.toFixed(4);
      return '推理应答 ' + (frame.ok ? '成功' : '失败');
    case CMD.ML_START:
    case CMD.ML_STOP:
      return frame.name + (frame.ok ? ' 成功' : ' 失败');
    case CMD.SRV_SET:
      return '服务器配置' + (frame.ok ? '成功' : '失败') + ' 端口=' + frame.port;
    default:
      return frame.name;
  }
}

module.exports = {
  CMD: CMD,
  CMD_NAME: CMD_NAME,
  ACK: ACK,
  NET_TYPE: NET_TYPE,
  IP_MODE: IP_MODE,
  utf8Encode: utf8Encode,
  utf8Decode: utf8Decode,
  buildNetSet: buildNetSet,
  parseNetSet: parseNetSet,
  buildHwQuery: buildHwQuery,
  buildMlStart: buildMlStart,
  buildMlStop: buildMlStop,
  buildMlRun: buildMlRun,
  buildSrvSet: buildSrvSet,
  parseNetAck: parseNetAck,
  parseHwAck: parseHwAck,
  parseMl: parseMl,
  parseSrvAck: parseSrvAck,
  parseFrame: parseFrame,
  describe: describe,
};
