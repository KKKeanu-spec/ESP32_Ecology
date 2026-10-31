'use strict';

const ble = require('../../utils/ble-manager');
const protocol = require('../../utils/protocol');
const fmt = require('../../utils/format');

Page({
  data: {
    connected: false,
    querying: false,
    queried: false,
    queriedAt: '',
    info: null,
    protoTypeHex: '',
    rawHex: '',
    rawLen: 0,
    deviceName: '',
    deviceId: '',
    serviceUuid: '00010108-0000-1000-8000-00805F9B34FB',
    mtu: 23,
    charsText: '',
  },

  onLoad() {
    const self = this;

    this._onFrame = function (p) {
      const f = p.frame;
      if (!f || f.cmd !== protocol.CMD.HW_ACK) return;

      self.setData({
        querying: false,
        queried: true,
        queriedAt: fmt.time(),
        info: {
          protoType: f.protoType,
          protoVer: f.protoVer,
          hwName: f.hwName,
          hwVer: f.hwVer,
          swName: f.swName,
          swVer: f.swVer,
        },
        protoTypeHex: ('0' + f.protoType.toString(16).toUpperCase()).slice(-2),
        rawHex: fmt.hexdump(p.bytes),
        rawLen: p.bytes.length,
      });
    };

    this._onConn = function () {
      self.setData({ connected: true });
      self._syncDetail();
    };
    this._onDisc = function () {
      self.setData({ connected: false, querying: false });
    };

    ble.on('frame', this._onFrame);
    ble.on('connected', this._onConn);
    ble.on('disconnected', this._onDisc);
  },

  onShow() {
    this.setData({ connected: ble.connected });
    this._syncDetail();
  },

  onUnload() {
    ble.off('frame', this._onFrame);
    ble.off('connected', this._onConn);
    ble.off('disconnected', this._onDisc);
  },

  _syncDetail() {
    const keys = Object.keys(ble.charIds || {});
    const map = { DATA_RX: '0xABF1', DATA_TX: '0xABF2', CMD_RX: '0xABF3', STATUS_TX: '0xABF4' };
    this.setData({
      deviceName: ble.deviceName,
      deviceId: ble.deviceId,
      mtu: ble.mtu,
      charsText: keys.length
        ? keys
            .map(function (k) {
              return map[k] || k;
            })
            .join(' / ')
        : '—',
    });
  },

  onQuery() {
    const self = this;
    this.setData({ querying: true });
    ble
      .queryHw()
      .then(function () {
        setTimeout(function () {
          if (self.data.querying) {
            self.setData({ querying: false });
            wx.showToast({ title: '已发送，未收到 0x62 应答', icon: 'none' });
          }
        }, 3000);
      })
      .catch(function (e) {
        self.setData({ querying: false });
        wx.showModal({ title: '发送失败', content: e.message, showCancel: false });
      });
  },

  onCopy() {
    if (!this.data.rawHex) return;
    wx.setClipboardData({ data: this.data.rawHex });
  },
});
