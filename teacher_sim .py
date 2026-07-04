#!/usr/bin/env python3
"""极域V6.0教师端 - 逐字节匹配真实抓包

本脚本模拟极域课堂管理系统 V6.0 教师端，通过 UDP 多播/单播与学生端通信。
核心能力分两块：
  1. 教师端广播：周期性发送 OONC/CANC/ANNO，让学生端发现教师机。
  2. 接收学生机消息：在 main_recv()、session_recv()、handle_tnal() 中处理
     学生端发来的登录、保活、控制请求、缩略图分片等数据包，并回送响应。

下面代码保留原始的字节级构造逻辑，仅增加中文注释说明“接收学生机消息”部分。
"""
import socket,struct,threading,time,random,sys,os,uuid,colorsys
from PIL import Image, ImageOps
import io

# ---------------------------------------------------------------------------
# 网络配置
# ---------------------------------------------------------------------------
# 主通道多播组：学生端和教师端都用 224.50.50.42:4705 交换控制/缩略图包
MCAST, PORT = '224.50.50.42', 4705
# 会话通道多播组：主要用于 ANNO、学生登录宣告 LOGI、MESS 等
SMCAST, SPORT = '225.2.2.1', 5512
# 教师端 GUID，来自真实教师端，用于 OONC/CANC/LPNT/DMOC 等包
TGUID = uuid.UUID('{F96A6D19-5B29-46B9-AB95-8A143ECDDC26}')

oonc_seq = 0

def get_ip():
    # 通过连外网获取本机 IP，用于构造包中的源 IP 字段
    s=socket.socket(2,2); s.connect(('8.8.8.8',80)); ip=s.getsockname()[0]; s.close(); return ip

ip = get_ip()
# 主通道 socket：接收/发送 4705 端口上的学生端消息
sock = socket.socket(2,2); sock.setsockopt(socket.SOL_SOCKET,4,1); sock.bind(('',4705))
sock.setsockopt(0, socket.IP_ADD_MEMBERSHIP, struct.pack('4s4s', socket.inet_aton(MCAST), socket.inet_aton(ip)))
sock.setsockopt(0, socket.IP_MULTICAST_TTL, 32)
# 会话通道 socket：接收/发送 5512 端口上的登录/状态消息
sock2 = socket.socket(2,2); sock2.setsockopt(socket.SOL_SOCKET,4,1); sock2.bind(('',SPORT))
sock2.setsockopt(0, socket.IP_ADD_MEMBERSHIP, struct.pack('4s4s', socket.inet_aton(SMCAST), socket.inet_aton(ip)))

print(f'Teacher {ip} started on {MCAST}:{PORT} + {SMCAST}:{SPORT}')

# ---------------------------------------------------------------------------
# 学生状态
# ---------------------------------------------------------------------------
# students: 记录已登录的学生机，key=学生IP，value={'logged_in':True}
students={}
# previews: 缩略图分片重组状态，key=学生IP，value={'total':总大小,'buf':bytearray,'got':已收到字节}
previews={}

running=True

# ---------------------------------------------------------------------------
# 教师端广播包构造
# ---------------------------------------------------------------------------
def oonc():
    """Online Notification：教师端在线宣告（多播到 224.50.50.42:4705）。

    结构：Magic 'OONC' (0x4F4F4E43) + 版本 0x10000 + 数据长度 16 + 教师GUID + 教师IP
         + 在线状态 1 + 设备类型 1 + 递增序列号。
    """
    global oonc_seq
    pkt = (struct.pack('<II',0x4F4F4E43,0x10000)
           + struct.pack('<I',16)
           + TGUID.bytes_le
           + socket.inet_aton(ip)
           + struct.pack('<III', 1, 1, oonc_seq))
    oonc_seq += 1
    return pkt

def canc():
    """Control Announcement：教师端控制宣告（多播到 224.50.50.42:4705）。

    结构：Magic 'CANC' (0x43414E43) + 版本 0x10000 + 数据长度 84 + 教师GUID
         + 状态标志 + 教师IP + 在线状态 + 设备类型 + 教师名（UTF-16-LE）。
    """
    n='1\x00'; nw=n.encode('utf-16-le'); nc=len(nw)//2
    af=(nc<<16)|1; p=struct.pack('<II',0x43414E43,0x10000)+struct.pack('<I',84)+TGUID.bytes_le
    p+=struct.pack('<I',af)+socket.inet_aton(ip)+b'\x01\x00\x00\x00\x01\x00\x00\x00'+nw; p+=b'\x00'*(84-len(nw)-8)
    return p

def waca(sip):
    """Wake/Acknowledge：回应学生端的 KACA 保活/发现请求。

    Magic 'WACA' (0x41434157)，学生端发 KACA 后教师端必须回这个包，
    否则学生端可能认为教师端不在线。
    """
    return struct.pack('<II',0x41434157,0x10000)+struct.pack('<I',8)+TGUID.bytes_le+socket.inet_aton(ip)+b'\x01\x00\x00\x00'

def request_preview(sip):
    """主动请求学生端发送预览缩略图 (TNRS)。

    正常情况下学生端准备好后会主动发 TRNT，然后直接传 LANT；
    如果学生端不发，教师端可以主动发 TNRS 触发。
    """
    tg=bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
    # length = guid 之后的数据长度 = 4+4+4+4 = 16
    pkt=struct.pack('<II',0x53524E54,0x10000)+struct.pack('<I',16)+tg
    pkt+=struct.pack('<IIII',0x48,1,0,0x100)
    sock.sendto(pkt,(sip,PORT))
    print(f'[Preview] TNRS sent to {sip}, len={len(pkt)}')

def build_dmoc():
    """构造 DMOC 包（教师端控制信息）。

    学生端登录后，教师端会周期性发送 DMOC + LPNT subtype=3，
    维持会话并告知学生端显示参数（分辨率、颜色等）。
    """
    cg=bytes.fromhex('ce90fd383df5844c857fa35183c051f3')
    dd=b'\x20\x4e\x00\x00'+socket.inet_aton(ip)+b'\x35\x00\x00\x00\x35\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x80'
    dd+=bytes.fromhex('e10202331e16e102023421160000a046000020419a99993fa0052000')
    dd+=b'\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3d\x00'
    return struct.pack('<II',0x434F4D44,0x10000)+struct.pack('<I',len(dd))+cg+dd

def build_lpnt_subtype3():
    """构造 LPNT subtype=3 包。

    LPNT = Login/Preview Notification，subtype=3 表示教师端已就绪，
    学生端收到后可进入缩略图传输阶段。
    """
    lg=bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
    return struct.pack('<II',0x544E504C,0x10000)+struct.pack('<I',20)+lg+b'\x03\x00\x00\x00\x01\x00\x00\x00\x50\x00\x00\x00\x3c\x00\x00\x00\x05\x00\x00\x00'

def keep_alive_preview(sip):
    """学生登录后周期性发送 LPNT subtype=3 + DMOC，直到收到预览或学生下线。

    这是维持学生端在线并诱导其发送缩略图的心跳机制：
      - 每隔约 0.5 秒发一次 LPNT+DMOC；
      - 一旦收到 LANT 分片（sip 进入 previews），说明学生端开始传图，停止发送。
    """
    lp=build_lpnt_subtype3()
    dm=build_dmoc()
    while running and sip in students:
        # 收到 TRNT 或已经开始收 LANT 就停止
        if sip in previews:
            break
        sock.sendto(lp,(sip,PORT))
        print(f'[KeepAlive] LPNT subtype-3 to {sip}')
        time.sleep(0.05)
        sock.sendto(dm,(sip,PORT))
        print(f'[KeepAlive] DMOC to {sip}')
        time.sleep(0.5)

# ---------------------------------------------------------------------------
# 接收学生机消息：缩略图分片重组
# ---------------------------------------------------------------------------
def handle_tnal(d, sip):
    """接收学生端 LANT/TNAL 预览缩略图片段并拼成 JPEG。

    学生端把屏幕截图分成多个 UDP 包发送，每个包头部：
      - 偏移 36-39：total（整张 JPEG 总大小）
      - 偏移 40-43：offset（本分片在图中的偏移）
      - 偏移 44-47：frag_len（本分片长度）
      - 偏移 48+ ：实际 JPEG 数据
    收到全部分片后保存原始 JPEG，并做极域特有的图像修正：
      1. 垂直翻转（学生端使用 bottom-up DIB）；
      2. 仅在 YCbCr 空间反相 Cb/Cr 色度，Y 亮度保持不变。
    """
    if len(d) < 48:
        print(f'[TNAL] too short {len(d)} from {sip}')
        return
    total = struct.unpack('<I', d[36:40])[0]
    offset = struct.unpack('<I', d[40:44])[0]
    frag_len = struct.unpack('<I', d[44:48])[0]
    frag = d[48:48+frag_len]
    print(f'[TNAL] {sip} total={total} offset={offset} frag_len={frag_len} frag_real={len(frag)}')
    if not frag or total == 0:
        print(f'[TNAL] empty fragment or total==0, skip')
        return
    # 每个 IP 独立维护重组缓冲区；total 变化或 offset 为 0 时重置
    state = previews.get(sip)
    if state is None or state['total'] != total or offset == 0:
        state = {'total': total, 'buf': bytearray(total), 'got': 0}
        previews[sip] = state
    end = min(offset + len(frag), total)
    written = end - offset
    if written <= 0:
        print(f'[TNAL] invalid write offset={offset} end={end}')
        return
    state['buf'][offset:end] = frag[:written]
    state['got'] += written
    print(f'[TNAL] {sip} progress {state["got"]}/{total}')
    if state['got'] >= total:
        # 找一个不重复的文件名保存
        idx = 0
        while True:
            fn = os.path.join(os.path.expanduser('~'), 'Desktop', f'preview_{sip.replace(".","_")}_{idx}.jpg')
            if not os.path.exists(fn): break
            idx += 1
        with open(fn, 'wb') as f: f.write(state['buf'])
        print(f'[Preview] saved {fn} ({total} bytes)')
        # 极域学生端传来的 JPEG 是上下颠倒 + 色度反相（Cb/Cr 反相，Y 不变）
        # 同时保存一个修复版本
        try:
            fixed_fn = fn.replace('.jpg', '_fixed.jpg')
            img = Image.open(io.BytesIO(state['buf']))
            # 转换为 RGB（避免 RGBA 等模式导致后续处理失败）
            if img.mode != 'RGB':
                img = img.convert('RGB')
            # 极域学生端截图用的是 bottom-up DIB，需要先垂直翻转
            img = img.transpose(Image.FLIP_TOP_BOTTOM)
            # 学生端编码 JPEG 时只反相了色度（Cb/Cr），没有反亮度（Y）。
            # 如果直接对 RGB 用 ImageOps.invert，黑字会变成白字。
            # 所以只在 YCbCr 的 Cb、Cr 通道上做反相，Y 保持不变。
            ycbcr = img.convert('YCbCr')
            y, cb, cr = ycbcr.split()
            cb = ImageOps.invert(cb)
            cr = ImageOps.invert(cr)
            img = Image.merge('YCbCr', (y, cb, cr)).convert('RGB')
            img.save(fixed_fn, 'JPEG', quality=95)
            print(f'[Preview] saved fixed {fixed_fn}')
        except Exception as e:
            print(f'[Preview] fixed save failed: {e}')
        del previews[sip]

# ---------------------------------------------------------------------------
# 教师端广播线程
# ---------------------------------------------------------------------------
def broadcast():
    while running:
        sock.sendto(oonc(),(MCAST,PORT))
        time.sleep(0.5)
        sock.sendto(canc(),(MCAST,PORT))
        time.sleep(0.5)

def session_anno():
    """在会话通道 225.2.2.1:5512 上周期性发送 ANNO 状态广播。"""
    while running:
        try:
            sock2.sendto(struct.pack('<II',0x4F4E4E41,1),(SMCAST,SPORT))
            time.sleep(0.3)
            a=struct.pack('<III',0x4F4E4E41,1,1)+b'\x00'*8+socket.inet_aton(ip)+struct.pack('<I',0x0D5AD030)+b'\x00'*4+struct.pack('<I',0x0D5AD030)+struct.pack('<I',1)+b'\x00'*32
            sock2.sendto(a,(SMCAST,SPORT))
            time.sleep(0.7)
        except: break

# ---------------------------------------------------------------------------
# 接收学生机消息：会话通道 5512
# ---------------------------------------------------------------------------
def session_recv():
    """处理会话通道 5512 上的学生端消息。

    目前主要处理 LOGI（学生端登录宣告）：
      1. 收到 LOGI 后回 MESS；
      2. 发送 LPNT subtype=2 和 subtype=3；
      3. 发送 DMOC；
      4. 把学生加入 students，启动 keep_alive_preview 维持会话。
    """
    sock2.settimeout(1)
    while running:
        try:
            d,a=sock2.recvfrom(4096); sip,sp=a
            if len(d)<4: continue
            mag=struct.unpack('<I',d[:4])[0]
            if mag==0x49474F4C:
                # LOGI：学生端宣告上线
                print(f'LOGI from {sip}:{sp}')
                # MESS response — 补全真实抓包尾部的 11 字节
                mess=(struct.pack('<II',0x5353454D,1)
                      +struct.pack('<I',1)
                      +socket.inet_aton(sip)
                      +b'\x1b\x00\x00\x00\x00\x80\x00\x00'
                      +b'\x00'*8
                      +b'\xc6\x12\x20\x20\x20\x20\xb0\x34\x00\x27\x65')
                sock2.sendto(mess,(sip,SPORT))
                print(f'[MESS] sent to {sip}:{SPORT}, len={len(mess)}')
                time.sleep(0.05)
                # LPNT x2 (subType=2, then 3)
                lg=bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
                lp=struct.pack('<II',0x544E504C,0x10000)+struct.pack('<I',20)+lg+b'\x02\x00\x00\x00\x00\x00\x00\x00\x50\x00\x00\x00\x3c\x00\x00\x00\x05\x00\x00\x00'
                sock.sendto(lp,(sip,PORT))
                print(f'[LPNT] subtype=2 sent to {sip}:{PORT}')
                lp2=bytes(lp);lp2=lp2[:28]+b'\x03\x00\x00\x00\x01\x00\x00\x00'+lp2[36:]
                sock.sendto(lp2,(sip,PORT))
                print(f'[LPNT] subtype=3 sent to {sip}:{PORT}')
                # DMOC — 使用真实抓包里的教师 GUID，并补上尾部的 3d 00
                dm=build_dmoc()
                sock.sendto(dm,(sip,PORT))
                print(f'[DMOC] sent to {sip}:{PORT}, len={len(dm)}')
                students[sip]={'logged_in':True}
                print(f'{sip} login OK!')
                # 真实流程中教师端会周期性发送 LPNT subtype=3 + DMOC，
                # 直到学生主动发 TRNT 然后开始传 LANT
                threading.Thread(target=keep_alive_preview,args=(sip,),daemon=True).start()
                # 如需要主动请求可取消下面注释
                # request_preview(sip)
        except socket.timeout: continue
        except Exception as e:
            if running: print(f'session err: {e}')

# ---------------------------------------------------------------------------
# 接收学生机消息：主通道 4705
# ---------------------------------------------------------------------------
def main_recv():
    """处理主通道 224.50.50.42:4705 上的学生端消息。

    消息类型（按 Magic 小端序 4 字节）：
      KACA (0x4143414B)：学生端发现/保活请求，回 WACA；
      TRMC (0x434D5254)：学生端控制信息请求，回 LPNT subtype=3 + DMOC；
      TRNT (0x544E5254)：学生端准备好发送缩略图，通常无需回包；
      DENT (0x544E4544)：学生端登录/设备注册完成，回 SRNT；
      LANT (0x544E414C)：缩略图分片，交给 handle_tnal 重组。
    """
    while running:
        try:
            d,a=sock.recvfrom(4096); sip,sp=a
            # 过滤自己广播的 OONC/CANC，减少日志噪音
            if sip == ip:
                continue
            if len(d)<4: continue
            mag=struct.unpack('<I',d[:4])[0]
            if mag==0x4143414B:  # KACA
                print(f'KACA {sip}')
                sock.sendto(waca(sip),(sip,PORT))
            elif mag==0x434D5254:  # TRMC
                print(f'TRMC {sip}')
                lp=build_lpnt_subtype3()
                dm=build_dmoc()
                sock.sendto(lp,(sip,PORT))
                print(f'[TRMC] LPNT subtype-3 sent to {sip}')
                time.sleep(0.05)
                sock.sendto(dm,(sip,PORT))
                print(f'[TRMC] DMOC sent to {sip}')
            elif mag==0x544E5254:  # TRNT
                print(f'TRNT {sip} — student ready for preview')
                # 真实教师端收到 TRNT 后不回 TNRS，学生直接发 LANT
                # 如果学生端不发 LANT，可取消下面注释主动回一个 TNRS
                # request_preview(sip)
            elif mag==0x544E4544:  # DENT
                print(f'DENT {sip} — login DONE')
                tg=bytes.fromhex('aa3a8dbe2b906645908ea29526218540')
                sock.sendto(struct.pack('<II',0x544E5253,0x10000)+struct.pack('<I',14)+tg+b'\x06\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x09\x00',(sip,PORT))
            elif mag==0x544E414C:  # LANT
                handle_tnal(d, sip)
            else:
                print(f'main unknown {hex(mag)} from {sip} len={len(d)}')
        except Exception as e:
            if running: print(f'main err: {e}')

# ---------------------------------------------------------------------------
# 启动
# ---------------------------------------------------------------------------
threading.Thread(target=broadcast,daemon=True).start()
threading.Thread(target=session_anno,daemon=True).start()
threading.Thread(target=session_recv,daemon=True).start()
threading.Thread(target=main_recv,daemon=True).start()
try:
    while True: time.sleep(1)
except KeyboardInterrupt:
    running=False; print('Done')
