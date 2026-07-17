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

# 默认文件日志级别：INFO 已足够，DEBUG 太占空间。
# 运行中可用 debug on/off 切换。
FILE_LOG_LEVEL = logging.INFO


def get_file_handler():
    """返回当前的文件日志 handler（不存在返回 None）。"""
    for h in logging.getLogger().handlers:
        if isinstance(h, logging.handlers.RotatingFileHandler):
            return h
    return None


def set_file_log_level(level):
    """切换文件日志级别并持久化到全局变量。"""
    global FILE_LOG_LEVEL
    FILE_LOG_LEVEL = level
    fh = get_file_handler()
    if fh:
        fh.setLevel(level)


def clear_old_logs():
    """启动时清理上一次的日志文件，避免累积过大。"""
    for path in [LOG_PATH] + [f'{LOG_PATH}.{i}' for i in range(1, 4)]:
        try:
            if os.path.exists(path):
                os.remove(path)
        except OSError as e:
            # 如果删不掉（比如被其他进程占用），至少不要阻止程序启动
            print(f'[警告] 删除旧日志 {path} 失败：{e}', file=sys.stderr)


def setup_logging():
    logger = logging.getLogger()
    logger.setLevel(logging.DEBUG)  # 允许 DEBUG 通过，由 handler 决定是否写入
    if logger.handlers:
        logger.handlers.clear()

    clear_old_logs()

    fh = logging.handlers.RotatingFileHandler(
        LOG_PATH, maxBytes=10*1024*1024, backupCount=3, encoding='utf-8'
    )
    fh.setLevel(FILE_LOG_LEVEL)
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

    logger.info('日志系统初始化完成，级别=%s，log 文件：%s',
                logging.getLevelName(FILE_LOG_LEVEL), LOG_PATH)
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
cmd_seq = 0

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
last_status = {}  # (sip, key) -> 最近一条同类消息内容（重复降为 DEBUG，防止刷屏）
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
        # [12..15] 经测试应设为 UTF-16 码元数（含结尾空字符），否则学生端可能只显示第一个字。
        wchar_count = len(text_utf16) // 2
        payload = (struct.pack('<I', 16 + len(text_utf16))
                   + struct.pack('<I', 0x800)
                   + struct.pack('<I', 0)
                   + struct.pack('<I', wchar_count)
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


def request_info(sip, rtype=0):
    """请求学生上报信息（MESS，payload type=0x100000）。

    rtype（学生端 sub_445670 的分支条件）：
           0=全部（type 5 系统信息 + type 6 进程列表 + type 7 窗口列表）
           1=type 6 进程列表（分片）
           2=type 7 窗口列表（分片）
    学生端入口: sub_44CA70 [payload+4]==0x100000 → sub_445670。
    """
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    try:
        payload = (struct.pack('<I', 16)
                   + struct.pack('<I', 0x100000)
                   + struct.pack('<I', 0)
                   + struct.pack('<I', rtype))
        mess = (struct.pack('<II', 0x5353454D, 1)
                + struct.pack('<I', 1)
                + socket.inet_aton(sip)
                + payload)
        sock2.sendto(mess, (sip, SPORT))
        logger.info('[Info] 信息请求 rtype=%d -> %s:%d', rtype, sip, SPORT)
    except Exception as e:
        logger.error('[Info] 发送信息请求失败：%s', e, exc_info=True)
        print(f'[命令] 发送失败：{e}')


def _parse_id_name_pairs(buf):
    """解析 {u32 id, wchar name\0} 序列（type 6 进程列表 / type 7 窗口列表的条目）。

    学生端条目格式（sub_445670）：id(4) + name(UTF-16LE) + \\x00\\x00，
    每条 6 + 2*len(name) 字节。返回 [(id, name), ...]，容错截断。
    """
    entries = []
    pos, end = 0, len(buf)
    while pos + 6 <= end:
        rid = struct.unpack('<I', buf[pos:pos + 4])[0]
        chars = bytearray()
        p = pos + 4
        while p + 2 <= end and buf[p:p + 2] != b'\x00\x00':
            chars += buf[p:p + 2]
            p += 2
        name = bytes(chars).decode('utf-16-le', errors='ignore')
        entries.append((rid, name))
        pos = p + 2
    return entries


def _parse_student_info(payload, sip):
    """解析 type 5 学生信息结构体（自 payload 起，含 12 字节消息头）。

    布局（学生端 sub_445670 填充，均为 UTF-16LE 定长字段）：
      +0x0C  DWORD 5            +0x10  计算机名[32]   +0x50  学生ID u32
      +0x54  MAC[6]             +0x5A  登录用户[32]   +0x9A  OS名称[32]
      +0xDA  OS版本[64]         +0x21A CPU厂商[32]   +0x25A CPU型号[64]
      +0x2DA 内存 "xxxx MB"[16]
    """
    def wstr(off, maxlen):
        raw = payload[off:off + maxlen * 2]
        return raw.decode('utf-16-le', errors='ignore').split('\x00')[0]

    info = {
        'name': wstr(0x10, 32),
        'stu_id': struct.unpack('<I', payload[0x50:0x54])[0],
        'mac': '-'.join(f'{b:02X}' for b in payload[0x54:0x5A]),
        'user': wstr(0x5A, 32),
        'os': wstr(0x9A, 32),
        'osver': wstr(0xDA, 64),
        'cpu_vendor': wstr(0x21A, 32),
        'cpu_model': wstr(0x25A, 64),
        'mem': wstr(0x2DA, 16),
    }
    if sip in students:
        students[sip]['info'] = info
    return info


def build_comd_command(cmd_code, payload):
    """构造 COMD 命令包（Magic=0x434F4D44，即代码中的 DMOC）。"""
    global cmd_seq
    cmd_id = cmd_seq
    cmd_seq += 1
    inner = (struct.pack('<I', cmd_code)
             + struct.pack('<I', cmd_id)
             + struct.pack('<I', len(payload))
             + struct.pack('<I', 0)      # reserved
             + payload)
    return (struct.pack('<II', 0x434F4D44, 0x10000)
            + struct.pack('<I', len(inner))
            + bytes.fromhex('ce90fd383df5844c857fa35183c051f3')
            + inner)


def build_comd_command_ex(cmd_code, payload, guid_hex, extra_header=b''):
    """构造 COMD 命令包，可自定义 GUID 与 GUID 后的额外头。"""
    global cmd_seq
    cmd_id = cmd_seq
    cmd_seq += 1
    inner = (struct.pack('<I', cmd_code)
             + struct.pack('<I', cmd_id)
             + struct.pack('<I', len(payload))
             + struct.pack('<I', 0)
             + payload)
    return (struct.pack('<II', 0x434F4D44, 0x10000)
            + struct.pack('<I', len(extra_header) + len(inner))
            + bytes.fromhex(guid_hex)
            + extra_header
            + inner)


def build_blackscreen_mess_payload(lock_input=True, timeout=10, text=None, text_color=0x0000FFFF):
    """构造黑屏安静命令的 MESS payload。

    逐字节匹配真实抓包（教师端 sub_54C4E0，MESS type=0x20）。

    抓包验证结构（Line 223, 教师→组播 225.2.2.1:5512）：
      [0..3]  = 总长度 (基础 39 字节 + 可选文本)
      [4..7]  = 0x20 (黑屏)
      [8..11] = 0x80000000 (启动标志)
      [12..15]= lock_input (1=锁定键鼠)
      [16..19]= 0x01 (field_1)
      [20..23]= timeout (超时秒数, 0=永久)
      [24..27]= has_text (0/1)
      [28..31]= text_color (Windows COLORREF: 0x00BBGGRR)
      [32..35]= 0x00000000 (field_5)
      [36..38]= 0xA00520 (padding, 仅 has_text=0 时)
      [36..]  = UTF-16LE 文本 (has_text=1 时)

    text_color 默认值 0x0000FFFF = 黄色 (R=255,G=255,B=0)，匹配真实教师端。
    要白色用 0x00FFFFFF，红色用 0x000000FF。
    """
    has_text = 1 if text else 0
    text_utf16 = b''
    if has_text:
        text_utf16 = (text.encode('utf-16-le') + b'\x00\x00') if text else b''

    total_len = 39 + len(text_utf16)  # 基础 39 字节

    payload = struct.pack('<I', total_len)               # [0] 总长
    payload += struct.pack('<I', 0x20)                    # [4] 黑屏
    payload += struct.pack('<I', 0x80000000)              # [8] 启动标志
    payload += struct.pack('<I', 1 if lock_input else 0)  # [12] 锁定输入
    payload += struct.pack('<I', 1)                       # [16] field_1=1
    payload += struct.pack('<I', timeout)                 # [20] 超时
    payload += struct.pack('<I', has_text)                # [24] 有自定义文字
    payload += struct.pack('<I', text_color)              # [28] 文字颜色 (0x00BBGGRR)
    payload += struct.pack('<I', 0)                       # [32] field_5
    if has_text:
        payload += text_utf16                             # [36+] UTF-16LE 文本
    else:
        payload += b'\xa0\x05\x20'                        # [36..38] padding (来自真实抓包)
    return payload


# 跟踪各学生的自动解锁定时器，方便手动解锁时取消
blackscreen_timers = {}   # sip -> threading.Timer


def _send_comd_lock(sip, lock=True):
    """通过 COMD 路径锁定/解锁键鼠（sub_44A490 case 6）。

    MESS 黑屏包负责显示黑屏窗口，COMD case 6 负责实际锁定键鼠。
    真实教师端两条路径都会发。
    """
    payload = struct.pack('<I', 0x200)        # subcmd
    payload += struct.pack('<I', 0)            # flags
    payload += struct.pack('<I', 6)            # case 6 = 黑屏/锁键鼠
    payload += struct.pack('<I', 1 if lock else 0)  # lock_input
    payload += struct.pack('<I', 0)            # timer_flag
    payload += struct.pack('<I', 10)           # timeout
    payload += struct.pack('<I', 0)            # has_text
    payload += b'\x00' * 8                    # padding
    pkt = build_comd_command_ex(0x80000010, payload, 'f96a6d195b2946b9ab958a143ecddc26')
    sock.sendto(pkt, (sip, PORT))
    logger.info('[锁键鼠] COMD case6 lock=%s -> %s:%d', lock, sip, PORT)


def send_blackscreen(sip, lock_input=True, timeout=10, text=None):
    """向已登录学生发送黑屏安静命令。

    MESS 协议 → 组播 225.2.2.1:5512（黑屏窗口 + bit 0x20 状态）
    COMD 协议 → 学生单播 :4705（锁定键鼠，sub_44A490 case 6）

    超时由教师端主动发解锁包实现——先发 flags=0x80000000（锁），
    时间到再发 flags=0x90000000（解）。
    """
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    try:
        # 1) MESS 黑屏包 → 组播（创建黑屏窗口 + 设置 bit 0x20）
        payload = build_blackscreen_mess_payload(lock_input, timeout, text)
        mess = (struct.pack('<II', 0x5353454D, 1)
                + struct.pack('<I', 1)
                + socket.inet_aton(sip)
                + payload)
        sock2.sendto(mess, (SMCAST, SPORT))

        # 2) COMD 锁键鼠 → 单播（实际锁定键盘和鼠标）
        if lock_input:
            _send_comd_lock(sip, lock=True)

        # 取消之前的定时器
        if sip in blackscreen_timers:
            blackscreen_timers[sip].cancel()
            del blackscreen_timers[sip]

        if timeout > 0:
            timeout_str = f'{timeout}秒后自动解锁'
            t = threading.Timer(timeout, _auto_unlock, args=(sip,))
            t.daemon = True
            t.start()
            blackscreen_timers[sip] = t
        else:
            timeout_str = '永久（需手动 unlock）'

        logger.info('[命令] 黑屏安静 -> %s, lock=%s, timeout=%s, text=%s',
                    sip, lock_input, timeout_str, text)
        print(f'[命令] 已向 {sip} 发送黑屏安静（{timeout_str}）')
    except Exception as e:
        logger.error('[命令] 发送黑屏安静失败：%s', e, exc_info=True)
        print(f'[命令] 发送失败：{e}')


def _auto_unlock(sip):
    """定时器回调：到了超时时间自动解锁。"""
    if sip in blackscreen_timers:
        del blackscreen_timers[sip]
    if sip in students:
        logger.info('[AutoUnlock] 超时自动解锁 %s', sip)
        print(f'[自动] {sip} 黑屏超时，正在解锁...')
        _do_unlock(sip)
    else:
        logger.info('[AutoUnlock] %s 已下线，跳过', sip)


def _do_unlock(sip):
    """同时通过 MESS + COMD 两条路径解锁。"""
    try:
        # MESS 解锁 → 组播（flags=0x90000000，清除 bit 0x20）
        payload = (struct.pack('<I', 0x0D)
                   + struct.pack('<I', 0x20)
                   + struct.pack('<I', 0x90000000)
                   + b'\x01')
        mess = (struct.pack('<II', 0x5353454D, 1)
                + struct.pack('<I', 1)
                + socket.inet_aton(sip)
                + payload)
        sock2.sendto(mess, (SMCAST, SPORT))
        logger.info('[解锁] MESS -> %s:%d 目标=%s', SMCAST, SPORT, sip)

        # COMD 解锁 → 单播（lock_input=0，解除键鼠锁）
        _send_comd_lock(sip, lock=False)
    except Exception as e:
        logger.error('[解锁] 发送失败：%s', e, exc_info=True)


def send_unlock(sip):
    """向已登录学生发送解锁命令（MESS + COMD 双路径）。"""
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    # 取消自动解锁定时器
    if sip in blackscreen_timers:
        blackscreen_timers[sip].cancel()
        del blackscreen_timers[sip]
    _do_unlock(sip)
    print(f'[命令] 已向 {sip} 发送解锁')


def send_shutdown(sip, reboot=False, delay=0, force=True, text=None):
    """关机/重启学生机（COMD 0x80000010，category=0x200 → sub_44A490）。

    cmdId: 0x14=关机，0x13=重启，|0x10000000=强制（跳过学生端倒计时气泡）。
    delay>0 且非 force 时，学生端弹倒计时气泡（(delay+1)*1000ms，文本取 text）。
    执行端为学生端目录下 Shutdown.exe：关机=-nb，重启=-b，force=-f/-nf，
    执行后学生端进程自杀退出。

    注意：payload 末尾必须多补 4 字节——学生端按包内 len 从 body+0xC 复制，
    会从 reserved 字段开始算，吃掉 payload 尾部 4 字节。
    """
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    cmd = (0x13 if reboot else 0x14) | (0x10000000 if force else 0)
    payload = struct.pack('<I', 0x200)                # category: 应用命令
    payload += struct.pack('<I', 0)                   # flags
    payload += struct.pack('<I', cmd)                 # cmdId
    payload += struct.pack('<I', delay)               # 延迟秒数
    payload += b'\x00' * 8                            # reserved
    if text:
        payload += text.encode('utf-16-le') + b'\x00\x00'
    payload += b'\x00' * 4                            # 吸收接收端 len 截断
    pkt = build_comd_command_ex(0x80000010, payload,
                                'f96a6d195b2946b9ab958a143ecddc26')
    action = '重启' if reboot else '关机'
    try:
        sock.sendto(pkt, (sip, PORT))
        mode = '强制立即' if force else (f'倒计时{delay}秒' if delay > 0 else '立即')
        logger.info('[命令] %s -> %s, cmd=0x%08X, delay=%d, text=%s',
                    action, sip, cmd, delay, text)
        print(f'[命令] 已向 {sip} 发送{mode}{action}')
    except Exception as e:
        logger.error('[命令] 发送%s失败：%s', action, e, exc_info=True)
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
            logger.debug('[KeepAlive] LPNT -> %s', sip)
        except Exception as e:
            logger.error('[KeepAlive] LPNT -> %s 失败：%s', sip, e, exc_info=True)
        time.sleep(0.05)
        try:
            sock.sendto(dm, (sip, PORT))
            logger.debug('[KeepAlive] DMOC -> %s', sip)
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
    logger.debug('[TNAL] %s total=%d offset=%d frag_len=%d', sip, total, offset, frag_len)

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
    logger.debug('[TNAL] %s 进度 %d/%d (+%d)', sip, state['got'], total, written)

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
    # 去重键：区分消息类别/子类型，避免交替消息绕过相邻去重
    dedupe_key = None
    msg_type = struct.unpack('<I', payload[4:8])[0] if len(payload) >= 8 else 0
    category = struct.unpack('<I', payload[8:12])[0] if len(payload) >= 12 else 0
    subtype = struct.unpack('<I', payload[12:16])[0] if len(payload) >= 16 else 0

    # IDA 中聊天消息负载结构：
    # [0..3]=总长, [4..7]=0x800, [8..11]=0, [12..15]=1, [16..]=UTF-16-LE 字符串
    if len(payload) >= 16 and msg_type == 0x800:
        dedupe_key = 'chat'
        try:
            raw = payload[16:].rstrip(b'\x00')
            if len(raw) % 2:
                raw = raw[:-1]
            text = raw.decode('utf-16-le')
            msg_kind = 'chat'
        except Exception as e:
            logger.debug('[MESS] 聊天消息 UTF-16-LE 解码失败：%s', e)

    elif len(payload) >= 24 and msg_type == 0:
        dedupe_key = f'{category:#010x}/{subtype}'
        extra = struct.unpack('<I', payload[20:24])[0]
        if category == 0x800000:
            # 信息上报（request_info 0x100000 的回复）：
            # [12..15]: 5=系统信息 6=进程列表(分片) 7=窗口列表(分片)
            if subtype == 5:
                if len(payload) >= 0x2E0:
                    try:
                        info = _parse_student_info(payload, sip)
                        text = (f"[学生信息] 计算机名={info['name']} 用户={info['user']} "
                                f"MAC={info['mac']} OS={info['os']} {info['osver']} "
                                f"CPU={info['cpu_vendor']}/{info['cpu_model']} 内存={info['mem']}")
                        msg_kind = 'status'
                    except Exception as e:
                        logger.debug('[MESS] 学生信息解析失败：%s', e)
                else:
                    logger.debug('[MESS] type 5 学生信息长度不足: %d', len(payload))
            elif subtype in (6, 7):
                # [16..19]=分片标志(1=首片), [20..]={u32 id, wchar name\0} 序列
                chunk_flag = struct.unpack('<I', payload[16:20])[0]
                entries = _parse_id_name_pairs(payload[20:])
                store_key = 'processes' if subtype == 6 else 'windows'
                if sip in students:
                    if chunk_flag == 1 or store_key not in students[sip]:
                        students[sip][store_key] = []
                    students[sip][store_key].extend(entries)
                total = len(students[sip][store_key]) if sip in students and store_key in students[sip] else len(entries)
                kind = '进程' if subtype == 6 else '窗口'
                names = '，'.join(n for _, n in entries[:6])
                text = (f'[{kind}列表{"首片" if chunk_flag == 1 else "续片"}] '
                        f'本片 {len(entries)} 个，累计 {total} 个：{names}'
                        + ('…' if len(entries) > 6 else ''))
                msg_kind = 'status'
                logger.debug('[MESS] %s列表完整分片: %s', kind,
                             '，'.join(f'{p}:{n}' for p, n in entries))
            else:
                logger.debug('[MESS] 未知信息上报子类型 %d', subtype)
        else:
            # 状态消息（category=3 等）：
            # [12..15]=子类型, [16..19]=字符串最大长度, [20..23]=PID/计数/额外数据, [24..]=UTF-16-LE 字符串
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
                logger.debug('[MESS] 未知状态子类型 %d (category=%#x)', subtype, category)

    if text:
        if msg_kind == 'chat':
            logger.info('[MESS] 来自 %s:%d 的聊天消息：%s', sip, sp, text)
        elif last_status.get((sip, dedupe_key)) == text:
            # 内容无变化的同类消息（周期性窗口标题/WiFi/列表分片）只记 DEBUG
            logger.debug('[MESS] 来自 %s:%d 的状态消息(重复)：%s', sip, sp, text)
        else:
            last_status[(sip, dedupe_key)] = text
            logger.info('[MESS] 来自 %s:%d 的状态消息：%s', sip, sp, text)
    else:
        key = f'<未解析 type=0x{msg_type:08X} len={len(payload)}>'
        if last_status.get((sip, key)) == payload[:64]:
            logger.debug('[MESS] 来自 %s:%d 的消息无法解析文本(重复)', sip, sp)
        else:
            last_status[(sip, key)] = payload[:64]
            logger.info('[MESS] 来自 %s:%d 的消息无法解析文本，payload_len=%d, msg_type=0x%08X',
                        sip, sp, len(payload), msg_type)


def broadcast():
    logger.info('[Broadcast] 启动')
    while running:
        try:
            sock.sendto(oonc(), (MCAST, PORT))
            logger.debug('[Broadcast] OONC 已发送')
        except Exception as e:
            logger.error('[Broadcast] OONC 失败：%s', e, exc_info=True)
        time.sleep(0.5)

        try:
            sock.sendto(canc(), (MCAST, PORT))
            logger.debug('[Broadcast] CANC 已发送')
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
            logger.debug('[Session] ANNO(type1) 已发送')
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
            logger.debug('[Session] ANNO(type2) 已发送')
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
                already_logged_in = sip in students
                # 已登录学生会周期性重发 LOGI（相当于心跳），属常态——
                # 回复包照发，但日志降为 DEBUG，避免每次心跳刷 4 条 INFO。
                log = logger.debug if already_logged_in else logger.info
                log('[SessionRecv] LOGI from %s:%d%s', sip, sp,
                    '（周期重复）' if already_logged_in else '')

                # 1) 真实教师端第一条回复：msg_type=0x1000，总长 0x0d
                mess1 = (struct.pack('<II', 0x5353454D, 1)
                         + struct.pack('<I', 1)
                         + socket.inet_aton(sip)
                         + b'\x0d\x00\x00\x00\x00\x10\x00\x00'
                         + b'\x00\x00\x00\x00\x00\x00\x00\x00')
                sock2.sendto(mess1, (sip, SPORT))
                log('[MESS] type 0x1000 -> %s:%d, len=%d', sip, SPORT, len(mess1))
                time.sleep(0.05)

                # 2) 真实教师端第二条回复：msg_type=0x8000，总长 0x1b
                mess2 = (struct.pack('<II', 0x5353454D, 1)
                         + struct.pack('<I', 1)
                         + socket.inet_aton(sip)
                         + b'\x1b\x00\x00\x00\x00\x80\x00\x00'
                         + b'\x00\x00\x00\x00\x00\x00\x00\x00'
                         + b'\xc6\x12\x00\x00\x00\x00\xb0\x34\x00\x27\x00')
                sock2.sendto(mess2, (sip, SPORT))
                log('[MESS] type 0x8000 -> %s:%d, len=%d', sip, SPORT, len(mess2))
                time.sleep(0.05)

                if already_logged_in:
                    continue

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

                # 登录后自动请求学生信息（计算机名/MAC/用户/OS/CPU/内存）
                time.sleep(0.2)
                request_info(sip)

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

            # TRMC/TRNT 是心跳/预览就绪包，数量很大，默认用 DEBUG
            if mag in (0x434D5254, 0x544E5254):
                logger.debug('[MainRecv] %s from %s:%d, len=%d',
                             magic_name(mag), sip, sp, len(d))
            else:
                logger.info('[MainRecv] %s from %s:%d, len=%d',
                            magic_name(mag), sip, sp, len(d))

            if mag == 0x4143414B:  # KACA
                logger.info('[MainRecv] KACA %s -> WACA', sip)
                sock.sendto(waca(sip), (sip, PORT))

            elif mag == 0x434D5254:  # TRMC
                logger.debug('[MainRecv] TRMC %s -> LPNT+DMOC', sip)
                lp = build_lpnt_subtype3()
                dm = build_dmoc()
                sock.sendto(lp, (sip, PORT))
                time.sleep(0.05)
                sock.sendto(dm, (sip, PORT))

            elif mag == 0x544E5254:  # TRNT
                logger.debug('[MainRecv] TRNT %s 学生准备好预览', sip)

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

def _parse_lock_text(rest):
    """从 bs/bsperm/bsall 的原始剩余字符串解析 lock 标志和 text。

    rest 为 ip 之后的整段字符串（command_loop 以 maxsplit=2 切分）：
      '' / '0' / '1'       → 仅 lock 标志
      '0 消息' / '1 消息'  → lock + text
      '消息'               → lock=True + text
    """
    lock, text = True, None
    if rest:
        rest = rest.strip()
        if rest in ('0', '1'):
            lock = (rest == '1')
        elif rest[:2] in ('0 ', '1 '):
            lock = (rest[0] == '1')
            text = rest[2:].strip() or None
        else:
            text = rest
    return lock, text


def _parse_delay_text(rest):
    """解析 shutdown/reboot 的 '[倒计时秒数] [提示文字]'。

      ''          → (0, None)
      '30'        → (30, None)
      '30 请保存'  → (30, '请保存')
      '请保存'     → (0, '请保存')
    """
    delay, text = 0, None
    if rest:
        parts = rest.strip().split(maxsplit=1)
        if parts[0].isdigit():
            delay = int(parts[0])
            text = parts[1] if len(parts) > 1 else None
        else:
            text = rest.strip()
    return delay, text


def cmd_help():
    print('''可用命令：
  help / ?              显示帮助
  list / ls             列出已登录学生
  preview <ip>          请求指定学生的屏幕预览
  all                   请求所有学生的屏幕预览
  msg <ip> <text>       向指定学生发送聊天消息
  info <ip> [0|1|2]     请求学生上报信息（0=全部 1=进程列表 2=窗口列表，登录后自动请求一次）
  ps <ip>               显示学生进程列表（先 info 请求过）
  wins <ip>             显示学生窗口列表（先 info 请求过）
  blackscreen / bs <ip> [lock=1|0] [text]   向指定学生发送黑屏安静（默认锁键鼠，10秒自动解锁）
  bsperm / bsp <ip> [lock=1|0] [text]       向指定学生发送永久黑屏安静（需手动 unlock）
  unlock <ip>           解锁指定学生的黑屏/键盘鼠标锁
  bsall [lock=1|0] [text]  对所有已登录学生发送黑屏安静
  unlock_all            对所有已登录学生发送解锁
  shutdown / sd <ip> [秒] [text]   关闭指定学生机（不带秒数=立即强制；带秒数=倒计时提示）
  reboot / rb <ip> [秒] [text]     重启指定学生机（参数同 shutdown）
  debug on / off        切换文件日志级别（默认 INFO，on=DEBUG 会详细记录并占空间）
  exit / quit / q       退出程序

  例：
    bs 192.168.2.139              黑屏 + 锁键鼠，10秒自动解
    bs 192.168.2.139 0            只黑屏不锁键鼠
    bs 192.168.2.139 1 请认真听课  黑屏锁键鼠 + 自定义文字
    shutdown 192.168.2.139        立即强制关机
    reboot 192.168.2.139 30 请保存作业  倒计时30秒重启并显示提示文字''')


def cmd_list():
    if not students:
        print('[命令] 当前无学生登录')
        return
    print('[命令] 已登录学生：')
    for i, (sip, st) in enumerate(students.items(), 1):
        info = st.get('info')
        if info:
            print(f"  {i}. {sip}  {info['name']}  用户:{info['user']}  MAC:{info['mac']}")
            print(f"      OS:{info['os']} {info['osver']}  CPU:{info['cpu_model']}  内存:{info['mem']}")
        else:
            print(f'  {i}. {sip}  (信息未获取，用 info {sip} 请求)')


def cmd_info(args):
    if not args:
        print('[命令] 用法：info <学生IP> [0|1|2]  (0=系统信息 1=+进程列表 2=窗口列表)')
        return
    sip = args[0]
    if sip not in students:
        print(f'[命令] 学生 {sip} 未登录')
        return
    rtype = 0
    if len(args) > 1 and args[1] in ('0', '1', '2'):
        rtype = int(args[1])
    request_info(sip, rtype)
    print(f'[命令] 已向 {sip} 请求信息(rtype={rtype})，结果见日志窗口')


def cmd_ps(args):
    """显示已收集的进程列表（type 6）。"""
    if not args:
        print('[命令] 用法：ps <学生IP>')
        return
    sip = args[0]
    procs = students.get(sip, {}).get('processes')
    if not procs:
        print(f'[命令] 无 {sip} 的进程列表（先 info {sip} 1 请求）')
        return
    print(f'[命令] {sip} 进程列表（{len(procs)} 个）：')
    for pid, name in procs:
        print(f'  {pid:>6}  {name}')


def cmd_wins(args):
    """显示已收集的窗口列表（type 7）。"""
    if not args:
        print('[命令] 用法：wins <学生IP>')
        return
    sip = args[0]
    wins = students.get(sip, {}).get('windows')
    if not wins:
        print(f'[命令] 无 {sip} 的窗口列表（先 info {sip} 2 请求）')
        return
    print(f'[命令] {sip} 窗口列表（{len(wins)} 个）：')
    for hwnd, title in wins:
        print(f'  0x{hwnd:08X}  {title}')


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


def cmd_debug(args):
    if not args or args[0].lower() not in ('on', 'off'):
        print(f'[命令] 用法：debug on/off（当前文件日志级别：{logging.getLevelName(FILE_LOG_LEVEL)}）')
        return
    if args[0].lower() == 'on':
        set_file_log_level(logging.DEBUG)
        print('[命令] 已开启 DEBUG 日志（文件会变大）')
    else:
        set_file_log_level(logging.INFO)
        print('[命令] 已关闭 DEBUG 日志，仅保留 INFO 及以上')



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
        elif cmd == 'debug':
            cmd_debug(args)
        elif cmd == 'msg':
            if len(args) < 2:
                print('[命令] 用法：msg <学生IP> <消息内容>')
            else:
                send_chat(args[0], args[1])
        elif cmd == 'info':
            cmd_info(args)
        elif cmd == 'ps':
            cmd_ps(args)
        elif cmd == 'wins':
            cmd_wins(args)
        elif cmd in ('blackscreen', 'bs'):
            if len(args) < 1:
                print('[命令] 用法：blackscreen <学生IP> [lock=1|0] [提示文字]')
            else:
                lock, text = _parse_lock_text(args[1] if len(args) > 1 else '')
                send_blackscreen(args[0], lock_input=lock, timeout=10, text=text)
        elif cmd in ('bsperm', 'bsp'):
            if len(args) < 1:
                print('[命令] 用法：bsperm <学生IP> [lock=1|0] [提示文字]')
            else:
                lock, text = _parse_lock_text(args[1] if len(args) > 1 else '')
                send_blackscreen(args[0], lock_input=lock, timeout=0, text=text)
        elif cmd == 'unlock':
            if len(args) < 1:
                print('[命令] 用法：unlock <学生IP>')
            else:
                send_unlock(args[0])
        elif cmd == 'bsall':
            if not students:
                print('[命令] 当前无学生登录')
            else:
                lock, text = _parse_lock_text(args[0] if args else '')
                for sip in list(students.keys()):
                    send_blackscreen(sip, lock_input=lock, timeout=10, text=text)
                    time.sleep(0.05)
        elif cmd == 'unlock_all':
            if not students:
                print('[命令] 当前无学生登录')
            else:
                for sip in list(students.keys()):
                    send_unlock(sip)
                    time.sleep(0.05)
        elif cmd in ('shutdown', 'sd'):
            if len(args) < 1:
                print('[命令] 用法：shutdown <学生IP> [倒计时秒数] [提示文字]')
            else:
                delay, text = _parse_delay_text(args[1] if len(args) > 1 else '')
                send_shutdown(args[0], reboot=False, delay=delay,
                              force=(delay == 0), text=text)
        elif cmd in ('reboot', 'rb'):
            if len(args) < 1:
                print('[命令] 用法：reboot <学生IP> [倒计时秒数] [提示文字]')
            else:
                delay, text = _parse_delay_text(args[1] if len(args) > 1 else '')
                send_shutdown(args[0], reboot=True, delay=delay,
                              force=(delay == 0), text=text)
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
