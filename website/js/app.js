/* ==========================================================================
 * app.js —— UI 逻辑，把界面事件接到 BleClient 上
 * ========================================================================== */

'use strict';

(function () {

    /* ----------------------------------------------------------------------
     * DOM 引用
     * -------------------------------------------------------------------- */
    const $ = (id) => document.getElementById(id);

    const el = {
        dot: $('statusDot'),
        statusText: $('statusText'),
        btnConnect: $('btnConnect'),
        btnDisconnect: $('btnDisconnect'),

        // ---- 连接状态 ----
        stBle: $('stBle'),
        stWifi: $('stWifi'),
        stSsid: $('stSsid'),
        stIp: $('stIp'),
        stMqtt: $('stMqtt'),
        stHint: $('stHint'),

        infoName: $('infoName'),
        infoId: $('infoId'),
        infoProtoType: $('infoProtoType'),
        infoProtoVer: $('infoProtoVer'),
        btnQueryHw: $('btnQueryHw'),

        gattList: $('gattList'),

        ssid: $('ssid'),
        passwd: $('passwd'),
        staticIpGroup: $('staticIpGroup'),
        ip: $('ip'),
        netmask: $('netmask'),
        gw: $('gw'),
        dns: $('dns'),
        btnBuildWifi: $('btnBuildWifi'),
        btnSendWifi: $('btnSendWifi'),
        wifiHex: $('wifiHex'),

        // ---- 云端上报 ----
        srvHost: $('srvHost'),
        srvPort: $('srvPort'),
        btnUseLocalIp: $('btnUseLocalIp'),
        btnSendSrv: $('btnSendSrv'),
        srvAck: $('srvAck'),

        console: $('console'),
        autoScroll: $('autoScroll'),
        btnClearLog: $('btnClearLog'),

        // ---- 0xABF1 数据接收 ----
        f1Hex: $('f1Hex'),
        f1Text: $('f1Text'),
        f1SendHex: $('f1SendHex'),
        f1SendText: $('f1SendText'),
        f1Info: $('f1Info'),

        // ---- 0xABF2 数据通知 ----
        f2SubState: $('f2SubState'),
        f2Subscribe: $('f2Subscribe'),
        f2Unsubscribe: $('f2Unsubscribe'),
        f2Last: $('f2Last'),
        f2Info: $('f2Info'),

        // ---- 0xABF3 命令接收 ----
        f3Hex: $('f3Hex'),
        f3Send: $('f3Send'),
        f3QueryHw: $('f3QueryHw'),
        f3Info: $('f3Info'),

        // ---- 0xABF4 状态通知 ----
        f4SubState: $('f4SubState'),
        f4Subscribe: $('f4Subscribe'),
        f4Unsubscribe: $('f4Unsubscribe'),
        f4Last: $('f4Last'),
        f4Info: $('f4Info'),

        // ---- TinyML ----
        mlRunState: $('mlRunState'),
        btnMlStart: $('btnMlStart'),
        btnMlStop: $('btnMlStop'),
        mlInput: $('mlInput'),
        mlPath: $('mlPath'),
        btnMlRun: $('btnMlRun'),
        mlLast: $('mlLast'),
        mlCount: $('mlCount'),
        mlTbody: $('mlTbody'),

        footMsg: $('footMsg'),
    };

    const ble = new BleClient();

    /* ----------------------------------------------------------------------
     * 日志输出
     * -------------------------------------------------------------------- */
    let logCount = 0;

    function log(dir, msg) {
        if (logCount === 0) el.console.innerHTML = '';

        const now = new Date();
        const ts = [now.getHours(), now.getMinutes(), now.getSeconds()]
            .map(n => String(n).padStart(2, '0')).join(':')
            + '.' + String(now.getMilliseconds()).padStart(3, '0');

        const dirText = { tx: 'TX →', rx: 'RX ←', sys: 'SYS ', err: 'ERR ' }[dir] || '??? ';

        const line = document.createElement('div');
        line.className = 'line';

        const spanTs = document.createElement('span');
        spanTs.className = 'ts';
        spanTs.textContent = ts;

        const spanDir = document.createElement('span');
        spanDir.className = 'dir ' + dir;
        spanDir.textContent = dirText;

        const spanMsg = document.createElement('span');
        spanMsg.className = 'msg';
        spanMsg.textContent = msg;

        line.appendChild(spanTs);
        line.appendChild(spanDir);
        line.appendChild(spanMsg);
        el.console.appendChild(line);

        logCount++;

        // 限制条数，防内存膨胀
        while (el.console.childElementCount > 500) {
            el.console.removeChild(el.console.firstChild);
        }

        if (el.autoScroll.checked) {
            el.console.scrollTop = el.console.scrollHeight;
        }
    }

    function foot(msg) {
        el.footMsg.textContent = msg;
    }

    /* ----------------------------------------------------------------------
     * 连接状态显示
     * -------------------------------------------------------------------- */
    /** 更新一个状态胶囊
     * @param {HTMLElement} box  状态元素
     * @param {'off'|'on'|'pending'|'err'} state
     * @param {string} text
     */
    function setPill(box, state, text) {
        if (!box) return;
        box.className = 'stat-pill ' + state;
        const t = box.querySelector('.stat-text');
        if (t) t.textContent = text;
    }

    /** BLE 连接状态 */
    function setBleState(state, text) {
        setPill(el.stBle, state, text);
    }

    /** WiFi 连接状态 */
    function setWifiState(state, text) {
        setPill(el.stWifi, state, text);
    }

    /** 把 WiFi 状态重置为未知（断开 BLE 时调用） */
    function resetWifiState() {
        setWifiState('off', '未知');
        el.stSsid.textContent = '—';
        el.stIp.textContent = '—';
        el.stMqtt.textContent = '—';
        el.stHint.textContent = 'WiFi 状态由 ESP32 上报（0x5A 应答）';
    }

    /* ----------------------------------------------------------------------
     * BLE 事件绑定
     * -------------------------------------------------------------------- */
    BleEvents.onLog = log;

    BleEvents.onStatus = (text, state) => {
        el.statusText.textContent = text;
        el.dot.className = 'dot' + (state === 'ok' ? ' on' : state === 'err' ? ' err' : '');

        // 同步左侧状态胶囊
        if (state === 'ok') {
            setBleState('on', '已连接');
        } else if (state === 'err') {
            setBleState('err', '已断开');
        } else {
            setBleState('pending', text);
        }
    };

    BleEvents.onDisconnect = () => {
        setUiConnected(false);
        setBleState('err', '已断开');
        resetWifiState();
    };

    BleEvents.onGatt = (list) => {
        renderGatt(list);
    };

    /* ----------------------------------------------------------------------
     * 渲染 GATT 列表
     * -------------------------------------------------------------------- */
    const KNOWN_SVC = {
        '00001800-0000-1000-8000-00805f9b34fb': 'Generic Access',
        '00001801-0000-1000-8000-00805f9b34fb': 'Generic Attribute',
        '00010108-0000-1000-8000-00805f9b34fb': '产品服务 ★',
    };

    const KNOWN_CHR = {
        '00002a00-0000-1000-8000-00805f9b34fb': 'Device Name',
        '00002a01-0000-1000-8000-00805f9b34fb': 'Appearance',
        '00002a05-0000-1000-8000-00805f9b34fb': 'Service Changed',
        '0000abf1-0000-1000-8000-00805f9b34fb': '数据接收 ★',
        '0000abf2-0000-1000-8000-00805f9b34fb': '数据通知 ★',
        '0000abf3-0000-1000-8000-00805f9b34fb': '命令接收 ★',
        '0000abf4-0000-1000-8000-00805f9b34fb': '状态通知 ★',
    };

    function shortUuid(u) {
        return u.toLowerCase().replace('-0000-1000-8000-00805f9b34fb', '');
    }

    function renderGatt(list) {
        el.gattList.innerHTML = '';
        if (!list || list.length === 0) {
            el.gattList.innerHTML = '<p class="hint">未枚举到服务</p>';
            return;
        }

        list.forEach(svc => {
            const div = document.createElement('div');
            div.className = 'svc';

            const nameEl = document.createElement('div');
            nameEl.className = 'svc-name';
            nameEl.textContent = (KNOWN_SVC[svc.uuid] || 'Unknown Service') + '  ' + shortUuid(svc.uuid);
            div.appendChild(nameEl);

            (svc.chars || []).forEach(c => {
                const cdiv = document.createElement('div');
                cdiv.className = 'chr' + (c.ours ? ' ours' : '');

                const props = [];
                if (c.props.read) props.push('READ');
                if (c.props.write) props.push('WRITE');
                if (c.props.writeWithoutResponse) props.push('WRITE_NR');
                if (c.props.notify) props.push('NOTIFY');

                cdiv.innerHTML = `<span class="chr-name">${KNOWN_CHR[c.uuid] || 'Unknown'}</span> `
                    + `${shortUuid(c.uuid)}<br><span class="prop">${props.join(' · ')}</span>`;
                div.appendChild(cdiv);
            });

            el.gattList.appendChild(div);
        });
    }

    /* ----------------------------------------------------------------------
     * 连接 / 断开
     * -------------------------------------------------------------------- */
    function setUiConnected(on) {
        el.btnConnect.disabled = on;
        el.btnDisconnect.disabled = !on;
        el.btnQueryHw.disabled = !on;
        el.btnSendWifi.disabled = !on;
        el.btnMlRun.disabled = !on;
        el.btnMlStart.disabled = !on;
        el.btnMlStop.disabled = !on;
        el.btnSendSrv.disabled = !on;

        if (!on) {
            setMlRunning(false);
            el.mlRunState.textContent = '未知（未连接）';
        }

        // 四个特征面板
        el.f1SendHex.disabled = !on;
        el.f1SendText.disabled = !on;
        el.f2Subscribe.disabled = !on;
        el.f4Subscribe.disabled = !on;
        el.f3Send.disabled = !on;
        el.f3QueryHw.disabled = !on;

        // 取消订阅按钮：连上且已订阅时才可用
        el.f2Unsubscribe.disabled = !on;
        el.f4Unsubscribe.disabled = !on;

        // 面板提示
        const tip = on ? '就绪' : '未连接';
        [el.f1Info, el.f2Info, el.f3Info, el.f4Info].forEach(x => { if (x) x.textContent = tip; });

        if (!on) {
            [el.f2SubState, el.f4SubState].forEach(box => {
                if (box) { box.textContent = '未订阅'; box.className = 'sub-state off'; }
            });
        }
    }

    el.btnConnect.addEventListener('click', async () => {
        setBleState('pending', '连接中…');
        try {
            const ok = await ble.connect();
            if (ok) {
                setUiConnected(true);
                setBleState('on', '已连接');
                el.infoName.textContent = ble.device.name || '(无名)';
                el.infoId.textContent = ble.device.id || '—';
                syncSubState();
                foot('已连接 · 可以开始收发数据');
                try { sessionStorage.setItem('esp32_was_connected', '1'); } catch (e) { /* ignore */ }
            } else {
                setBleState('off', '未连接');
            }
        } catch (e) {
            log('err', '连接失败: ' + e.message);
            setStatus('连接失败');
            el.dot.className = 'dot err';
            setBleState('err', '连接失败');
            foot('连接失败: ' + e.message);
        }
    });

    el.btnDisconnect.addEventListener('click', () => {
        ble.disconnect();
        setUiConnected(false);
        resetWifiState();
    });

    /* ----------------------------------------------------------------------
     * WiFi 报文预览
     * -------------------------------------------------------------------- */
    function currentWifiOpt() {
        const netType = parseInt(document.querySelector('input[name=netType]:checked').value, 10);
        const ipMode = parseInt(document.querySelector('input[name=ipMode]:checked').value, 10);
        return {
            netType,
            ipMode,
            ssid: el.ssid.value,
            passwd: el.passwd.value,
            ip: el.ip.value,
            netmask: el.netmask.value,
            gw: el.gw.value,
            dns: el.dns.value,
        };
    }

    function refreshWifiPreview() {
        try {
            const bytes = Codec.buildNetSet(currentWifiOpt());
            el.wifiHex.textContent = Codec.toHexDump(bytes);
        } catch (e) {
            el.wifiHex.textContent = '组包失败: ' + e.message;
        }
    }

    // 静态 IP 面板启用/禁用
    document.querySelectorAll('input[name=ipMode]').forEach(r => {
        r.addEventListener('change', () => {
            el.staticIpGroup.disabled = document.querySelector('input[name=ipMode]:checked').value !== '0';
            refreshWifiPreview();
        });
    });

    document.querySelectorAll('input[name=netType]').forEach(r => {
        r.addEventListener('change', refreshWifiPreview);
    });

    [el.ssid, el.passwd, el.ip, el.netmask, el.gw, el.dns].forEach(inp => {
        inp.addEventListener('input', refreshWifiPreview);
    });

    el.btnBuildWifi.addEventListener('click', refreshWifiPreview);

    el.btnSendWifi.addEventListener('click', async () => {
        try {
            const bytes = Codec.buildNetSet(currentWifiOpt());

            // 回显校验：把自己组的包解回来，确认与输入一致
            const back = Codec.parseNetSet(bytes);
            if (back && back.ssid !== undefined) {
                log('sys', `组包回显校验: SSID="${back.ssid}" (${back.ssid.length}B) `
                    + `密码="${'*'.repeat(back.passwd.length)}" (${back.passwd.length}B) `
                    + `网络=${back.netType} IP方式=${back.ipMode}`);
            }

            const path = document.querySelector('input[name=txPath]:checked').value;
            await send(path, bytes);
            log('sys', `WiFi 配置已发送 (${bytes.length} 字节)，等待 0x5A 应答…`);

            // 立刻显示"连接中"，不必等应答回来
            setWifiState('pending', '连接中…');
            el.stSsid.textContent = el.ssid.value || '—';
            el.stIp.textContent = '—';
            el.stHint.textContent = '已下发配置，等待 ESP32 连接 WiFi…';
        } catch (e) {
            log('err', '发送失败: ' + e.message);
            setWifiState('err', '下发失败');
        }
    });

    /* ----------------------------------------------------------------------
     * 统一发送入口
     * -------------------------------------------------------------------- */
    async function send(pathStr, bytes) {
        const uuid = parseInt(pathStr.replace('0x', ''), 16);
        await ble.write(uuid, bytes);
        log('tx', `${pathStr} [${bytes.length}B] ${Codec.toHex(bytes)}`);
    }

    /* ======================================================================
     * 0xABF1 —— 数据接收（写入）
     * ==================================================================== */
    el.f1SendHex.addEventListener('click', async () => {
        try {
            const bytes = Protocol.parseHex(el.f1Hex.value);
            if (bytes.length === 0) throw new Error('数据为空');
            if (bytes.length > 512) log('sys', `警告: ${bytes.length}B 超过特征容量声明(512B)`);
            await send('0xABF1', bytes);
        } catch (e) {
            log('err', '0xABF1 发送失败: ' + e.message);
        }
    });

    el.f1SendText.addEventListener('click', async () => {
        try {
            const txt = el.f1Text.value;
            if (!txt) throw new Error('文本为空');
            const bytes = new TextEncoder().encode(txt);
            await send('0xABF1', bytes);
        } catch (e) {
            log('err', '0xABF1 发送失败: ' + e.message);
        }
    });

    /* ======================================================================
     * 0xABF2 —— 数据通知（订阅 / 取消）
     * ==================================================================== */
    el.f2Subscribe.addEventListener('click', () => ble.subscribe(0xABF2));
    el.f2Unsubscribe.addEventListener('click', () => ble.unsubscribe(0xABF2));

    /* ======================================================================
     * 0xABF3 —— 命令接收（写入，20B 上限）
     * ==================================================================== */
    el.f3Send.addEventListener('click', async () => {
        try {
            const bytes = Protocol.parseHex(el.f3Hex.value);
            if (bytes.length === 0) throw new Error('命令为空');
            if (bytes.length > 20) log('sys', `警告: ${bytes.length}B 超过命令特征上限(20B)`);
            await send('0xABF3', bytes);
        } catch (e) {
            log('err', '0xABF3 发送失败: ' + e.message);
        }
    });

    el.f3QueryHw.addEventListener('click', async () => {
        try {
            el.f3Hex.value = '61';
            await send('0xABF3', Codec.buildHwQuery());
            log('sys', '已发送硬件信息查询 (0x61)，等待 0x62 应答…');
        } catch (e) {
            log('err', '0xABF3 发送失败: ' + e.message);
        }
    });

    /* ======================================================================
     * 0xABF4 —— 状态通知（订阅 / 取消）
     * ==================================================================== */
    el.f4Subscribe.addEventListener('click', () => ble.subscribe(0xABF4));
    el.f4Unsubscribe.addEventListener('click', () => ble.unsubscribe(0xABF4));

    /* ======================================================================
     * 接收回调 —— 更新 0xABF2 / 0xABF4 面板
     * ==================================================================== */
    BleEvents.onData = (uuid, bytes) => {
        renderLast(el.f2Last, bytes);

        // TinyML 相关回包优先处理
        if (bytes.length > 0 && handleTinyMlResponse(bytes)) return;

        // 其余按协议解析
        tryParseAck(bytes);
    };

    BleEvents.onStatusNotify = (uuid, bytes) => {
        renderLast(el.f4Last, bytes);
    };

    BleEvents.onSubState = (uuid, subscribed) => {
        const box = uuid === 0xABF2 ? el.f2SubState : el.f4SubState;
        if (!box) return;
        box.textContent = subscribed ? '已订阅' : '未订阅';
        box.className = 'sub-state ' + (subscribed ? 'on' : 'off');

        const info = uuid === 0xABF2 ? el.f2Info : el.f4Info;
        if (info) info.textContent = subscribed ? '订阅中，等待数据…' : '已取消订阅';
    };

    /** 把当前订阅状态同步到界面（连接完成后调用） */
    function syncSubState() {
        BleEvents.onSubState(0xABF2, ble.subscribed.has(0xABF2));
        BleEvents.onSubState(0xABF4, ble.subscribed.has(0xABF4));
    }

    /** 渲染"最近接收"（HEX + ASCII） */
    function renderLast(box, bytes) {
        if (!box) return;
        const hex = Protocol.formatBytes(bytes);
        const ascii = Array.from(bytes)
            .map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.')
            .join('');
        box.textContent = `${bytes.length} B\n${hex}\n${ascii}`;
    }

    /** 尝试把接收到的字节按已知协议解析并输出到控制台 */
    function tryParseAck(bytes) {
        if (bytes.length === 0) return;
        const cmd = bytes[0];

        if (cmd === CMD.NET_ACK) {
            const ack = Codec.parseNetAck(bytes);
            if (ack) {
                const f = (v) => v === 1 ? '成功' : v === 0 ? '失败' : '—';
                log('sys', `解析 0x5A 应答: 收到配置=${f(ack.recv)} 网络=${f(ack.net)} MQTT=${f(ack.mqtt)}`);
                foot(`WiFi 配置: 收到=${f(ack.recv)} 网络=${f(ack.net)} MQTT=${f(ack.mqtt)}`);

                // ---- 更新 WiFi 状态显示 ----
                if (ack.recv === 0) {
                    setWifiState('err', '配置被拒绝');
                    el.stHint.textContent = 'ESP32 拒绝了本次配置（格式或网络类型不支持）';
                } else if (ack.net === 1) {
                    setWifiState('on', '已连接');
                    el.stSsid.textContent = el.ssid.value || '—';
                    el.stIp.textContent = '(协议未携带)';
                    el.stHint.textContent = 'WiFi 已连接（IP 请从 ESP32 串口日志查看）';
                } else {
                    // 收到配置成功但网络尚未连通 → 等待中
                    setWifiState('pending', '连接中…');
                    el.stSsid.textContent = el.ssid.value || '—';
                    el.stHint.textContent = '等待 ESP32 连接 WiFi（高亮为连接中）';
                }

                // MQTT 段
                el.stMqtt.textContent = f(ack.mqtt);
            }
        } else if (cmd === CMD.HW_ACK || bytes.length >= 41) {
            const info = Codec.parseHwAck(bytes);
            if (info) {
                log('sys', `解析 0x62: 协议${info.protoType}/${info.protoVer} `
                    + `硬件=${info.hwName} v${info.hwVer} 软件=${info.swName} v${info.swVer}`);
                el.infoProtoType.textContent = info.protoType;
                el.infoProtoVer.textContent = info.protoVer;
            }
        }
    }

    /* ----------------------------------------------------------------------
     * 查询硬件信息
     * -------------------------------------------------------------------- */
    el.btnQueryHw.addEventListener('click', async () => {
        try {
            const bytes = Codec.buildHwQuery();
            const path = document.querySelector('input[name=txPath]:checked').value;
            await send(path, bytes);
            log('sys', '已发送硬件信息查询，等待 0x62 应答…');
        } catch (e) {
            log('err', '发送失败: ' + e.message);
        }
    });

    /* ======================================================================
     * TinyML 控制
     * ==================================================================== */
    let mlRecvCount = 0;

    // ---- 启动周期推理 ----
    el.btnMlStart.addEventListener('click', async () => {
        try {
            await send('0xABF1', Codec.buildMlStart());
            log('sys', '已发送启动周期推理 (0xE1)');
        } catch (e) {
            log('err', '启动失败: ' + e.message);
        }
    });

    // ---- 停止周期推理 ----
    el.btnMlStop.addEventListener('click', async () => {
        try {
            await send('0xABF1', Codec.buildMlStop());
            log('sys', '已发送停止周期推理 (0xE0)');
        } catch (e) {
            log('err', '停止失败: ' + e.message);
        }
    });

    // ---- 单次推理 ----
    el.btnMlRun.addEventListener('click', async () => {
        try {
            const x = parseFloat(el.mlInput.value);
            if (isNaN(x)) throw new Error('输入无效');

            await send(el.mlPath.value, Codec.buildMlRun(x));
            log('sys', `已发送单次推理 (0xE2) x=${x}`);
        } catch (e) {
            log('err', '推理失败: ' + e.message);
        }
    });

    /* ----------------------------------------------------------------------
     * 处理扩展命令回包（从 0xABF2 收到）
     *   0xE4 长度 4 -> 服务器地址下发应答
     *   0xE0/0xE1 长度 2 -> 推理启停应答
     *   0xE2 长度 5 -> 单次推理结果
     *   0xE2 长度 9 -> 周期推送 (phase, pred)
     * 返回 true 表示已处理，不再交给通用协议解析。
     * -------------------------------------------------------------------- */
    function handleTinyMlResponse(bytes) {
        const cmd = bytes[0];

        // 云端服务器地址下发应答 (0xE4, 4 字节)
        if (cmd === CMD.SRV_ACK && bytes.length === 4) {
            const ack = Codec.parseSrvAck(bytes);
            if (ack) {
                const okTxt = ack.ok ? '成功' : '失败';
                log('sys', `服务器地址下发应答: ${okTxt} (端口 ${ack.port})`);
                el.srvAck.textContent =
                    `状态: ${okTxt}\n端口: ${ack.port}\n原始: ${Codec.toHex(bytes)}`;
                foot(`ESP32 服务器配置: ${okTxt}`);
                if (ack.ok) {
                    log('sys', 'ESP32 已保存配置到 NVS，稍后会自动连接服务器');
                }
            }
            return true;
        }

        // 启停应答
        if ((cmd === CMD.ML_START || cmd === CMD.ML_STOP) && bytes.length === 2) {
            const ok = bytes[1] === ML_ACK.OK;
            setMlRunning(cmd === CMD.ML_START && ok);
            log('sys', `${cmd === CMD.ML_START ? '启动' : '停止'}应答: ${ok ? '成功' : '失败'}`);
            foot(`TinyML 周期推理已${cmd === CMD.ML_START ? '启动' : '停止'}`);
            return true;
        }

        // 单次推理结果
        if (cmd === CMD.ML_RUN && bytes.length === 5) {
            const r = Codec.parseMlResult(bytes);
            if (r) {
                const truth = Math.sin(2 * Math.PI * parseFloat(el.mlInput.value));
                el.mlLast.textContent =
                    `y = ${r.value.toFixed(6)}\n(输入 x=${el.mlInput.value})`;
                log('sys', `单次推理结果 y=${r.value.toFixed(6)}`);
            }
            return true;
        }

        // 周期推送
        if (cmd === CMD.ML_RUN && bytes.length === 9) {
            const p = Codec.parseMlPush(bytes);
            if (p) {
                addMlRow(p.phase, p.pred);
            }
            return true;
        }

        return false;
    }

    // ---- 周期结果写入表格 ----
    function addMlRow(phase, pred) {
        mlRecvCount++;
        el.mlCount.textContent = mlRecvCount + ' 条';

        // 首次收到时清空占位行
        if (mlRecvCount === 1) el.mlTbody.innerHTML = '';

        const truth = Math.sin(2 * Math.PI * phase);
        const err = Math.abs(pred - truth);

        const tr = document.createElement('tr');
        tr.innerHTML =
            `<td>${mlRecvCount}</td>` +
            `<td>${phase.toFixed(4)}</td>` +
            `<td>${pred >= 0 ? '+' : ''}${pred.toFixed(4)}</td>` +
            `<td>${truth >= 0 ? '+' : ''}${truth.toFixed(4)}</td>` +
            `<td class="col-err${err > 0.01 ? ' err-warn' : ''}">${err.toFixed(4)}</td>`;

        el.mlTbody.insertBefore(tr, el.mlTbody.firstChild);   // 新数据在上

        // 只保留最近 100 行
        while (el.mlTbody.childElementCount > 100) {
            el.mlTbody.removeChild(el.mlTbody.lastChild);
        }
    }

    // ---- 更新运行状态显示 ----
    function setMlRunning(on) {
        if (!el.mlRunState) return;
        el.mlRunState.textContent = on ? '运行中' : '已停止';
        el.mlRunState.className = 'sub-state ' + (on ? 'on' : 'off');
    }

    /* ======================================================================
     * 云端服务器地址下发（0xE4）
     * ==================================================================== */
    el.btnUseLocalIp.addEventListener('click', () => {
        // 浏览器无法直接取本机局域网 IP（隐私限制），这里给出获取指引
        log('sys', '浏览器出于隐私限制无法读取本机内网 IP，请手动填写');
        log('sys', '在命令行执行 ipconfig，找「IPv4 地址」（如 192.168.1.100 或 10.0.103.xxx）');
        foot('请用 ipconfig 查看本机 IPv4 地址后填入');
    });

    el.btnSendSrv.addEventListener('click', async () => {
        try {
            const host = el.srvHost.value.trim();
            const port = el.srvPort.value.trim();
            const bytes = Codec.buildSrvSet(host, port);

            await send('0xABF1', bytes);
            log('sys', `服务器地址已下发: ${host}:${port} (${bytes.length} 字节)`);

            el.srvAck.textContent = '已发送，等待应答…';
            setWifiHintCloud(host, port);
        } catch (e) {
            log('err', '下发失败: ' + e.message);
            el.srvAck.textContent = '失败: ' + e.message;
        }
    });

    function setWifiHintCloud(host, port) {
        foot(`已在 ESP32 上配置云端服务器 ${host}:${port}，请确认服务端已启动`);
    }

    /* ----------------------------------------------------------------------
     * 标签页切换
     * -------------------------------------------------------------------- */
    document.querySelectorAll('.tab').forEach(btn => {
        btn.addEventListener('click', () => {
            const target = btn.dataset.tab;

            document.querySelectorAll('.tab').forEach(b => b.classList.toggle('active', b === btn));
            document.querySelectorAll('.tabpanel').forEach(p => {
                p.classList.toggle('active', p.id === target);
            });
        });
    });

    /* ----------------------------------------------------------------------
     * 控制台工具
     * -------------------------------------------------------------------- */
    el.btnClearLog.addEventListener('click', () => {
        el.console.innerHTML = '<p class="hint">已清空</p>';
        logCount = 0;
    });

    /* ----------------------------------------------------------------------
     * 初始化
     * -------------------------------------------------------------------- */
    function init() {
        // 初始状态：BLE 未连接，WiFi 未知
        setBleState('off', '未连接');
        resetWifiState();

        if (!ble.supported) {
            log('err', '当前浏览器不支持 Web Bluetooth');
            log('sys', '请使用 Chrome / Edge / Opera，并通过 http://localhost 或 https:// 访问');
            el.btnConnect.disabled = true;
            setBleState('err', '浏览器不支持');
            foot('浏览器不支持 Web Bluetooth —— 请换 Chrome / Edge，或改用 localhost 访问');
            return;
        }

        const isSecure = location.protocol === 'https:'
            || location.hostname === 'localhost'
            || location.hostname === '127.0.0.1';

        if (!isSecure) {
            log('err', '页面不在安全上下文中，Web Bluetooth 会被浏览器禁用');
            log('sys', '当前地址: ' + location.href);
            log('sys', '请改用 http://localhost:8000 访问（见 README 的启动方式）');
            el.btnConnect.disabled = true;
            setBleState('err', '需 HTTPS');
            foot('需通过 localhost 或 HTTPS 访问');
            return;
        }

        log('sys', 'ESP32 调试平台已就绪');
        log('sys', '浏览器支持 Web Bluetooth ✓');
        refreshWifiPreview();

        // 若本会话此前连接过，说明是刷新导致连接丢失（Web Bluetooth 无法自动重连）
        try {
            const prev = sessionStorage.getItem('esp32_was_connected');
            if (prev) {
                sessionStorage.removeItem('esp32_was_connected');
                log('sys', '上次连接已因页面刷新而断开，请重新点击「连接设备」');
                foot('页面刷新会断开 BLE 连接，请重新连接');
                setBleState('off', '需重连');
            }
        } catch (e) { /* sessionStorage 不可用则忽略 */ }
    }

    // 脚本加载在 body 末尾，DOM 已就绪；若尚未就绪则等 DOMContentLoaded
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
