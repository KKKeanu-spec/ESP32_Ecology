'use strict';

const ble = require('../../utils/ble-manager');
const protocol = require('../../utils/protocol');

const MAX_SAMPLES = 60;
const Y_MIN = -0.15;
const Y_MAX = 1.15;

Page({
  data: {
    connected: false,
    running: false,
    x: 0.25,
    xPct: 25,
    phaseText: '—',
    predText: '—',
    yText: '—',
    count: 0,
    maxSamples: MAX_SAMPLES,
  },

  onLoad() {
    const self = this;
    this._samples = [];
    this._dirty = false;
    this._x = 0.25;
    this._lastPhase = null;
    this._lastPred = null;
    this._lastY = null;

    this._onFrame = function (p) {
      const f = p.frame;
      if (!f) return;

      if (f.cmd === protocol.CMD.ML_RUN) {
        if (f.kind === 'push') {
          self._samples.push({ phase: f.phase, pred: f.pred });
          if (self._samples.length > MAX_SAMPLES) self._samples.shift();
          self._lastPhase = f.phase;
          self._lastPred = f.pred;
          self._dirty = true;
          if (!self.data.running) self.setData({ running: true });
        } else if (f.kind === 'result') {
          self._lastY = f.value;
          self._dirty = true;
        } else if (f.kind === 'ack') {
          wx.showToast({ title: f.ok ? '执行成功' : '执行失败', icon: 'none' });
        }
        return;
      }

      if (f.cmd === protocol.CMD.ML_START || f.cmd === protocol.CMD.ML_STOP) {
        self.setData({ running: f.cmd === protocol.CMD.ML_START && f.ok });
        wx.showToast({ title: f.name + (f.ok ? ' 成功' : ' 失败'), icon: 'none' });
      }
    };

    this._onConn = function () {
      self.setData({ connected: true });
    };
    this._onDisc = function () {
      self.setData({ connected: false, running: false });
    };

    ble.on('frame', this._onFrame);
    ble.on('connected', this._onConn);
    ble.on('disconnected', this._onDisc);
  },

  onShow() {
    this.setData({ connected: ble.connected });
  },

  onReady() {
    const self = this;
    this._initCanvas();
    // 推理结果可能 20Hz 以上，setData 节流到 4Hz，避免页面卡顿
    this._timer = setInterval(function () {
      if (!self._dirty) return;
      self._dirty = false;
      self.setData({
        phaseText: self._lastPhase == null ? '—' : self._lastPhase.toFixed(4),
        predText: self._lastPred == null ? '—' : self._lastPred.toFixed(4),
        yText: self._lastY == null ? '—' : self._lastY.toFixed(4),
        count: self._samples.length,
      });
      self._draw();
    }, 250);
  },

  onUnload() {
    ble.off('frame', this._onFrame);
    ble.off('connected', this._onConn);
    ble.off('disconnected', this._onDisc);
    if (this._timer) clearInterval(this._timer);
  },

  /* ---------------------------- 交互 ---------------------------- */

  onSliding(e) {
    this._x = e.detail.value / 100;
    this.setData({ x: this._x, xPct: e.detail.value });
  },

  onSlid(e) {
    this._x = e.detail.value / 100;
    this.setData({ x: this._x, xPct: e.detail.value });
  },

  onStart() {
    ble.startMl().catch(this._fail);
  },

  onStop() {
    ble.stopMl().catch(this._fail);
  },

  onRun() {
    ble.runMl(this._x).catch(this._fail);
  },

  onClear() {
    this._samples = [];
    this._lastPhase = null;
    this._lastPred = null;
    this._lastY = null;
    this.setData({ count: 0, phaseText: '—', predText: '—', yText: '—' });
    this._draw();
  },

  _fail(e) {
    wx.showModal({ title: '发送失败', content: e.message, showCancel: false });
  },

  /* ---------------------------- 画布 ---------------------------- */

  _initCanvas() {
    const self = this;
    const q = wx.createSelectorQuery().in(this);
    q.select('#chart')
      .fields({ node: true, size: true })
      .exec(function (res) {
        const item = res && res[0];
        if (!item || !item.node) return;
        const canvas = item.node;
        const ctx = canvas.getContext('2d');
        let dpr = 2;
        try {
          const info = wx.getWindowInfo ? wx.getWindowInfo() : wx.getSystemInfoSync();
          dpr = info.pixelRatio || 2;
        } catch (e) {}
        canvas.width = item.width * dpr;
        canvas.height = item.height * dpr;
        ctx.scale(dpr, dpr);
        self._ctx = ctx;
        self._cw = item.width;
        self._ch = item.height;
        self._draw();
      });
  },

  _draw() {
    const ctx = this._ctx;
    if (!ctx) return;
    const w = this._cw;
    const h = this._ch;
    const padL = 36;
    const padR = 10;
    const padT = 10;
    const padB = 18;
    const x0 = padL;
    const x1 = w - padR;
    const y0 = padT;
    const y1 = h - padB;

    const mapY = function (v) {
      let t = (v - Y_MIN) / (Y_MAX - Y_MIN);
      if (t < 0) t = 0;
      if (t > 1) t = 1;
      return y1 - t * (y1 - y0);
    };

    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = '#0d1117';
    ctx.fillRect(0, 0, w, h);

    // 网格 + Y 轴刻度
    ctx.strokeStyle = '#262d38';
    ctx.lineWidth = 1;
    ctx.font = '10px sans-serif';
    ctx.fillStyle = '#6e7681';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    [0, 0.5, 1].forEach(function (v) {
      const y = mapY(v);
      ctx.beginPath();
      ctx.moveTo(x0, y);
      ctx.lineTo(x1, y);
      ctx.stroke();
      ctx.fillText(v.toFixed(1), x0 - 6, y);
    });

    if (!this._samples.length) {
      ctx.fillStyle = '#6e7681';
      ctx.textAlign = 'center';
      ctx.fillText('暂无数据，先启动推理', (x0 + x1) / 2, (y0 + y1) / 2);
      return;
    }

    const n = this._samples.length;
    const step = n > 1 ? (x1 - x0) / (n - 1) : 0;

    const self = this;
    const line = function (key, color) {
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let i = 0; i < n; i++) {
        const px = x0 + step * i;
        const py = mapY(self._samples[i][key]);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    };

    line('phase', '#3fb950');
    line('pred', '#2f81f7');

    // 图例
    ctx.textAlign = 'left';
    ctx.fillStyle = '#3fb950';
    ctx.fillText('phase', x0 + 4, y0 + 10);
    ctx.fillStyle = '#2f81f7';
    ctx.fillText('pred', x0 + 46, y0 + 10);
  },
});
