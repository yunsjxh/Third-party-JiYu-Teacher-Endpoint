#!/usr/bin/env python3
"""极域V6.0教师端 - 逐字节匹配真实抓包，带调试日志。

运行后会弹出两个窗口：
- 主窗口：显示命令提示符 teacher>，用于输入操作命令。
- 日志窗口：PowerShell 实时 tail teacher_sim.log。
"""
import socket, struct, threading, time, random, sys, os, uuid, colorsys
import logging, logging.handlers
from PIL import Image, ImageOps
import io
import subprocess

LOG_DIR = os.path.join(os.path.expanduser('~'), 'Desktop')
LOG_PATH = os.path.join(LOG_DIR, 'teacher_sim.log')


def setup_logging():
    logger = logging.getLogger()
    logger.setLevel(logging.DEBUG)
    if logger.handlers:
        logger.handlers.clear()

    fh = logging.handlers.RotatingFileHandler(
        LOG_PATH, maxBytes=10*1024*1024, backupCount=3, encoding='utf-8'
    )
    fh.setLevel(logging.DEBUG)
    fmt = logging.Formatter(
        '%(asctime)s.%(msecs)03d [%(levelname)-8s] [%(threadName)-12s] %(filename)s:%(lineno)d - %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    fh.setFormatter(fmt)
    logger.addHandler(fh)

    # 日志由独立窗口实时显示，主窗口只保留命令交互，所以不再输出到 stdout。
    # 如需在控制台也看日志，可取消下面注释。
    # ch = logging.StreamHandler(sys.stdout)
    # ch.setLevel(logging.INFO)
    # ch.setFormatter(logging.Formatter('%(asctime)s [%(levelname)s] %(message)s'))
    # logger.addHandler(ch)

    logger.info('日志系统初始化完成，log 文件：%s', LOG_PATH)
    return logger


def spawn_log_window():
    """在独立的命令行窗口中实时跟踪日志文件。"""
    # 确保日志文件已存在，避免 PowerShell Get-Content 报错。
    if not os.path.exists(LOG_PATH):
        open(LOG_PATH, 'a', encoding='utf-8').close()

    # 强制 PowerShell 控制台使用 UTF-8，避免中文日志乱码。
    ps_cmd = (
        f'chcp 65001 | Out-Null; '
        f'[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; '
        f'Get-Content -Path "{LOG_PATH}" -Wait -Tail 50 -Encoding UTF8'
    )
    try:
        subprocess.Popen(
            ['start', 'powershell', '-NoExit', '-Command', ps_cmd],
            shell=True,
            creationflags=subprocess.CREATE_NEW_CONSOLE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        print('[启动] 已打开实时日志窗口（PowerShell 跟踪 log，UTF-8）')
    except Exception as e:
        print(f'[启动] 打开日志窗口失败：{e}')


logger = setup_logging()

MCAST, PORT = '224.50.50.42', 4705
SMCAST, SPORT = '225.2.2.1', 5512
TGUID = uuid.UUID('{F96A6D19-5B29-46B9-AB95-8A143ECDDC26}')

oonc_seq = 0

MAGIC_NAMES = {
    0x434E4F4F: 'OONC',
    0x434E4143: 'CANC',
    0x41434157: 'WACA',
    0x53524E54: 'TNRS',
    0x434F4D44: 'DMOC',
    0x544E504C: 'LPNT',
    0x4143414B: 'KACA',
    0x434D5254: 'TRMC',
    0x544E5254: 'TRNT',
    0x544E4544: 'DENT',
    0x544E414C: 'LANT',
    0x4F4E4E41: 'ANNO',
    0x49474F4C: 'LOGI',
    0x5353454D: 'MESS',
}

# 日常广播类魔数，不必逐条记录 hexdump
ROUTINE_MAGICS = {0x434E4F4F, 0x434E4143, 0x4F4E4E41}

students = {}
previews = {}   # sip -> {'total':int, 'buf':bytearray, 'got':int}
running = True


def hexdump(data, width=16):
    if data is None:
        return '<None>'
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hex_part = ' '.join(f'{b:02x}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'{i:04x}  {hex_part:<{width*3}} {ascii_part}')
    return '\n'.join(lines)


def magic_name(mag):
    return MAGIC_NAMES.get(mag, f'UNKNOWN(0x{mag:08X})')


def is_interesting(d, sip):
    """判断是否值得记录的数据包：非本机、非日常广播、或未知类型。"""
    if sip == ip:
        return False
    if len(d) < 4:
        return True
    mag = struct.unpack('<I', d[:4])[0]
    return mag not in ROUTINE_MAGICS


def get_ip():
    s = socket.socket(2, 2)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        logger.info('本机 IP 获取成功：%s', ip)
        return ip
    except Exception as e:
        logger.error('获取本机 IP 失败：%s', e, exc_info=True)
        raise
    finally:
        s.close()


ip = get_ip()
sock = socket.socket(2, 2)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('', PORT))
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                struct.pack('4s4s', socket.inet_aton(MCAST), socket.inet_aton(ip)))
sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
logger.info('主 socket 就绪：%s:%d', MCAST, PORT)

sock2 = socket.socket(2, 2)
sock2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock2.bind(('', SPORT))
sock2.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack('4s4s', socket.inet_aton(SMCAST), socket.inet_aton(ip)))
logger.info('会话 socket 就绪：%s:%d', SMCAST, SPORT)

logger.info('Teacher %s started on %s:%d + %s:%d', ip, MCAST, PORT, SMCAST, SPORT)


def oonc():
    global oonc_seq
    pkt = (struct.pack('<II', 0x434E4F4F, 0x10000)
           + struct.pack('<I', 16)
           + TGUID.bytes_le
           + socket.inet_aton(ip)
           + struct.pack('<III', 1, 1, oonc_seq))
    oonc_seq += 1
    return pkt


def canc():
    n = '1\x00'
    nw = n.encode('utf-16-le')
    nc = len(nw) // 2
    af = (nc << 16) | 1
    p = struct.pack('<II', 0x434E4143, 0x10000) + struct.pack('<I', 84) + TGUID.bytes_le
    p += struct.pack('<I', af) + socket.inet_aton(ip) + b'\x01\x00\x00\x00\x01\x00\x00\x00' + nw
    p += b'\x00' * (84 - len(nw) - 8)
    return p


def waca(sip):
    return struct.pack('<II', 0x41434157, 0x10000) + struct.pack('<I', 8) + TGUID.bytes_le + socket.inet_aton(ip) + b'\x01\x00\x00\x00'


def request_preview(sip):
    """主动请求学生端发送预览缩略图 (TNRS)。"""
    tg = bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
    pkt = struct.pack('<II', 0x53524E54, 0x10000) + struct.pack('<I', 16) + tg
    pkt += struct.pack('<IIII', 0x48, 1, 0, 0x100)
    logger.info('[Preview] TNRS -> %s, len=%d', sip, len(pkt))
    try:
        sock.sendto(pkt, (sip, PORT))
    except Exception as e:
        logger.error('[Preview] TNRS 发送给 %s 失败：%s', sip, e, exc_info=True)


def send_chat(sip, text):
    """向已登录学生发送聊天消息（MESS，payload type=0x800）。"""
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    try:
        text_utf16 = text.encode('utf-16-le') + b'\x00\x00'
        payload = (struct.pack('<I', 16 + len(text_utf16))
                   + struct.pack('<I', 0x800)
                   + struct.pack('<I', 0)
                   + struct.pack('<I', 1)
                   + text_utf16)
        mess = (struct.pack('<II', 0x5353454D, 1)
                + struct.pack('<I', 1)
                + socket.inet_aton(sip)
                + payload)
        sock2.sendto(mess, (sip, SPORT))
        logger.info('[命令] 发送聊天消息 -> %s: %s', sip, text)
        print(f'[命令] 聊天消息已发送给 {sip}')
    except Exception as e:
        logger.error('[命令] 发送聊天消息失败：%s', e, exc_info=True)
        print(f'[命令] 发送失败：{e}')


def build_dmoc():
    """构造 DMOC 包（教师端控制信息）。"""
    cg = bytes.fromhex('ce90fd383df5844c857fa35183c051f3')
    dd = b'\x20\x4e\x00\x00' + socket.inet_aton(ip) + b'\x35\x00\x00\x00\x35\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x80'
    dd += bytes.fromhex('e10202331e16e102023421160000a046000020419a99993fa0052000')
    dd += b'\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3d\x00'
    return struct.pack('<II', 0x434F4D44, 0x10000) + struct.pack('<I', len(dd)) + cg + dd


def build_lpnt_subtype3():
    """构造 LPNT subtype=3 包。"""
    lg = bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
    return struct.pack('<II', 0x544E504C, 0x10000) + struct.pack('<I', 20) + lg + b'\x03\x00\x00\x00\x01\x00\x00\x00\x50\x00\x00\x00\x3c\x00\x00\x00\x05\x00\x00\x00'


def keep_alive_preview(sip):
    """学生登录后周期性发送 LPNT subtype=3 + DMOC，直到收到预览或学生下线。"""
    lp = build_lpnt_subtype3()
    dm = build_dmoc()
    logger.info('[KeepAlive] 启动 %s', sip)
    while running and sip in students:
        if sip in previews:
            logger.info('[KeepAlive] %s previews 已存在，停止', sip)
            break
        try:
            sock.sendto(lp, (sip, PORT))
        except Exception as e:
            logger.error('[KeepAlive] LPNT -> %s 失败：%s', sip, e, exc_info=True)
        time.sleep(0.05)
        try:
            sock.sendto(dm, (sip, PORT))
        except Exception as e:
            logger.error('[KeepAlive] DMOC -> %s 失败：%s', sip, e, exc_info=True)
        time.sleep(0.5)
    logger.info('[KeepAlive] %s 退出', sip)


def handle_tnal(d, sip):
    """接收 LANT 预览缩略图片段并拼成 JPEG。"""
    logger.debug('[TNAL] RECV from %s, len=%d\n%s', sip, len(d), hexdump(d[:256]))

    if len(d) < 48:
        logger.warning('[TNAL] 包太短：%d 字节 from %s', len(d), sip)
        return

    try:
        total = struct.unpack('<I', d[36:40])[0]
        offset = struct.unpack('<I', d[40:44])[0]
        frag_len = struct.unpack('<I', d[44:48])[0]
    except struct.error as e:
        logger.error('[TNAL] 解包失败 from %s：%s', sip, e, exc_info=True)
        return

    frag = d[48:48+frag_len]
    logger.info('[TNAL] %s total=%d offset=%d frag_len=%d', sip, total, offset, frag_len)

    if not frag or total == 0:
        logger.warning('[TNAL] 空片段或 total==0 from %s', sip)
        return

    state = previews.get(sip)
    if state is None or state['total'] != total or offset == 0:
        logger.info('[TNAL] %s 新建/重置 previews，total=%d', sip, total)
        state = {'total': total, 'buf': bytearray(total), 'got': 0}
        previews[sip] = state

    end = min(offset + len(frag), total)
    written = end - offset
    if written <= 0:
        logger.warning('[TNAL] 非法写入 offset=%d end=%d from %s', offset, end, sip)
        return

    state['buf'][offset:end] = frag[:written]
    state['got'] += written
    logger.info('[TNAL] %s 进度 %d/%d (+%d)', sip, state['got'], total, written)

    if state['got'] >= total:
        idx = 0
        while True:
            fn = os.path.join(LOG_DIR, f'preview_{sip.replace(".", "_")}_{idx}.jpg')
            if not os.path.exists(fn):
                break
            idx += 1
        try:
            with open(fn, 'wb') as f:
                f.write(state['buf'])
            logger.info('[Preview] 已保存原图 %s (%d bytes)', fn, total)
        except Exception as e:
            logger.error('[Preview] 保存原图 %s 失败：%s', fn, e, exc_info=True)
            return

        # 极域学生端 JPEG 是 bottom-up DIB + 仅色度反相（Cb/Cr 反相，Y 不变）。
        try:
            fixed_fn = fn.replace('.jpg', '_fixed.jpg')
            img = Image.open(io.BytesIO(state['buf']))
            if img.mode != 'RGB':
                img = img.convert('RGB')
            img = img.transpose(Image.FLIP_TOP_BOTTOM)
            ycbcr = img.convert('YCbCr')
            y, cb, cr = ycbcr.split()
            cb = ImageOps.invert(cb)
            cr = ImageOps.invert(cr)
            img = Image.merge('YCbCr', (y, cb, cr)).convert('RGB')
            img.save(fixed_fn, 'JPEG', quality=95)
            logger.info('[Preview] 已保存修复图 %s', fixed_fn)
        except Exception as e:
            logger.error('[Preview] 修复图保存失败：%s', e, exc_info=True)

        logger.info('[Preview] %s 接收完成', sip)
        del previews[sip]


def handle_mess(d, sip, sp, via='unknown'):
    """解析学生端发来的 MESS 消息包（聊天/提示类）。"""
    if len(d) < 12:
        logger.warning('[MESS] 包过短（%d 字节）from %s:%d via %s', len(d), sip, sp, via)
        return

    try:
        magic, sender_id, rcpt_count = struct.unpack('<III', d[:12])
        header_len = 12 + 4 * rcpt_count
        if len(d) < header_len:
            logger.warning('[MESS] 头长度不足 from %s:%d (rcpt_count=%d)', sip, sp, rcpt_count)
            return
        payload = d[header_len:]
    except struct.error as e:
        logger.error('[MESS] 解包失败 from %s:%d：%s', sip, sp, e)
        return

    logger.info('[MESS] RECV from %s:%d via %s, sender_id=0x%08X, rcpt_count=%d, payload_len=%d',
                sip, sp, via, sender_id, rcpt_count, len(payload))
    logger.debug('[MESS] payload hex\n%s', hexdump(payload[:256]))

    text = None
    msg_kind = 'unknown'
    msg_type = struct.unpack('<I', payload[4:8])[0] if len(payload) >= 8 else 0

    # IDA 中聊天消息负载结构：
    # [0..3]=总长, [4..7]=0x800, [8..11]=0, [12..15]=1, [16..]=UTF-16-LE 字符串
    if len(payload) >= 16 and msg_type == 0x800:
        try:
            raw = payload[16:].rstrip(b'\x00')
            if len(raw) % 2:
                raw = raw[:-1]
            text = raw.decode('utf-16-le')
            msg_kind = 'chat'
        except Exception as e:
            logger.debug('[MESS] 聊天消息 UTF-16-LE 解码失败：%s', e)

    # 状态/窗口标题等（payload[4:8]==0，payload[8:12]==0x03000000）
    # IDA 中结构：
    # [0..3]=总长, [4..7]=0, [8..11]=3, [12..15]=子类型,
    # [16..19]=字符串最大长度, [20..23]=PID/计数/额外数据, [24..]=UTF-16-LE 字符串
    elif len(payload) >= 24 and msg_type == 0:
        subtype = struct.unpack('<I', payload[12:16])[0]
        extra = struct.unpack('<I', payload[20:24])[0]
        if subtype == 6:
            try:
                raw = payload[24:].rstrip(b'\x00')
                if len(raw) % 2:
                    raw = raw[:-1]
                title = raw.decode('utf-16-le')
                text = f'[窗口标题] {title}'
                msg_kind = 'status'
            except Exception as e:
                logger.debug('[MESS] 窗口标题解码失败：%s', e)
        elif subtype == 7:
            # sub_43B080：调用 Wlanapi 获取可用 WiFi 网络数量，-1 表示检测失败
            count = extra if extra != 0xFFFFFFFF else -1
            text = f'[WiFi可用网络数量] {count}'
            msg_kind = 'status'
        elif subtype == 1:
            text = f'[IE/浏览器URL信息] extra=0x{extra:08X}'
            msg_kind = 'status'
        elif subtype == 0:
            text = f'[窗口标题清空/PID=0x{extra:08X}]'
            msg_kind = 'status'
        elif subtype == 3:
            text = '[系统性能/进程信息]'
            msg_kind = 'status'
        else:
            logger.debug('[MESS] 未知状态子类型 %d', subtype)

    if text:
        if msg_kind == 'chat':
            logger.info('[MESS] 来自 %s:%d 的聊天消息：%s', sip, sp, text)
        else:
            logger.info('[MESS] 来自 %s:%d 的状态消息：%s', sip, sp, text)
    else:
        logger.info('[MESS] 来自 %s:%d 的消息无法解析文本，payload_len=%d, msg_type=0x%08X',
                    sip, sp, len(payload), msg_type)


def broadcast():
    logger.info('[Broadcast] 启动')
    while running:
        try:
            sock.sendto(oonc(), (MCAST, PORT))
        except Exception as e:
            logger.error('[Broadcast] OONC 失败：%s', e, exc_info=True)
        time.sleep(0.5)

        try:
            sock.sendto(canc(), (MCAST, PORT))
        except Exception as e:
            logger.error('[Broadcast] CANC 失败：%s', e, exc_info=True)
        time.sleep(0.5)
    logger.info('[Broadcast] 退出')


def session_anno():
    logger.info('[Session] 广播线程启动')
    while running:
        try:
            pkt1 = struct.pack('<II', 0x4F4E4E41, 1)
            sock2.sendto(pkt1, (SMCAST, SPORT))
            time.sleep(0.3)

            pkt2 = (struct.pack('<III', 0x4F4E4E41, 1, 1)
                    + b'\x00'*8
                    + socket.inet_aton(ip)
                    + struct.pack('<I', 0x0D5AD030)
                    + b'\x00'*4
                    + struct.pack('<I', 0x0D5AD030)
                    + struct.pack('<I', 1)
                    + b'\x00'*32)
            sock2.sendto(pkt2, (SMCAST, SPORT))
            time.sleep(0.7)
        except Exception as e:
            logger.error('[Session] 广播异常：%s', e, exc_info=True)
            break
    logger.info('[Session] 广播线程退出')


def session_recv():
    logger.info('[SessionRecv] 启动')
    sock2.settimeout(1)
    while running:
        try:
            d, a = sock2.recvfrom(4096)
            sip, sp = a

            # 忽略本机发出的 ANNO 回包
            if sip == ip:
                continue

            if len(d) < 4:
                logger.warning('[SessionRecv] 包过短（%d 字节）from %s:%d', len(d), sip, sp)
                continue

            mag = struct.unpack('<I', d[:4])[0]

            if mag == 0x49474F4C:  # LOGI
                logger.info('[SessionRecv] LOGI from %s:%d', sip, sp)

                mess = (struct.pack('<II', 0x5353454D, 1)
                        + struct.pack('<I', 1)
                        + socket.inet_aton(sip)
                        + b'\x1b\x00\x00\x00\x00\x80\x00\x00'
                        + b'\x00'*8
                        + b'\xc6\x12\x20\x20\x20\x20\xb0\x34\x00\x27\x65')
                sock2.sendto(mess, (sip, SPORT))
                logger.info('[MESS] -> %s:%d, len=%d', sip, SPORT, len(mess))
                time.sleep(0.05)

                lg = bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
                lp = struct.pack('<II', 0x544E504C, 0x10000) + struct.pack('<I', 20) + lg + b'\x02\x00\x00\x00\x00\x00\x00\x00\x50\x00\x00\x00\x3c\x00\x00\x00\x05\x00\x00\x00'
                sock.sendto(lp, (sip, PORT))
                logger.info('[LPNT] subtype=2 -> %s:%d', sip, PORT)

                lp2 = bytes(lp)
                lp2 = lp2[:28] + b'\x03\x00\x00\x00\x01\x00\x00\x00' + lp2[36:]
                sock.sendto(lp2, (sip, PORT))
                logger.info('[LPNT] subtype=3 -> %s:%d', sip, PORT)

                dm = build_dmoc()
                sock.sendto(dm, (sip, PORT))
                logger.info('[DMOC] -> %s:%d, len=%d', sip, PORT, len(dm))

                students[sip] = {'logged_in': True}
                logger.info('[Login] %s 登录成功，students=%s', sip, list(students.keys()))

                threading.Thread(target=keep_alive_preview, args=(sip,), daemon=True).start()

            elif mag == 0x5353454D:  # MESS 学生端发来的消息
                handle_mess(d, sip, sp, 'SessionRecv')

            elif mag not in ROUTINE_MAGICS:
                # 非日常广播包才记录，减少噪音
                logger.warning(
                    '[SessionRecv] 未认证/非登录包 %s from %s:%d, len=%d\n%s',
                    magic_name(mag), sip, sp, len(d), hexdump(d[:256])
                )

        except socket.timeout:
            continue
        except Exception as e:
            if running:
                logger.error('[SessionRecv] 异常：%s', e, exc_info=True)
    logger.info('[SessionRecv] 退出')


def main_recv():
    global running
    logger.info('[MainRecv] 启动')
    while running:
        try:
            d, a = sock.recvfrom(4096)
            sip, sp = a

            # 过滤本机广播
            if sip == ip:
                continue

            if len(d) < 4:
                logger.warning('[MainRecv] 包过短（%d 字节）from %s:%d', len(d), sip, sp)
                continue

            mag = struct.unpack('<I', d[:4])[0]

            # 日常广播（OONC/CANC）直接跳过，不记录
            if mag in ROUTINE_MAGICS:
                continue

            if sip not in students:
                logger.warning(
                    '[MainRecv] 未登录学生 %s:%d 发送 %s',
                    sip, sp, magic_name(mag)
                )

            logger.info('[MainRecv] %s from %s:%d, len=%d', magic_name(mag), sip, sp, len(d))

            if mag == 0x4143414B:  # KACA
                logger.info('[MainRecv] KACA %s -> WACA', sip)
                sock.sendto(waca(sip), (sip, PORT))

            elif mag == 0x434D5254:  # TRMC
                logger.info('[MainRecv] TRMC %s -> LPNT+DMOC', sip)
                lp = build_lpnt_subtype3()
                dm = build_dmoc()
                sock.sendto(lp, (sip, PORT))
                time.sleep(0.05)
                sock.sendto(dm, (sip, PORT))

            elif mag == 0x544E5254:  # TRNT
                logger.info('[MainRecv] TRNT %s 学生准备好预览', sip)

            elif mag == 0x544E4544:  # DENT
                logger.info('[MainRecv] DENT %s -> TNRS', sip)
                tg = bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
                pkt = struct.pack('<II', 0x544E5253, 0x10000) + struct.pack('<I', 14) + tg + b'\x06\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x09\x00'
                sock.sendto(pkt, (sip, PORT))

            elif mag == 0x544E414C:  # LANT
                handle_tnal(d, sip)

            elif mag == 0x5353454D:  # MESS 学生端发来的消息
                handle_mess(d, sip, sp, 'MainRecv')

            else:
                logger.warning('[MainRecv] 未知包 %s from %s:%d, len=%d\n%s',
                               hex(mag), sip, sp, len(d), hexdump(d[:256]))

        except Exception as e:
            if running:
                logger.error('[MainRecv] 异常：%s', e, exc_info=True)
    logger.info('[MainRecv] 退出')


# -------------------- 命令行交互 --------------------

def cmd_help():
    print('''可用命令：
  help / ?              显示帮助
  list / ls             列出已登录学生
  preview <ip>          请求指定学生的屏幕预览
  all                   请求所有学生的屏幕预览
  msg <ip> <text>       向指定学生发送聊天消息
  exit / quit / q       退出程序
''')


def cmd_list():
    if not students:
        print('[命令] 当前无学生登录')
        return
    print('[命令] 已登录学生：')
    for i, sip in enumerate(students.keys(), 1):
        print(f'  {i}. {sip}')


def cmd_preview(args):
    if not args:
        print('[命令] 用法：preview <学生IP>')
        return
    sip = args[0]
    request_preview(sip)
    print(f'[命令] 已向 {sip} 请求预览')


def cmd_all():
    if not students:
        print('[命令] 当前无学生登录')
        return
    for sip in list(students.keys()):
        request_preview(sip)
        time.sleep(0.05)
    print(f'[命令] 已向 {len(students)} 个学生请求预览')


def command_loop():
    global running
    print('教师端已启动，输入 help 查看命令')
    while running:
        try:
            line = input('teacher> ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        parts = line.split(maxsplit=2)
        cmd = parts[0].lower()
        args = parts[1:]

        if cmd in ('help', '?', 'h'):
            cmd_help()
        elif cmd in ('list', 'ls', 'students'):
            cmd_list()
        elif cmd == 'preview':
            cmd_preview(args)
        elif cmd == 'all':
            cmd_all()
        elif cmd == 'msg':
            if len(args) < 2:
                print('[命令] 用法：msg <学生IP> <消息内容>')
            else:
                send_chat(args[0], args[1])
        elif cmd in ('exit', 'quit', 'q'):
            break
        else:
            print(f'[命令] 未知命令：{cmd}，输入 help 查看帮助')


# -------------------- 启动 --------------------

spawn_log_window()
logger.info('启动 4 个后台线程')
threading.Thread(target=broadcast, name='broadcast', daemon=True).start()
threading.Thread(target=session_anno, name='session_anno', daemon=True).start()
threading.Thread(target=session_recv, name='session_recv', daemon=True).start()
threading.Thread(target=main_recv, name='main_recv', daemon=True).start()

command_loop()

running = False
logger.info('程序退出。students=%s, previews=%s', list(students.keys()), list(previews.keys()))
