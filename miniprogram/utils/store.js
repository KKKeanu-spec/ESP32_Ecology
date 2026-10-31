'use strict';

/**
 * 本地持久化（wx.storage 薄封装）
 *
 * 说明：iOS 的 deviceId 是系统生成的 UUID，换手机/重装小程序后会变，
 * 因此只用作「本机快捷重连」，绝不作为设备唯一标识上送服务端。
 */

const PREFIX = 'race22:';
const LIMIT = {
  devices: 5,
  wifi: 8,
  logs: 200,
};

let ready = false;
const cache = {};

function init() {
  ready = true;
}

function get(key, def) {
  if (key in cache) return cache[key];
  let v;
  try {
    v = wx.getStorageSync(PREFIX + key);
  } catch (e) {
    v = null;
  }
  if (v === '' || v === null || v === undefined) v = def;
  cache[key] = v;
  return v;
}

function set(key, value) {
  cache[key] = value;
  try {
    wx.setStorageSync(PREFIX + key, value);
  } catch (e) {
    // 存储写满时忽略，不影响功能
  }
}

function remove(key) {
  delete cache[key];
  try {
    wx.removeStorageSync(PREFIX + key);
  } catch (e) {}
}

/* ---------------- 业务封装 ---------------- */

/** 记录最近使用的设备（最多 5 台） */
function rememberDevice(dev) {
  if (!dev || !dev.deviceId) return;
  const list = get('devices', []).filter(function (d) {
    return d.deviceId !== dev.deviceId;
  });
  list.unshift({
    deviceId: dev.deviceId,
    name: dev.name || '',
    ts: Date.now(),
  });
  set('devices', list.slice(0, LIMIT.devices));
  set('lastDevice', list[0]);
}

function getDevices() {
  return get('devices', []);
}

function forgetDevices() {
  remove('devices');
  remove('lastDevice');
}

/** WiFi 历史（SSID 列表，不存密码） */
function rememberSsid(ssid) {
  if (!ssid) return;
  const list = get('wifi', []).filter(function (s) {
    return s !== ssid;
  });
  list.unshift(ssid);
  set('wifi', list.slice(0, LIMIT.wifi));
}

function getSsids() {
  return get('wifi', []);
}

/** 云端服务器地址 */
function getServer() {
  return get('server', { host: '', port: 9000 });
}

function setServer(host, port) {
  set('server', { host: host, port: port });
}

/** 自动重连开关，默认开 */
function getAutoReconnect() {
  return get('autoReconnect', true) !== false;
}

function setAutoReconnect(on) {
  set('autoReconnect', !!on);
}

function clearAll() {
  const keys = ['devices', 'lastDevice', 'wifi', 'server', 'autoReconnect'];
  keys.forEach(remove);
}

module.exports = {
  init: init,
  get: get,
  set: set,
  remove: remove,
  rememberDevice: rememberDevice,
  getDevices: getDevices,
  forgetDevices: forgetDevices,
  rememberSsid: rememberSsid,
  getSsids: getSsids,
  getServer: getServer,
  setServer: setServer,
  getAutoReconnect: getAutoReconnect,
  setAutoReconnect: setAutoReconnect,
  clearAll: clearAll,
};
