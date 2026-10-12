#!/usr/bin/env node
/*
 * bench-mqtt-fwd.js —— 台面用**用户态 TCP 转发**：把本机 bench 栈的设备面 mTLS
 * 端口从 WSL 的 loopback 抬到 Windows 的 WLAN 地址上，让局域网里的板子够得到。
 *
 * 为什么必须有这一跳（本机实测，不是设计偏好）：
 *   · EMQX 的 ssl:deviceface 监听器在 WSL Docker 内：容器 8885 = 宿主 18883；
 *   · WSL 的端口中继在 Windows 侧**只绑 loopback**
 *     （Get-NetTCPConnection：18883 的 LocalAddress = 127.0.0.1 / ::1）
 *     ⇒ 板子（192.168.110.x）连 192.168.110.208:18883 是**连不上**的；
 *   · 本次会话无管理员权限 ⇒ 不能新增入站防火墙规则、不能改 wslrelay 绑定。
 *   ⇒ 用已在入站放行名单里的 node.exe 跑用户态转发（⛔ 不改防火墙、不提权）。
 *
 * 用法（Windows 侧；前台运行，Ctrl+C 结束）：
 *   node embedded/esp-adf/examples/oneye/korvo2_oneye/tools/bench-mqtt-fwd.js
 *   node .../bench-mqtt-fwd.js --bind 0.0.0.0 --port 18830 --target 127.0.0.1:18883
 *
 * 链路：板子 -> 192.168.110.208:18830 -> 127.0.0.1:18883 -> WSL oneye-emqx:8885
 *
 * ⛔ 它不是安全边界：本进程只是字节搬运，TLS 在两端各自终止（设备 <-> EMQX）。
 * ⛔ 本脚本不烧写、不 commit、不改任何配置；只开一个本地监听。
 */

'use strict';

const net = require('net');

function parseArgs(argv) {
  const out = { bind: '0.0.0.0', port: 18830, target: '127.0.0.1:18883' };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--bind') out.bind = argv[++i];
    else if (a === '--port') out.port = Number(argv[++i]);
    else if (a === '--target') out.target = argv[++i];
    else if (a === '-h' || a === '--help') out.help = true;
    else throw new Error('未知参数: ' + a);
  }
  const m = /^([^:]+):(\d+)$/.exec(out.target);
  if (!m) throw new Error('--target 需要 host:port 形式，收到 ' + out.target);
  out.targetHost = m[1];
  out.targetPort = Number(m[2]);
  if (!Number.isInteger(out.port) || out.port <= 0 || out.port > 65535) {
    throw new Error('--port 非法: ' + out.port);
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
if (args.help) {
  console.log('用法: node bench-mqtt-fwd.js [--bind 0.0.0.0] [--port 18830] [--target 127.0.0.1:18883]');
  process.exit(0);
}

let seq = 0;

const server = net.createServer((client) => {
  const id = ++seq;
  const peer = client.remoteAddress + ':' + client.remotePort;
  const upstream = net.connect({ host: args.targetHost, port: args.targetPort });
  let closed = false;

  const shutdown = (why) => {
    if (closed) return;
    closed = true;
    console.log('[fwd#' + id + '] close (' + why + ') peer=' + peer);
    client.destroy();
    upstream.destroy();
  };

  client.on('error', (e) => shutdown('client error ' + e.code));
  upstream.on('error', (e) => shutdown('upstream error ' + e.code));
  client.on('close', () => shutdown('client close'));
  upstream.on('close', () => shutdown('upstream close'));

  upstream.on('connect', () => {
    console.log('[fwd#' + id + '] open peer=' + peer + ' -> ' + args.target);
    client.pipe(upstream);
    upstream.pipe(client);
  });

  /* ⛔ 不设超时：MQTT 长连接会静默很久（心跳 5 min） */
  client.setNoDelay(true);
  upstream.setNoDelay(true);
});

server.on('error', (e) => {
  console.error('[fwd] 监听失败: ' + e.code + ' ' + e.message);
  process.exit(2);
});

server.listen(args.port, args.bind, () => {
  console.log('[fwd] listening ' + args.bind + ':' + args.port + ' -> ' + args.target);
  console.log('[fwd] 板子侧端点 = CONFIG_ONEYE_FW_CLOUD_HOST:CONFIG_ONEYE_FW_CLOUD_PORT');
  console.log('[fwd] ⛔ 前台常驻；本进程退出 = 板子断连（MQTT 会自动重连，但期间不在线）');
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    console.log('[fwd] ' + sig + ' -> 退出');
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 500).unref();
  });
}
