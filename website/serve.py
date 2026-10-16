# -*- coding: utf-8 -*-
"""
ESP32 调试平台 —— 本地静态服务器

Web Bluetooth 要求页面运行在「安全上下文」中。
http://localhost 被浏览器视为安全上下文，因此用本脚本起服务即可，
无需 HTTPS 证书。

用法:
    python serve.py                 # 默认端口 8000，自动开浏览器
    python serve.py 9000            # 指定端口
    python serve.py --no-browser    # 不自动打开浏览器

然后浏览器打开:  http://localhost:8000
（务必用 Chrome / Edge / Opera；iOS 不支持 Web Bluetooth；
  且 PC 需具备蓝牙适配器）
"""

import os
import sys
import http.server
import socketserver
import threading

DEFAULT_PORT = 8000

# 自动打开浏览器（可用 --no-browser 关闭，或在无 GUI 环境下自动跳过）
OPEN_BROWSER = '--no-browser' not in sys.argv


class Handler(http.server.SimpleHTTPRequestHandler):
    """禁用缓存，方便改完代码刷新即生效。"""

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

    def log_message(self, fmt, *args):
        # 简化日志格式
        sys.stderr.write("  %s\n" % (fmt % args))


def find_free_port(start):
    """从 start 开始找一个可用端口。"""
    import socket
    for port in range(start, start + 50):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(('127.0.0.1', port)) != 0:
                return port
    raise RuntimeError("找不到可用端口")


def _open_browser(url):
    """尽力打开浏览器，失败不影响服务器运行。"""
    try:
        import webbrowser
        webbrowser.open(url)
    except Exception as e:
        print("  (未能自动打开浏览器: %s)" % e)


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    # 取第一个纯数字参数作为端口
    port = DEFAULT_PORT
    for a in sys.argv[1:]:
        if a.isdigit():
            port = int(a)
            break
    port = find_free_port(port)

    url = "http://localhost:%d" % port

    print("=" * 62)
    print("  ESP32 调试平台 - 本地服务器")
    print("=" * 62)
    print("  目录: %s" % os.getcwd())
    print("  地址: %s" % url)
    print()
    print("  请用 Chrome / Edge / Opera 打开上面的地址")
    print("  按 Ctrl+C 停止服务")
    print("=" * 62)

    if OPEN_BROWSER:
        threading.Timer(0.8, _open_browser, args=(url,)).start()

    with socketserver.TCPServer(("127.0.0.1", port), Handler) as httpd:
        httpd.allow_reuse_address = True
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n  服务已停止")


if __name__ == "__main__":
    main()
