# ESP32 调试平台（Web Bluetooth）

通过浏览器直接连接 ESP32-C3 的 BLE，进行数据收发、WiFi 配置下发、TinyML 触发。

> **硬件前提**：PC 必须配备蓝牙适配器（且支持 BLE 4.0+）。
> 若为无蓝牙的台式机，需外接 USB 蓝牙适配器，或改用 WiFi + HTTP 方案。

---

## 一、快速开始

```bash
cd website
python serve.py
```

浏览器会自动打开 `http://localhost:8000`。

> **必须用 Chrome / Edge / Opera。**
> Firefox 和 Safari（含 iOS 全部浏览器）不支持 Web Bluetooth。

### 为什么必须这样启动

Web Bluetooth 只在**安全上下文**下可用：

| 访问方式 | 能用吗 |
|---|---|
| `http://localhost:8000` | ✅ 可以（浏览器特批 localhost） |
| `https://xxx.com` | ✅ 可以 |
| `file:///C:/.../index.html` | ❌ 不行 |
| `http://192.168.x.x` | ❌ 不行（非 localhost 的 http） |

---

## 二、功能一览

| 功能区 | 说明 | 状态 |
|---|---|---|
| 连接设备 | 扫描并连接 BLE，自动识别 | ✅ 可用 |
| 设备信息 | 显示名称 / ID / 连接状态 | ✅ 可用 |
| GATT 枚举 | 列出所有服务与特征，标注本设备的 | ✅ 可用 |
| WiFi 配置下发 | 按 `0x59` 协议组包并发送 | ⚠️ 待抓包验证 |
| 手动收发 | 任意 HEX 数据的收发 | ✅ 可用 |
| 硬件信息查询 | 发送 `0x61` 查询 | ⚠️ 待抓包验证 |
| TinyML 触发 | 触发 ESP32 推理 | ❌ ESP32 侧未实现 |

---

## 三、目录结构

```
website/
├── index.html          页面结构
├── serve.py            本地服务器（Python 标准库，无依赖）
├── README.md           本文件
├── css/
│   └── style.css       样式（深色主题）
└── js/
    ├── ble.js          Web Bluetooth 封装（连接/订阅/分片重组）
    ├── protocol.js     应用层协议组包解包（0x59 / 0x61 / 0x62）
    └── app.js          UI 逻辑
```

---

## 四、协议实现说明（重要）

### 已确认的

- **字节序**：小端（协议 PDF 尾部明确说明）
- **UUID**：与固件 `components/ble/esp32_ble_server.c` 一致
- **通道划分**：

| 特征 | 方向 | 用途 |
|---|---|---|
| `0xABF1` | 网页 → ESP32 | 数据（WRITE_NO_RSP） |
| `0xABF2` | ESP32 → 网页 | 数据通知（NOTIFY） |
| `0xABF3` | 网页 → ESP32 | 命令（WRITE_NO_RSP，容量 20B） |
| `0xABF4` | ESP32 → 网页 | 状态通知（NOTIFY，容量 20B） |

- **长数据分片**（ESP32 → 网页）：

```
[0x23 0x23][totalNum 1B][currNum 1B][data...]
   '#'  '#'   总片数       当前片号
```

网页侧 `ble.js` 的 `_onNotify()` 已实现自动重组。

### 待验证的

`protocol.js` 里 `0x59` 报文的**字段排列顺序**是按 PDF 表格文字整理的**推测**，尚未用真实抓包核对。

**推测的布局：**

```
[0]      命令码 0x59
[1]      网络类型  0x00=以太网 0x01=4G 0x02=WiFi
[2]      IP方式    0x00=静态   0x01=动态
--- WiFi 时 ---
[3]      SSID 长度
[4]      密码长度
[5..]    静态IP参数（仅静态模式）: ip[4]+mask[4]+gw[4]+dns[4]
[..]     SSID (UTF8)
[..]     密码
--- 可选 ---
addr_len + key_len + addr + key
```

**如果实测不符，只需修改 `protocol.js` 的 `Codec.buildNetSet()` 一个函数。**

---

## 五、如何验证协议（推荐流程）

### 步骤 1：ESP32 侧加抓包日志

固件 `components/ble/esp32_ble_server.c` 里已经加了 3 处调试日志：

```
>>> [PATH-A] 0xABF1 DATA recv, len=N      ← 数据通道收到
>>> [PATH-B] 0xABF3 COMMAND recv, len=N   ← 命令通道收到
>>> [CCCD] 0xABF2 notify cfg: XX XX       ← 订阅开关
```

每处都带 `ESP_LOG_BUFFER_HEXDUMP`，会把原始字节打出来。

### 步骤 2：用官方 APP 配一次 WiFi

用手机 APP 连接 `RACE 22`，执行一次 WiFi 配置操作。

### 步骤 3：分析日志

从日志能确定：

1. APP 走 `0xABF1` 还是 `0xABF3`
2. `0x59` 报文的真实字节布局
3. SSID/密码是否带长度前缀

### 步骤 4：修正 `protocol.js`

按实测结果调整 `Codec.buildNetSet()`。

### 替代方案：直接在本网页里试

也可以绕过 APP，在本网页的「手动发送」里直接写 HEX 试：

```
59 02 01 08 74 65 73 74 77 69 66 69 08 31 32 33 34 35 36 37 38
│  │  │  │  │                    │  │
│  │  │  │  └─ "testwifi"        │  └─ "12345678"
│  │  │  └─ SSID长度=8           └─ 密码长度=8
│  │  └─ 动态IP
│  └─ WiFi
└─ 0x59
```

发送后看 ESP32 是否连上 WiFi，以及是否回 `0x5A` 应答。

---

## 六、常见问题

### 点「连接设备」没反应 / 报错

| 现象 | 原因 | 解决 |
|---|---|---|
| 按钮点了没弹窗 | 不是 Chrome/Edge | 换浏览器 |
| 报 `navigator.bluetooth is undefined` | 非安全上下文 | 用 `python serve.py` 启动 |
| 设备列表为空 | ESP32 没在广播 | 检查固件日志是否有 `Advertising start` |
| 扫到了但连不上 | 距离太远 / 已被别的设备连 | 靠近，或断开其他连接 |
| 连上后没有通知 | 未订阅 | 看控制台有无「已订阅」日志 |

### 之前用手机能连，现在电脑连不上

BLE 从机通常只允许**一个**中心设备连接。先确保手机上已断开（或关掉手机蓝牙）。

### 想看原始收发字节

控制台里所有 TX/RX 都带 HEX 输出。`0xABF1`/`0xABF3` 的写入会显示完整字节。

---

## 七、待办

- [ ] 用真实抓包验证 `0x59` 报文布局，修正 `protocol.js`
- [ ] ESP32 侧实现 `0x61/0x62` 硬件信息应答
- [ ] ESP32 侧实现 BLE 触发 TinyML 推理，结果从 `0xABF2` 推出
- [ ] 按实际 APP UI 调整界面
- [ ] 解析 `0x5A`、`0x62` 应答并显示到界面（`protocol.js` 已备好解析函数，待接）
