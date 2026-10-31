'use strict';

const ble = require('../../utils/ble-manager');
const protocol = require('../../utils/protocol');
const store = require('../../utils/store');
const fmt = require('../../utils/format');

Page({
  data: {
    connected: false,
    host: '',
    port: '9000',
    sending: false,
    ackVisible: false,
    ackOk: false,
    ackPort: 0,
    autoReconnect: true,
    deviceCount: 0,
    platform: '',
    version: 'v1.0.0',
    chunkSize: 20,
  },

  onLoad() {
    const self = this;

    this._onFrame = function (p) {
      const f = p.frame;
      if (!f || f.cmd !== protocol.CMD.SRV_SET) return;
      self.setData({ ackVisible: true, ackOk: !!f.ok, ackPort: f.port || 0 });
      if (f.ok) {
        store.setServer(self.data.host, Number(self.data.port));
        wx.showToast({ title: '已保存到设备', icon: 'success' });
      } else {
        wx.showToast({ title: '设备保存失败', icon: 'none' });
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
    const srv = store.getServer();
    this.setData({
      connected: ble.connected,
      host: this.data.host || srv.host || '',
      port: String(this.data.port || srv.port || 9000),
      autoReconnect: store.getAutoReconnect(),
      deviceCount: store.getDevices().length,
      platform: ble.platform,
      chunkSize: ble.CHUNK_SIZE || 20,
    });
    ble.autoReconnect = store.getAutoReconnect();
  },

  onUnload() {
    ble.off('frame', this._onFrame);
    ble.off('connected', this._onConn);
    ble.off('disconnected', this._onDisc);
  },

  onHost(e) {
    this.setData({ host: e.detail.value });
  },

  onPort(e) {
    this.setData({ port: e.detail.value });
  },

  onSend() {
    const self = this;
    let bytes;
    try {
      bytes = protocol.buildSrvSet(this.data.host, this.data.port);
    } catch (e) {
      wx.showModal({ title: '参数有误', content: e.message, showCancel: false });
      return;
    }

    this.setData({ sending: true, ackVisible: false });
    ble
      .send(bytes)
      .then(function () {
        self.setData({ sending: false });
        wx.showToast({ title: '已发送（共 ' + bytes.length + ' 字节）', icon: 'none' });
      })
      .catch(function (e) {
        self.setData({ sending: false });
        wx.showModal({ title: '发送失败', content: e.message, showCancel: false });
      });
  },

  onAutoReconnect(e) {
    const on = !!e.detail.value;
    this.setData({ autoReconnect: on });
    store.setAutoReconnect(on);
    ble.autoReconnect = on;
  },

  onForget() {
    const self = this;
    wx.showModal({
      title: '清除设备记录',
      content: '将清空本机记住的设备列表与 WiFi 历史，不影响设备本身。',
      success: function (r) {
        if (!r.confirm) return;
        store.clearAll();
        self.setData({ deviceCount: 0 });
        wx.showToast({ title: '已清除', icon: 'success' });
      },
    });
  },
});
