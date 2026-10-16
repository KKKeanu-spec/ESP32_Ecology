# -*- coding: utf-8 -*-
"""
ESP32 云端测试服务器（TCP + JSON 行协议）

作用:
    1) 监听 TCP 端口，接受 ESP32 主动连入
    2) 接收并格式化显示 ESP32 上报的数据（心跳 / 事件 / TinyML 结果）
    3) 支持在命令行手动下发控制指令

用法:
    python esp32_server.py                # 默认监听 0.0.0.0:9000
    python esp32_server.py 9000           # 指定端口

    启动后，先在网页(BLE)里下发服务器地址:  <本机IP>:9000
    ESP32 会自动连上来，随后可在此终端看到数据。

    命令行指令（回车发送）:
        status              查询设备状态
        start / stop        启停周期推理
        run 0.25            单次推理(输入相位 0~1)
        report on / off     开关 TinyML 结果上报
        list                列出已连接设备
        help                帮助
        quit                退出
"""

import sys
import os
import json
import socket
import threading
import time
import datetime

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

DEFAULT_PORT = 9000
BUFSIZE = 4096

# 已连接的客户端: {addr: socket}
_clients = {}
_clients_lock = threading.Lock()


# ============================================================================
# 显示工具
# ============================================================================
def ts():
    return datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]


def log(tag, msg, color=None):
    colors = {'recv': '\033[36m', 'send': '\033[33m', 'sys': '\033[32m', 'err': '\033[31m'}
    c = colors.get(color or tag, '')
    reset = '\033[0m' if c else ''
    print(f"{c}[{ts()}] {tag.upper():5} {msg}{reset}", flush=True)


# ============================================================================
# 上报数据格式化
# ============================================================================
def format_uplink(obj):
    """把 ESP32 上报的 JSON 格式化为人可读的一行"""
    t = obj.get('t', '?')

    if t == 'hb':
        up = obj.get('up', 0)
        h, rem = divmod(up, 3600)
        m, s = divmod(rem, 60)
        return (f"♥ 心跳  在线 {h:02d}:{m:02d}:{s:02d}  "
                f"内存 {obj.get('heap',0)/1024:.1f}KB  "
                f"推理{'运行' if obj.get('ml_run') else '停止'}  "
                f"服务器 {obj.get('srv','?')}")

    if t == 'evt':
        names = {
            'net_up': '云端连接建立', 'wifi_up': 'WiFi 已连接',
            'wifi_down': 'WiFi 已断开', 'ml_start': '推理已启动', 'ml_stop': '推理已停止',
        }
        e = obj.get('e', '?')
        return f"⚡ 事件  {names.get(e, e)}"

    if t == 'ml':
        return (f"◆ 推理  #{obj.get('n',0):<5} "
                f"x={obj.get('x',0):+.4f}  "
                f"pred={obj.get('pred',0):+.4f}  "
                f"err={obj.get('err',0):.4f}")

    if t == 'ml_once':
        return f"◆ 单次  x={obj.get('x',0):+.4f}  pred={obj.get('pred',0):+.4f}"

    if t == 'status':
        return (f"ℹ 状态  推理{'运行' if obj.get('ml_run') else '停止'}  "
                f"上报{'开' if obj.get('ml_report') else '关'}  "
                f"内存 {obj.get('heap',0)/1024:.1f}KB  "
                f"在线 {obj.get('up',0)}s  服务器 {obj.get('srv','?')}")

    if t == 'ack':
        return f"✔ 应答  {obj.get('c','?')}  ok={obj.get('ok')}  {obj.get('msg','')}"

    return f"? 未知类型  {obj}"


# ============================================================================
# 客户端处理
# ============================================================================
def broadcast(line):
    """向所有已连接客户端发送一行"""
    data = (line + '\n').encode('utf-8')
    with _clients_lock:
        dead = []
        for addr, sock in _clients.items():
            try:
                sock.sendall(data)
                log('send', f"{addr}  {line}")
            except Exception as e:
                log('err', f"{addr} 发送失败: {e}")
                dead.append(addr)
        for addr in dead:
            _clients.pop(addr, None)


def handle_client(sock, addr):
    log('sys', f"ESP32 已连接: {addr}")

    with _clients_lock:
        _clients[addr] = sock

    buf = b''
    try:
        while True:
            chunk = sock.recv(BUFSIZE)
            if not chunk:
                break
            buf += chunk

            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                line = raw.decode('utf-8', errors='replace').strip()
                if not line:
                    continue

                try:
                    obj = json.loads(line)
                    log('recv', f"{addr}  {format_uplink(obj)}")
                except json.JSONDecodeError:
                    log('recv', f"{addr}  (非法JSON) {line}")

    except Exception as e:
        log('err', f"{addr} 接收异常: {e}")
    finally:
        with _clients_lock:
            _clients.pop(addr, None)
        try:
            sock.close()
        except Exception:
            pass
        log('sys', f"ESP32 已断开: {addr}")


# ============================================================================
# 命令行交互
# ============================================================================
HELP = """
可用指令:
  status              查询设备状态
  start / stop        启动 / 停止周期推理
  run <x>             单次推理，x 为 0~1 的相位 (如: run 0.25)
  report on|off       开关 TinyML 结果上报
  list                列出已连接设备
  help                显示帮助
  quit                退出
"""


def cmd_loop():
    print(HELP, flush=True)
    while True:
        try:
            line = input('> ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            os._exit(0)

        if not line:
            continue

        parts = line.split()
        cmd = parts[0].lower()

        if cmd == 'quit' or cmd == 'exit':
            os._exit(0)

        elif cmd == 'help':
            print(HELP, flush=True)

        elif cmd == 'list':
            with _clients_lock:
                if not _clients:
                    print("  (无设备连接)", flush=True)
                for a in _clients:
                    print(f"  {a}", flush=True)

        elif cmd == 'status':
            broadcast(json.dumps({"c": "query_status"}, separators=(',', ':')))

        elif cmd == 'start':
            broadcast(json.dumps({"c": "ml_start"}, separators=(',', ':')))

        elif cmd == 'stop':
            broadcast(json.dumps({"c": "ml_stop"}, separators=(',', ':')))

        elif cmd == 'run':
            try:
                x = float(parts[1]) if len(parts) > 1 else 0.25
            except ValueError:
                print("  用法: run <0~1 之间的数>", flush=True)
                continue
            broadcast(json.dumps({"c": "ml_run", "x": x}, separators=(',', ':')))

        elif cmd == 'report':
            if len(parts) < 2 or parts[1].lower() not in ('on', 'off'):
                print("  用法: report on | report off", flush=True)
                continue
            on = 1 if parts[1].lower() == 'on' else 0
            broadcast(json.dumps({"c": "ml_report", "on": on}, separators=(',', ':')))

        else:
            print("  未知指令，输入 help 查看帮助", flush=True)


# ============================================================================
# 主程序
# ============================================================================
def local_ips():
    """列出本机所有 IPv4 地址（用于提示在网页里下发哪个地址）"""
    ips = []
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith('127.'):
                ips.append(ip)
    except Exception:
        pass
    if not ips:
        ips = ['<本机IP>']
    return ips


def main():
    port = DEFAULT_PORT
    for a in sys.argv[1:]:
        if a.isdigit():
            port = int(a)
            break

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', port))
    srv.listen(4)

    print("=" * 66)
    print("  ESP32 云端测试服务器")
    print("=" * 66)
    print(f"  监听端口 : {port}")
    print(f"  本机地址 : {', '.join(local_ips())}")
    print()
    print("  使用步骤:")
    print("    1) 网页用 BLE 连接 ESP32")
    print(f"    2) 在网页「云端服务器」里填写地址:  {local_ips()[0]}:{port}")
    print("    3) ESP32 会自动连上来，此终端开始显示数据")
    print()
    print("  输入 help 查看可用指令，Ctrl+C 停止服务")
    print("=" * 66)
    print()

    threading.Thread(target=cmd_loop, daemon=True).start()

    try:
        while True:
            sock, addr = srv.accept()
            threading.Thread(target=handle_client, args=(sock, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\n  服务已停止")
    finally:
        srv.close()


if __name__ == '__main__':
    main()
