'use strict';

const ble = require('../../utils/ble-manager');
const protocol = require('../../utils/protocol');
const fmt = require('../../utils/format');

const MAX_SHOW = 100;
let seq = 0;

function toItem(l) {
  const arrow = l.dir === 'tx' ? '↑ ' : l.dir === 'rx' ? '↓ ' : '';
  seq += 1;
  return {
    key: 'k' + l.ts + '_' + seq,
    time: l.time,
    text: arrow + l.text,
    hex: l.hex,
    dir: l.dir,
  };
}

Page({
  data: {
    connected: false,
    channelIdx: 0,
    channelNames: ['命令通道 0xABF3', '数据通道 0xABF1'],
    hexInput: '',
    bytes: 0,
    chunks: 0,
    logs: [],
  },

  onLoad() {
    const self = this;

    this._onLog = function (l) {
      const list = [toItem(l)].concat(self.data.logs).slice(0, MAX_SHOW);
      self.setData({ logs: list });
    };
    this._onConn = function () {
      self.setData({ connected: true });
    };
    this._onDisc = function () {
      self.setData({ connected: false });
    };

    ble.on('log', this._onLog);
    ble.on('connected', this._onConn);
    ble.on('disconnected', this._onDisc);

    this.setData({
      connected: ble.connected,
      logs: ble.logs.slice(-MAX_SHOW).reverse().map(toItem),
    });
  },

  onUnload() {
    ble.off('log', this._onLog);
    ble.off('connected', this._onConn);
    ble.off('disconnected', this._onDisc);
  },

  /* ---------------------------- 输入 ---------------------------- */

  onChannel(e) {
    this.setData({ channelIdx: Number(e.detail.value) });
  },

  onHexInput(e) {
    const text = e.detail.value;
    let bytes = 0;
    try {
      bytes = fmt.parseHex(text).length;
    } catch (err) {
      bytes = 0;
    }
    this.setData({
      hexInput: text,
      bytes: bytes,
      chunks: bytes ? Math.ceil(bytes / 20) : 0,
    });
  },

  onFillHwQuery() {
    this.setData({ hexInput: '61', bytes: 1, chunks: 1 });
  },

  /* ---------------------------- 发送 ---------------------------- */

  _sendBytes(bytes) {
    const channel = this.data.channelIdx === 1 ? 'DATA_RX' : 'CMD_RX';
    ble.send(bytes, { channel: channel }).catch(function (e) {
      wx.showModal({ title: '发送失败', content: e.message, showCancel: false });
    });
  },

  onSend() {
    let bytes;
    try {
      bytes = fmt.parseHex(this.data.hexInput);
    } catch (e) {
      wx.showToast({ title: e.message, icon: 'none' });
      return;
    }
    this._sendBytes(bytes);
  },

  onQuickStart() {
    this._sendBytes(protocol.buildMlStart());
  },

  onQuickStop() {
    this._sendBytes(protocol.buildMlStop());
  },

  onQuickRun() {
    this._sendBytes(protocol.buildMlRun(0.25));
  },

  /* ---------------------------- 日志 ---------------------------- */

  onClear() {
    ble.clearLogs();
    this.setData({ logs: [] });
  },

  onCopy() {
    const text = this.data.logs
      .slice()
      .reverse()
      .map(function (l) {
        return l.time + ' ' + l.text + (l.hex ? ' ' + l.hex : '');
      })
      .join('\n');
    if (!text) {
      wx.showToast({ title: '没有日志', icon: 'none' });
      return;
    }
    wx.setClipboardData({ data: text });
  },
});
