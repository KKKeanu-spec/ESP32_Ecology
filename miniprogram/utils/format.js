'use strict';

/**
 * 通用格式化工具（无业务逻辑）
 */

function pad2(n) {
  return n < 10 ? '0' + n : '' + n;
}

/** HH:MM:SS.mmm */
function time(ts) {
  const d = ts ? new Date(ts) : new Date();
  return (
    pad2(d.getHours()) +
    ':' +
    pad2(d.getMinutes()) +
    ':' +
    pad2(d.getSeconds()) +
    '.' +
    ('00' + d.getMilliseconds()).slice(-3)
  );
}

/** Uint8Array -> "59 02 01 07 08" */
function hex(bytes) {
  const arr = [];
  for (let i = 0; i < bytes.length; i++) {
    arr.push(('0' + bytes[i].toString(16).toUpperCase()).slice(-2));
  }
  return arr.join(' ');
}

/** Uint8Array -> 可打印 ASCII，不可打印用 . 占位 */
function ascii(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i];
    s += b >= 32 && b < 127 ? String.fromCharCode(b) : '.';
  }
  return s;
}

/** Uint8Array -> 带偏移的 hexdump 多行文本 */
function hexdump(bytes, perLine) {
  const n = perLine || 16;
  const lines = [];
  for (let i = 0; i < bytes.length; i += n) {
    const part = bytes.slice(i, i + n);
    let h = hex(part);
    while (h.length < n * 3 - 1) h += ' ';
    lines.push(
      ('000' + i.toString(16).toUpperCase()).slice(-4) + '  ' + h + '  ' + ascii(part)
    );
  }
  return lines.join('\n');
}

/** 容错解析 HEX 字符串 -> Uint8Array */
function parseHex(str) {
  const clean = String(str || '')
    .replace(/0x/gi, '')
    .replace(/[^0-9a-fA-F]/g, '');
  if (!clean.length) throw new Error('HEX 内容为空');
  if (clean.length % 2 !== 0) throw new Error('HEX 长度必须为偶数');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.substr(i * 2, 2), 16);
  }
  return out;
}

/** 毫秒 -> 32.1s / 1m02s */
function duration(ms) {
  const s = Math.floor(ms / 1000);
  if (s < 60) return s + 's';
  return Math.floor(s / 60) + 'm' + pad2(s % 60) + 's';
}

function sleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms);
  });
}

module.exports = {
  time: time,
  hex: hex,
  ascii: ascii,
  hexdump: hexdump,
  parseHex: parseHex,
  duration: duration,
  sleep: sleep,
};
