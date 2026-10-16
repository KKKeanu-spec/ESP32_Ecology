# 部署到公网 HTTPS（GitHub Pages）

## ⚠️ 先读这一段：部署前必须理解的限制

**Web Bluetooth 的架构决定了：浏览器只能连「运行它的那台电脑」附近的蓝牙设备。**

部署到公网后：

```
同事 A 打开你的网址 → 弹出【同事 A 电脑】的蓝牙列表 → 能连他手边的 ESP32
同事 B 打开你的网址 → 弹出【同事 B 电脑】的蓝牙列表 → 能连他手边的 ESP32
你远程打开网址     → 弹出【你自己电脑】的蓝牙列表 → 连不到现场的 ESP32
```

**所以公网部署的用途是「把工具分发给多人，各自连自己面前的板子」，**
**不是「远程控制现场的 ESP32」。**

如果你要的是**真正的远程控制**，Web Bluetooth 做不到，需要改用：

```
浏览器 ──HTTPS/WebSocket──→ 云服务器 ──MQTT──→ ESP32（现场）
```

推荐用 MQTT（ESP32 生态最成熟，有现成的 esp-mqtt 组件）。

---

## 部署步骤（假设你走「分发工具」这条路）

### 前提

- GitHub 账号（免费注册：https://github.com/signup）
- Git（ESP-IDF 里已自带，路径见下方）

### 方式一：网页操作（最简单，不用命令行）

1. 登录 GitHub，点右上角 **+** → **New repository**
2. 仓库名填 `esp32-console`，选 **Public**，点 **Create repository**
3. 进入仓库，点 **uploading an existing file**
4. 把 `website/` 目录下这些**全部拖进去**：

```
index.html
README.md          （可选）
.nojekyll          ← 必须上传（注意是隐藏文件）
css/style.css
js/ble.js
js/protocol.js
js/app.js
```

> `serve.py` 不用上传（那是本地调试用的）

5. 点 **Commit changes**
6. 进入 **Settings → Pages**
7. **Source** 选 `Deploy from a branch`，**Branch** 选 `main` + `/ (root)`，点 **Save**
8. 等 1~2 分钟，页面顶部会显示网址：

```
https://<你的用户名>.github.io/esp32-console/
```

**这就是你的 HTTPS 地址，任何人打开都能用。**

### 方式二：命令行（如果你熟悉 Git）

ESP-IDF 自带 git，路径：

```
D:\ESP32\ESP_IDF\v5.3.3\tools\idf-git\2.44.0\cmd\git.exe
```

操作：

```powershell
cd c:\Users\huangguoyuan\Desktop\fcu_pwm_710\TinyML_ESP32C3\website

# 初始化仓库
& "D:\ESP32\ESP_IDF\v5.3.3\tools\idf-git\2.44.0\cmd\git.exe" init
& "...\git.exe" add .
& "...\git.exe" commit -m "ESP32 调试平台"

# 关联远程仓库（换成你自己的地址）
& "...\git.exe" remote add origin https://github.com/<你的用户名>/esp32-console.git
& "...\git.exe" branch -M main
& "...\git.exe" push -u origin main
```

推完后同样去 **Settings → Pages** 开启。

---

## 部署后的注意事项

### 1. 浏览器要求不变

| 浏览器 | 支持 |
|---|---|
| Chrome / Edge / Opera（桌面） | ✅ |
| 安卓 Chrome | ✅ |
| **iOS 全系（含 Chrome）** | ❌ 不支持 |
| Firefox / Safari | ❌ 不支持 |

**页面里已内置检测**，不支持的浏览器会直接提示原因，不会白屏。

### 2. 必须 HTTPS

GitHub Pages 自动提供 HTTPS，**不用管**。

### 3. 每次点「连接设备」都要重新选设备

这是 Web Bluetooth 的安全规定：**必须用户手势触发，且每次都要手动选设备**，不能记住上次的选择、不能静默连接。

### 4. 一个 ESP32 同时只能被一台设备连

BLE 从机通常只接受一个中心设备。**如果手机 APP 连着，电脑就连不上** —— 需要先断开手机。

### 5. 页面不能后台运行

标签页切到后台、或锁屏，可能导致连接断开。这是浏览器的省电策略。

---

## 本地 vs 公网：怎么选

| | 本地（`serve.py`） | 公网（GitHub Pages） |
|---|---|---|
| 谁用 | 只有你这台电脑 | 所有人 |
| 需要 HTTPS | 不用（localhost 特批） | 自动有 |
| 改代码后 | 刷新即可 | 要重新 push |
| 适用 | 开发调试 | 分发给同事 |

**建议：先在本地调通，再部署公网。**

---

## 常见问题

### 部署后页面打开是 404

- 检查仓库 **Settings → Pages** 是否已开启
- 检查 `index.html` 是否在仓库**根目录**（不是在子文件夹里）
- 等 2~3 分钟（首次部署有延迟）

### 部署后样式丢失、JS 报 404

检查是否用了绝对路径。本项目已全部使用**相对路径**（`css/style.css`），
若你手动改过，必须保证不是以 `/` 开头。

### 同事打开后扫不到设备

1. 确认他电脑有蓝牙且蓝牙已开
2. 确认他的浏览器是 Chrome / Edge
3. 确认他的 ESP32 在他附近且已通电广播
4. 确认没有手机正连着那块 ESP32
