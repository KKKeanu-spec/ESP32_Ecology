'use strict';

const ble = require('../../utils/ble-manager');
const store = require('../../utils/store');

Page({
  data: {
    adapterReady: false,
    scanning: false,
    connected: false,
    deviceName: '',
    deviceId: '',
    mtu: 0,
    platform: '',
    devices: [],
    lastDevice: null,
    err: '',
  },

  onLoad() {
    const self = this;

    this._handlers = {
      adapter: function (s) {
        self.setData({ adapterReady: !!s.ready });
      },
      scan: function (s) {
        self.setData({ scanning: !!s.scanning });
      },
      devices: function (list) {
        self.setData({
          devices: list.map(function (d) {
            return {
              deviceId: d.deviceId,
              name: d.name,
              rssi: d.rssi,
              hitSvc: d.hitSvc,
              cls: d.rssi > -60 ? 'ok' : d.rssi > -80 ? 'warn' : 'err',
            };
          }),
        });
      },
      connected: function (o) {
        self.setData({
          connected: true,
          deviceName: o.name,
          deviceId: o.deviceId,
          mtu: o.mtu,
          err: '',
        });
        store.rememberDevice({ deviceId: o.deviceId, name: o.name });
        self.setData({ lastDevice: store.get('lastDevice', null) });
      },
      disconnected: function (o) {
        self.setData({ connected: false, mtu: 0, err: o.reason || '' });
        self.setData({ lastDevice: store.get('lastDevice', null) });
      },
    };

    Object.keys(this._handlers).forEach(function (k) {
      ble.on(k, self._handlers[k]);
    });
  },

  onShow() {
    const st = ble.getState();
    this.setData({
      adapterReady: st.adapterReady,
      scanning: st.scanning,
      connected: st.connected,
      deviceName: st.deviceName,
      deviceId: st.deviceId,
      mtu: st.mtu,
      platform: st.platform,
      lastDevice: store.get('lastDevice', null),
      devices: ble.devices.map(function (d) {
        return {
          deviceId: d.deviceId,
          name: d.name,
          rssi: d.rssi,
          hitSvc: d.hitSvc,
          cls: d.rssi > -60 ? 'ok' : d.rssi > -80 ? 'warn' : 'err',
        };
      }),
    });
  },

  onUnload() {
    const self = this;
    Object.keys(this._handlers).forEach(function (k) {
      ble.off(k, self._handlers[k]);
    });
  },

  /* ---------------------------- 交互 ---------------------------- */

  onScan() {
    const self = this;
    this.setData({ err: '' });
    ble.startScan().catch(function (e) {
      self.setData({ err: e.message });
      wx.showModal({ title: '无法扫描', content: e.message, showCancel: false });
    });
  },

  onStop() {
    ble.stopScan();
  },

  onConnect(e) {
    const id = e.currentTarget.dataset.id;
    const name = e.currentTarget.dataset.name || '';
    wx.showLoading({ title: '连接中', mask: true });
    ble
      .connect(id, name)
      .then(function () {
        wx.hideLoading();
        wx.showToast({ title: '已连接', icon: 'success' });
      })
      .catch(function (err) {
        wx.hideLoading();
        wx.showModal({ title: '连接失败', content: err.message, showCancel: false });
      });
  },

  onReconnectLast() {
    const d = this.data.lastDevice;
    if (!d || !d.deviceId) return;
    this.onConnect({ currentTarget: { dataset: { id: d.deviceId, name: d.name } } });
  },

  onDisconnect() {
    ble.disconnect();
  },

  goConfig() {
    wx.switchTab({ url: '/pages/config/config' });
  },

  goInfo() {
    wx.navigateTo({ url: '/pages/info/info' });
  },

  goDebug() {
    wx.navigateTo({ url: '/pages/debug/debug' });
  },
});
