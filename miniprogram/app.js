'use strict';

const ble = require('./utils/ble-manager');
const store = require('./utils/store');

App({
  globalData: {
    ble,
    store,
    /** 最近一次连接过的设备，用于「快捷重连」 */
    lastDevice: null,
  },

  onLaunch() {
    store.init();
    const last = store.get('lastDevice', null);
    if (last && last.deviceId) {
      this.globalData.lastDevice = last;
    }
    // 安卓 12+ 需要系统「附近设备」权限，openBluetoothAdapter 会触发系统弹窗
    // iOS 需要用户在小程序设置里允许蓝牙，这里只做能力探测，不主动打扰
  },

  onShow() {
    // 从后台回前台时，适配器可能已被系统关闭
    if (ble.adapterReady) {
      ble
        .open()
        .catch(() => {})
        .then(() => {});
    }
  },

  onError(err) {
    console.error('[app onError]', err);
  },
});
