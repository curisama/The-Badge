#!/usr/bin/env python3
"""The shell that lets the simulator be touched from a browser.

It starts badge_sim --serve as a child and takes the screen over a pipe.

🚨 One child, started once and kept for the life of the server. Rebuilding the
simulator does not reach it — restart this server after sim/build.py or you are
looking at the binary from whenever you started it.

The point is that **only the tiles that changed are sent**. Rolling the whole
466x466 into a PNG is 30 KB a frame, while the clock moves one hand a second
and the home screen changes nothing at all. The screen is cut into 64 px tiles
and only the ones that differ go out over the WebSocket. Nothing changed,
nothing sent."""
import io, os, struct, subprocess, threading, time, zlib
# With numpy, a full-screen update is far faster. Without it, the old way runs.
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

# 🚨 Screens where everything changes every frame (water, moon, earth) have to
#    send the whole 434 KB, which at 30fps is 13 MB a second — over anything
#    but a local network that is the bottleneck. Sending at half resolution
#    cuts it to a quarter (3.3 MB a second) and the browser scales it up.
#    Nothing is lost for watching movement, and ?full=1 turns it off when the
#    detail is needed — the planet maps, say.
#    Each connection picks its own, so one viewer can watch at full detail
#    while another stays on half — SIM_SCALE only sets the default.
SCALE = int(os.environ.get("SIM_SCALE", "2"))     # 1 = full, 2 = half

def tx_dims(scale):
    """Transmitted size and tile count for a scale. 🚨 These were globals, which
    is why ?full=1 was documented for months without existing: there was no way
    for one connection to use a different size."""
    tx = (SIZE + scale - 1) // scale
    return tx, (tx + TILE - 1) // TILE

def want_scale(path):
    """?full=1 forces full, ?full=0 forces half, neither takes SIM_SCALE.
    🚨 It has to answer both ways round. Reading only "is full set?" means that
    with SIM_SCALE=1 the toggle cannot turn anything off."""
    q = parse_qs(urlparse(path).query).get("full")
    if not q:
        return SCALE
    return 1 if q[0] not in ("0", "", "false") else 2
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
            raise RuntimeError("the simulator process died")
        buf += chunk
    return bytes(buf)


def send(cmd):
    proc.stdin.write(cmd.encode())
    proc.stdin.flush()


def apply_events(text):
    """'233,100,1;240,105,0' or 'H' (home)"""
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
    """Flushes input, advances time by what has passed since the last call, then takes raw RGB565."""
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
    """The old way, used when numpy is missing. Noticeably slow on a full-screen update."""
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


def diff_tiles(prev, cur, scale):
    """Extracts only the changed tiles as (tx, ty, w, h, pixels).

    🚨 It used to slice and compare row by row per tile in Python. Screens where
    only part changes (clock, calculator) coped, but with everything changing
    every frame, like the moon and the earth, sweeping 434 KB a frame in Python
    got noticeably slow ("this is really slow?", 09-09). numpy compares it in
    one go.
    """
    tx_size, tx_cols = tx_dims(scale)
    if np is None:
        return _diff_tiles_slow(prev, cur)
    ca = np.frombuffer(cur, dtype=np.uint16).reshape(SIZE, SIZE)
    if scale > 1:
        ca = ca[::scale, ::scale]        # decimated — free
    out = []
    if prev is None:
        changed = None
    else:
        pa = np.frombuffer(prev, dtype=np.uint16).reshape(SIZE, SIZE)
        if scale > 1:
            pa = pa[::scale, ::scale]
        changed = ca != pa
    for ty in range(tx_cols):
        y0 = ty * TILE
        h = min(TILE, tx_size - y0)
        for tx in range(tx_cols):
            x0 = tx * TILE
            w = min(TILE, tx_size - x0)
            if changed is not None and not changed[y0:y0 + h, x0:x0 + w].any():
                continue
            tile = np.ascontiguousarray(ca[y0:y0 + h, x0:x0 + w])
            out.append(struct.pack("!BBBB", tx, ty, w, h) + tile.tobytes())
    return out


def ws_loop(sock, scale):
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
            tiles = diff_tiles(prev, cur, scale)
            prev = cur
            if tiles:
                ws.send(sock, zlib.compress(b"".join(tiles), 1))
                idle = 0
            else:
                idle = min(idle + 1, 6)
            time.sleep(0.02 + idle * 0.01)     # goes round slowly when it is quiet
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
  <button id=boot>BOOT · screen</button>
  <button id=pwr>PWR · home</button>
  <button id=hd>__HDLABEL__</button>
  <span class=hint id=fps>connecting…</span>
</div>
<script>
/* Half resolution is the default because a screen that changes everywhere
   costs four times as much at full. The button is here because a query string
   nobody can see is a feature nobody uses. */
document.getElementById('hd').onclick = () => {
  const u = new URL(location.href);
  /* Set it either way rather than deleting — deleting falls back to whatever
     the server was started with, which may be the mode we are leaving. */
  u.searchParams.set('full', __ISFULL__ ? '0' : '1');
  location.href = u.toString();
};

const cv = document.getElementById('s');
const ctx = cv.getContext('2d', {alpha:false});
const TILE = 64, SIZE = 466;   // SIZE is for touch coordinates — the badge screen size as it is
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
      const c = dv.getUint16(p + i*2, true);       // RGB565 little-endian
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
  sock = new WebSocket((location.protocol==='https:'?'wss://':'ws://') + location.host + '/ws__WSQ__');
  sock.binaryType = 'arraybuffer';
  sock.onmessage = async (e)=>{
    bytes += e.data.byteLength;
    const ds = new DecompressionStream('deflate');
    const buf = await new Response(new Blob([e.data]).stream().pipeThrough(ds)).arrayBuffer();
    paint(buf);
  };
  sock.onclose = ()=>{ document.getElementById('fps').textContent = 'disconnected — reconnecting'; setTimeout(connect, 1000); };
}
connect();

setInterval(()=>{
  const dt = performance.now() - t0;
  document.getElementById('fps').textContent =
      (frames*1000/dt).toFixed(0) + ' fps · ' + (bytes/dt).toFixed(0) + ' KB/s';
  frames = 0; bytes = 0; t0 = performance.now();
}, 1000);

/* Input is gathered and sent in one go. Otherwise a finger drag explodes into round trips. */
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
/* The finger count is counted and sent along. A two-finger tap on a phone is tested as it is. */
const active = new Set();
function sendCount(){ queue.push('N' + Math.min(active.size, 2)); }
cv.addEventListener('pointerdown', e=>{
  active.add(e.pointerId);
  if(active.size >= 2){ sendCount(); return; }   // the second finger sends no coordinates
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
/* The same layout as the hardware: BOOT = screen on/off, short PWR = home */
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
            # The canvas backing size follows the transmitted resolution. The
            # CSS size is unchanged, so the browser scales it up.
            scale = want_scale(self.path)
            tx_size, _ = tx_dims(scale)
            html = (PAGE.replace("__TX__", str(tx_size))
                        .replace("__WSQ__", "?full=1" if scale == 1 else "")
                        .replace("__HDLABEL__", "HD · on" if scale == 1 else "HD · off")
                        .replace("__ISFULL__", "true" if scale == 1 else "false"))
            self._send(html.encode(), "text/html; charset=utf-8")
        elif path == "/ws":
            if ws.handshake(self):
                self.close_connection = True
                ws_loop(self.connection, want_scale(self.path))
        elif path == "/frame":                       # the fallback for places without WebSockets
            ev = parse_qs(urlparse(self.path).query).get("ev", [""])[0]
            raw = step_and_raw(ev)
            img = Image.frombytes("RGB", (SIZE, SIZE), raw, "raw", "BGR;16")
            out = io.BytesIO()
            img.save(out, "PNG", compress_level=1)
            self._send(out.getvalue(), "image/png")
        else:
            self.send_error(404)


if __name__ == "__main__":
    print(f"simulator server → http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
