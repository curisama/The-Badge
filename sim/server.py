#!/usr/bin/env python3
"""시뮬레이터를 브라우저에서 만지게 해주는 껍데기.

badge_sim --serve 를 자식으로 띄우고 파이프로 화면을 받는다.

핵심은 **바뀐 칸만 보낸다**는 것. 466x466 을 통째로 PNG 로 말아 보내면
프레임마다 30KB 인데, 시계는 1초에 바늘만 움직이고 홈 화면은 아무것도
안 변한다. 화면을 64px 칸으로 나눠 달라진 칸만 웹소켓으로 밀어낸다.
안 변하면 아무것도 안 보낸다."""
import io, os, struct, subprocess, threading, time, zlib
# numpy 가 있으면 전면 갱신이 훨씬 빠르다. 없으면 예전 방식으로 돈다.
try:
    import numpy as np
except ImportError:
    np = None
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from PIL import Image
import ws

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 466
TILE = 64
COLS = (SIZE + TILE - 1) // TILE

# 🚨 전면이 매 프레임 바뀌는 화면(물·달·지구)은 434KB 를 통째로 보내야 해서
#    30fps 면 초당 13MB 다 — 테일스케일 너머로는 그게 병목이다.
#    절반 해상도로 보내면 4분의 1(초당 3.3MB)로 준다. 브라우저가 늘려 그린다.
#    움직임을 보는 데는 지장이 없고, 화질이 필요하면 ?full=1 로 끈다.
SCALE = int(os.environ.get("SIM_SCALE", "2"))     # 1 = 원본, 2 = 절반
TX_SIZE = (SIZE + SCALE - 1) // SCALE
TX_COLS = (TX_SIZE + TILE - 1) // TILE
PORT = int(os.environ.get("PORT", "8791"))

proc = subprocess.Popen([os.path.join(HERE, "badge_sim"), "--serve"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=HERE)
lock = threading.Lock()
last_tick = time.monotonic()


def _read_exact(n):
    buf = bytearray()
    while len(buf) < n:
        chunk = proc.stdout.read(n - len(buf))
        if not chunk:
            raise RuntimeError("시뮬 프로세스가 죽었다")
        buf += chunk
    return bytes(buf)


def send(cmd):
    proc.stdin.write(cmd.encode())
    proc.stdin.flush()


def apply_events(text):
    """'233,100,1;240,105,0' 또는 'H'(홈)"""
    for ev in text.split(";"):
        ev = ev.strip()
        if not ev:
            continue
        if ev == "H":
            send("H\n")
            continue
        if ev == "W":
            send("W\n")
            continue
        if ev.startswith("N"):
            send(f"N {int(ev[1:])}\n")
            continue
        x, y, st = ev.split(",")
        send(f"T {int(x)} {int(y)} {int(st)}\n")


def step_and_raw(events=""):
    """입력을 흘리고, 지난 호출 이후 흐른 만큼 시간을 민 뒤 RGB565 원본을 받는다."""
    global last_tick
    with lock:
        if events:
            apply_events(events)
        now = time.monotonic()
        ms = min(int((now - last_tick) * 1000), 500)
        last_tick = now
        if ms > 0:
            send(f"P {ms}\n")
        send("R\n")
        header = proc.stdout.readline().split()
        return _read_exact(int(header[1]))


def _diff_tiles_slow(prev, cur):
    """numpy 가 없을 때 쓰는 예전 방식. 전면 갱신이면 눈에 띄게 느리다."""
    out = []
    for ty in range(COLS):
        y0 = ty * TILE
        h = min(TILE, SIZE - y0)
        for tx in range(COLS):
            x0 = tx * TILE
            w = min(TILE, SIZE - x0)
            rows = []
            same = prev is not None
            for r in range(h):
                off = ((y0 + r) * SIZE + x0) * 2
                seg = cur[off:off + w * 2]
                if same and prev[off:off + w * 2] != seg:
                    same = False
                rows.append(seg)
            if not same:
                out.append(struct.pack("!BBBB", tx, ty, w, h) + b"".join(rows))
    return out


def diff_tiles(prev, cur):
    """달라진 칸만 (tx, ty, w, h, 픽셀) 로 뽑는다.

    🚨 예전엔 칸마다 줄 단위로 파이썬에서 잘라 비교했다. 화면 일부만
    바뀌는 화면(시계·계산기)은 견뎠지만, 달·지구처럼 전면이 매 프레임
    바뀌면 프레임당 434KB 를 파이썬으로 훑느라 눈에 띄게 느려졌다
    (0909 에 "너무 느린데?"). numpy 로 한 번에 비교한다.
    """
    if np is None:
        return _diff_tiles_slow(prev, cur)
    ca = np.frombuffer(cur, dtype=np.uint16).reshape(SIZE, SIZE)
    if SCALE > 1:
        ca = ca[::SCALE, ::SCALE]        # 건너뛰며 줄인다 — 공짜다
    out = []
    if prev is None:
        changed = None
    else:
        pa = np.frombuffer(prev, dtype=np.uint16).reshape(SIZE, SIZE)
        if SCALE > 1:
            pa = pa[::SCALE, ::SCALE]
        changed = ca != pa
    for ty in range(TX_COLS):
        y0 = ty * TILE
        h = min(TILE, TX_SIZE - y0)
        for tx in range(TX_COLS):
            x0 = tx * TILE
            w = min(TILE, TX_SIZE - x0)
            if changed is not None and not changed[y0:y0 + h, x0:x0 + w].any():
                continue
            tile = np.ascontiguousarray(ca[y0:y0 + h, x0:x0 + w])
            out.append(struct.pack("!BBBB", tx, ty, w, h) + tile.tobytes())
    return out


def ws_loop(sock):
    prev = None
    idle = 0
    while True:
        try:
            while ws.pending(sock):
                msg = ws.recv(sock)
                if msg is None:
                    return
                apply_events(msg.decode())
            cur = step_and_raw()
            tiles = diff_tiles(prev, cur)
            prev = cur
            if tiles:
                ws.send(sock, zlib.compress(b"".join(tiles), 1))
                idle = 0
            else:
                idle = min(idle + 1, 6)
            time.sleep(0.02 + idle * 0.01)     # 조용하면 천천히 돈다
        except (BrokenPipeError, ConnectionResetError, OSError):
            return


PAGE = """<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,user-scalable=no">
<title>badge sim</title>
<style>
 :root{color-scheme:dark}
 body{margin:0;height:100dvh;display:flex;flex-direction:column;align-items:center;
      justify-content:center;gap:20px;background:#0b0b0c;font:14px system-ui,sans-serif;color:#777}
 .bezel{padding:14px;border-radius:50%;background:linear-gradient(160deg,#4a4a4e,#232326 60%,#38383c);
        box-shadow:0 18px 50px #000a}
 #s{display:block;width:min(78vw,72vh,420px);aspect-ratio:1;border-radius:50%;
     image-rendering:auto;
    touch-action:none;background:#000}
 .row{display:flex;gap:12px;align-items:center}
 button{background:#1c1c1f;color:#ccc;border:1px solid #333;border-radius:10px;
        padding:9px 18px;font-size:14px}
 button:active{background:#2a2a2f}
 .hint{font-size:12px;color:#555;min-width:120px}
</style>
<div class=bezel><canvas id=s width=__TX__ height=__TX__></canvas></div>
<div class=row>
  <button id=boot>BOOT · 화면</button>
  <button id=pwr>PWR · 홈</button>
  <span class=hint id=fps>연결 중…</span>
</div>
<script>
const cv = document.getElementById('s');
const ctx = cv.getContext('2d', {alpha:false});
const TILE = 64, SIZE = 466;   // SIZE 는 터치 좌표용 — 배지 화면 크기 그대로다
let frames = 0, bytes = 0, t0 = performance.now();

function paint(buf){
  const dv = new DataView(buf);
  let p = 0;
  while(p < buf.byteLength){
    const tx = dv.getUint8(p), ty = dv.getUint8(p+1);
    const w = dv.getUint8(p+2), h = dv.getUint8(p+3);
    p += 4;
    const img = ctx.createImageData(w, h);
    const d = img.data;
    for(let i = 0; i < w*h; i++){
      const c = dv.getUint16(p + i*2, true);       // RGB565 리틀엔디안
      d[i*4]   = ((c >> 11) & 0x1F) * 255 / 31;
      d[i*4+1] = ((c >> 5)  & 0x3F) * 255 / 63;
      d[i*4+2] = ( c        & 0x1F) * 255 / 31;
      d[i*4+3] = 255;
    }
    p += w*h*2;
    ctx.putImageData(img, tx*TILE, ty*TILE);
  }
  frames++;
}

let sock;
function connect(){
  sock = new WebSocket((location.protocol==='https:'?'wss://':'ws://') + location.host + '/ws');
  sock.binaryType = 'arraybuffer';
  sock.onmessage = async (e)=>{
    bytes += e.data.byteLength;
    const ds = new DecompressionStream('deflate');
    const buf = await new Response(new Blob([e.data]).stream().pipeThrough(ds)).arrayBuffer();
    paint(buf);
  };
  sock.onclose = ()=>{ document.getElementById('fps').textContent = '끊김 — 다시 연결'; setTimeout(connect, 1000); };
}
connect();

setInterval(()=>{
  const dt = performance.now() - t0;
  document.getElementById('fps').textContent =
      (frames*1000/dt).toFixed(0) + ' fps · ' + (bytes/dt).toFixed(0) + ' KB/s';
  frames = 0; bytes = 0; t0 = performance.now();
}, 1000);

/* 입력은 모았다가 한 번에. 안 그러면 손가락 끌 때 왕복이 폭발한다. */
let queue = [], lastX = -99, lastY = -99;
function pos(e){
  const r = cv.getBoundingClientRect();
  return [Math.round((e.clientX - r.left) / r.width * SIZE),
          Math.round((e.clientY - r.top) / r.height * SIZE)];
}
function touch(e, down, force){
  const [x,y] = pos(e);
  if(!force && Math.abs(x-lastX) < 2 && Math.abs(y-lastY) < 2) return;
  lastX = x; lastY = y;
  queue.push(`${x},${y},${down?1:0}`);
}
setInterval(()=>{
  if(queue.length && sock && sock.readyState === 1){ sock.send(queue.join(';')); queue = []; }
}, 16);
/* 손가락 수를 세어 같이 보낸다. 폰에서 두 손가락 탭이 그대로 시험된다. */
const active = new Set();
function sendCount(){ queue.push('N' + Math.min(active.size, 2)); }
cv.addEventListener('pointerdown', e=>{
  active.add(e.pointerId);
  if(active.size >= 2){ sendCount(); return; }   // 둘째 손가락은 좌표를 안 보낸다
  cv.setPointerCapture(e.pointerId); sendCount(); touch(e,1,true);
});
cv.addEventListener('pointermove', e=>{ if(e.buttons||e.pointerType==='touch') touch(e,1,false); });
function up(e){
  active.delete(e.pointerId);
  sendCount();
  if(active.size === 0) touch(e,0,true);
}
cv.addEventListener('pointerup', up);
cv.addEventListener('pointercancel', up);
/* 실기와 같은 배치: BOOT = 화면 켜기/끄기, PWR 짧게 = 홈 */
document.getElementById('boot').onclick = ()=>{ if(sock && sock.readyState===1) sock.send('W'); };
document.getElementById('pwr').onclick  = ()=>{ if(sock && sock.readyState===1) sock.send('H'); };
</script>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            # 캔버스 뒷면 크기는 전송 해상도에 맞춘다. CSS 크기는 그대로라
            # 브라우저가 늘려 그린다.
            html = PAGE.replace("__TX__", str(TX_SIZE))
            self._send(html.encode(), "text/html; charset=utf-8")
        elif path == "/ws":
            if ws.handshake(self):
                self.close_connection = True
                ws_loop(self.connection)
        elif path == "/frame":                       # 웹소켓 안 되는 곳용 대비책
            ev = parse_qs(urlparse(self.path).query).get("ev", [""])[0]
            raw = step_and_raw(ev)
            img = Image.frombytes("RGB", (SIZE, SIZE), raw, "raw", "BGR;16")
            out = io.BytesIO()
            img.save(out, "PNG", compress_level=1)
            self._send(out.getvalue(), "image/png")
        else:
            self.send_error(404)


if __name__ == "__main__":
    print(f"시뮬 서버 → http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
