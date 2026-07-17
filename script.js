const canvas = document.querySelector("#packetCanvas");
const ctx = canvas.getContext("2d");
const consoleLines = document.querySelector("#consoleLines");

const packets = [];
const logMessages = [
  "[ANNO] session beacon sent",
  "[LOGI] 192.168.2.139 handshake accepted",
  "[学生信息] 计算机名=DESKTOP-SCRC6OB 用户=测试 MAC=00-0C-29-0F-AB-ED",
  "[进程列表首片] 本片 12 个，累计 12 个",
  "[黑屏安静] -> 192.168.2.139, lock=1, timeout=10秒",
  "[AutoUnlock] 超时自动解锁 192.168.2.139",
  "[命令] 已向 192.168.2.139 发送强制立即关机",
  "[MESS] [窗口标题] Program Manager",
  "[TNAL] fragment 16384/49152",
  "[Preview] fixed JPEG saved",
];

function resizeCanvas() {
  const ratio = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = Math.floor(rect.width * ratio);
  canvas.height = Math.floor(rect.height * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

function makePacket() {
  const rect = canvas.getBoundingClientRect();
  const fromLeft = Math.random() > 0.5;
  packets.push({
    x: fromLeft ? rect.width * 0.16 : rect.width * 0.84,
    y: rect.height * (0.22 + Math.random() * 0.54),
    tx: fromLeft ? rect.width * 0.84 : rect.width * 0.16,
    ty: rect.height * (0.22 + Math.random() * 0.54),
    life: 0,
    speed: 0.006 + Math.random() * 0.006,
    color: fromLeft ? "#27f3d2" : "#ff4d86",
  });
}

function drawGrid(width, height) {
  ctx.strokeStyle = "rgba(255,255,255,0.045)";
  ctx.lineWidth = 1;

  for (let x = 0; x < width; x += 54) {
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
  }

  for (let y = 0; y < height; y += 54) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }
}

function draw() {
  const rect = canvas.getBoundingClientRect();
  ctx.clearRect(0, 0, rect.width, rect.height);
  drawGrid(rect.width, rect.height);

  const centerX = rect.width / 2;
  const centerY = rect.height / 2;
  const pulse = 24 + Math.sin(Date.now() / 500) * 8;

  ctx.strokeStyle = "rgba(199,255,90,0.24)";
  ctx.lineWidth = 1;
  for (let r = 90; r < Math.max(rect.width, rect.height); r += 130) {
    ctx.beginPath();
    ctx.arc(centerX, centerY, r + pulse, 0, Math.PI * 2);
    ctx.stroke();
  }

  if (Math.random() > 0.92 && packets.length < 28) {
    makePacket();
  }

  packets.forEach((packet, index) => {
    packet.life += packet.speed;
    const t = Math.min(packet.life, 1);
    const curve = Math.sin(t * Math.PI) * 80;
    const x = packet.x + (packet.tx - packet.x) * t;
    const y = packet.y + (packet.ty - packet.y) * t - curve;

    ctx.strokeStyle = packet.color;
    ctx.globalAlpha = 0.22;
    ctx.beginPath();
    ctx.moveTo(packet.x, packet.y);
    ctx.quadraticCurveTo(centerX, centerY - 120, packet.tx, packet.ty);
    ctx.stroke();

    ctx.globalAlpha = 0.92;
    ctx.fillStyle = packet.color;
    ctx.beginPath();
    ctx.arc(x, y, 3.8, 0, Math.PI * 2);
    ctx.fill();

    if (packet.life >= 1) {
      packets.splice(index, 1);
    }
  });

  ctx.globalAlpha = 1;
  requestAnimationFrame(draw);
}

function pushLog() {
  const date = new Date();
  const time = date.toTimeString().slice(0, 8);
  const line = document.createElement("p");
  line.innerHTML = `<span>${time}</span> ${logMessages[Math.floor(Math.random() * logMessages.length)]}`;
  consoleLines.appendChild(line);

  while (consoleLines.children.length > 7) {
    consoleLines.removeChild(consoleLines.firstElementChild);
  }
}

window.addEventListener("resize", resizeCanvas);
resizeCanvas();
draw();
setInterval(pushLog, 1800);
