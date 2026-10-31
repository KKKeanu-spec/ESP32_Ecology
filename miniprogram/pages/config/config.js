'use strict';

const ble = require('../../utils/ble-manager');
const protocol = require('../../utils/protocol');
const store = require('../../utils/store');
const fmt = require('../../utils/format');

const IP_MODES = ['动态获取 (DHCP)', '静态 IP'];

Page({
  data: {
    connected: false,
    ssid: '',
    pwd: '',
    showPwd: false,
    ipModeIdx: 0,
    ipModes: IP_MODES,
    ip: '',
    netmask: '255.255.255.0',
    gw: '',
    dns: '',
    ssids: [],
    sending: false,
    preview: '',
    previewLen: 0,
    chunkCount: 0,
    statusVisible: false,
    statusTime: '',
    status: { recv: false, net: false, mqtt: false },
  },

  onLoad() {
    const self = this;

    this._onFrame = function (p) {
      const f = p.frame;
      if (!f || f.cmd !== protocol.CMD.NET_ACK) return;
      self.setData({
        statusVisible: true,
        statusTime: fmt.time(),
        status: { recv: !!f.recvOk, net: !!f.netOk, mqtt: !!f.mqttOk },
      });
      if (f.recvOk) {
        wx.showToast({ title: '配置已收到', icon: 'none' });
      } else {
        wx.showToast({ title: '配置格式错误', icon: 'none' });
      }
    };

    this._onConn = function () {
      self.setData({ connected: true });
    };
    this._onDisc = function () {
      self.setData({ connected: false });
    };

    ble.on('frame', this._onFrame);
    ble.on('connected', this._onConn);
    ble.on('disconnected', this._onDisc);
  },

  onShow() {
    this.setData({
      connected: ble.connected,
      ssids: store.getSsids(),
    });
    if (!this.data.ssid) {
      const last = store.getSsids()[0];
      if (last) this.setData({ ssid: last });
    }
    this.refreshPreview();
  },

  onUnload() {
    ble.off('frame', this._onFrame);
    ble.off('connected', this._onConn);
    ble.off('disconnected', this._onDisc);
  },

  /* ---------------------------- 表单 ---------------------------- */

  onSsid(e) {
    this.setData({ ssid: e.detail.value });
    this.refreshPreview();
  },

  onPwd(e) {
    this.setData({ pwd: e.detail.value });
    this.refreshPreview();
  },

  onPickSsid(e) {
    this.setData({ ssid: e.currentTarget.dataset.ssid });
    this.refreshPreview();
  },

  togglePwd() {
    this.setData({ showPwd: !this.data.showPwd });
  },

  onIpMode(e) {
    this.setData({ ipModeIdx: Number(e.detail.value) });
    this.refreshPreview();
  },

  onField(e) {
    const key = e.currentTarget.dataset.key;
    const patch = {};
    patch[key] = e.detail.value;
    this.setData(patch);
    this.refreshPreview();
  },

  _build() {
    const d = this.data;
    return protocol.buildNetSet({
      netType: protocol.NET_TYPE.WIFI,
      ipMode: d.ipModeIdx === 0 ? protocol.IP_MODE.DHCP : protocol.IP_MODE.STATIC,
      ssid: d.ssid,
      passwd: d.pwd,
      ip: d.ip,
      netmask: d.netmask,
      gw: d.gw,
      dns: d.dns,
    });
  },

  refreshPreview() {
    try {
      const bytes = this._build();
      this.setData({
        preview: fmt.hexdump(bytes),
        previewLen: bytes.length,
        chunkCount: Math.ceil(bytes.length / 20),
      });
    } catch (e) {
      this.setData({ preview: '', previewLen: 0, chunkCount: 0 });
    }
  },

  /* ---------------------------- 发送 ---------------------------- */

  onSend() {
    const self = this;
    let bytes;
    try {
      bytes = this._build();
    } catch (e) {
      wx.showModal({ title: '参数有误', content: e.message, showCancel: false });
      return;
    }

    if (!ble.connected) {
      wx.showToast({ title: '设备未连接', icon: 'none' });
      return;
    }

    this.setData({ sending: true });
    ble
      .send(bytes)
      .then(function () {
        store.rememberSsid(self.data.ssid);
        self.setData({ sending: false, ssids: store.getSsids() });
        wx.showToast({ title: '已发送，等待应答', icon: 'none' });
      })
      .catch(function (e) {
        self.setData({ sending: false });
        wx.showModal({ title: '发送失败', content: e.message, showCancel: false });
      });
  },
});
