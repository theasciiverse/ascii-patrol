#!/usr/bin/env python3
"""Records asciiverse/ascii-patrol.cast, the 100x30 catalog preview: the
intro, then Mission One played until just before the first car is lost.

    make -f asciiverse/Makefile && python3 asciiverse/record-preview.py

Needs pyte. The game runs in a pseudo terminal that answers the kitty
keyboard protocol's query, so key presses and releases reach it the way
they do in kitty or Ghostty, and the output is written straight to an
asciicast v3 file. Takes vary with timing; run it a few times and keep
a long one (--takes N keeps the longest of N).

The driver fires all the time (the forward shots break rocks, the upward
ones hit what flies over) and jumps by the game's own crash rule (see
LEVEL_MODAL in game.cpp): on the ground, the car breaks when neighbouring
wheels stand more than one row apart, or front and rear more than two;
each wheel stands on the highest of four columns, at chassis x+1, x+5
and x+10. It jumps when trouble is ahead and the landing spot is good,
or when trouble is two columns away. It brakes, which slides the car
back on the screen, while a bomb falls toward it.
"""
import argparse, fcntl, json, os, pty, re, select, shutil, struct, tempfile
import termios, threading, time
import pyte

COLS, ROWS = 100, 30
SURF = set('*_,./\\')
WHEELS = (1, 5, 10)
AIR = 0.73   # seconds in the air
KEYS = {'Enter': ('13', 'u'), 'Down': ('1', 'B')}


class Term:
    """the game in a pty, a pyte screen, the output kept as cast events"""

    def __init__(self, argv, env, cwd):
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.lock = threading.Lock()
        self.events = []
        self.t0 = time.time()
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(cwd)
            os.execve(argv[0], argv, env)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
        self.alive = True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while self.alive:
            if not select.select([self.fd], [], [], 0.05)[0]:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                data = b''
            if not data:
                self.alive = False
                break
            if b'\x1b[?u' in data:
                # kitty keyboard protocol flags, then primary device attributes
                os.write(self.fd, b'\x1b[?0u\x1b[?62;22c')
            with self.lock:
                self.stream.feed(data)
                self.events.append((time.time() - self.t0, data))

    def key(self, key, event):
        code, final = KEYS[key] if key in KEYS else (str(ord(key)), 'u')
        ev = '' if event == 1 else ';1:3'
        if final == 'B' and not ev:
            seq = '\x1b[B'
        else:
            seq = f'\x1b[{code}{ev}{final}'
        os.write(self.fd, seq.encode())

    def press(self, key):
        self.key(key, 1)

    def release(self, key):
        self.key(key, 3)

    def tap(self, key, hold=0.08, after=0.3):
        self.press(key)
        time.sleep(hold)
        self.release(key)
        time.sleep(after)

    def text(self):
        with self.lock:
            return '\n'.join(self.screen.display)

    def wait_for(self, needle, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.text():
                return True
            time.sleep(0.1)
        return False

    def close(self):
        try:
            os.kill(self.pid, 15)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass
        self.alive = False


def ground(rows, top=12):
    """per column, the rows of ground from the bottom up to a space or a
    surface character"""
    h = []
    for x in range(len(rows[0])):
        c = 0
        for y in range(len(rows) - 1, top - 1, -1):
            if rows[y][x] in SURF or rows[y][x] == ' ':
                break
            c += 1
        h.append(c)
    return h


def chassis(rows):
    for y, l in enumerate(rows):
        i = l.find('%x%x%x%')
        if i >= 0:
            return i - 1, y
    return None, None


def wheels_ok(h, x):
    if len(h) < x + 14:
        return True
    hw = [max(h[x + o:x + o + 4]) for o in WHEELS]
    return abs(hw[0] - hw[1]) <= 1 and abs(hw[1] - hw[2]) <= 1 and abs(hw[0] - hw[2]) <= 2


def shift(a, b, maxs=6):
    """columns the ground moved left from a to b"""
    best = (10**9, 0)
    for s in range(maxs + 1):
        d = sum(1 for i in range(30, 90) if i + s < len(a) and a[i + s] != b[i])
        if d < best[0]:
            best = (d, s)
    return best[1]


def cars(text):
    m = re.search(r'Cars: (\d)', text.split('\n')[0])
    return int(m.group(1)) if m else None


def drive(term, secs):
    t0 = time.time(); lastfire = lastjump = 0.0
    prev = prevt = None; v = 14.0
    braking = False; brake_t = 0.0
    while time.time() - t0 < secs and term.alive:
        txt = term.text()
        if cars(txt) is not None and cars(txt) < 3:
            break
        rows = txt.split('\n')
        now = time.time()
        cx, cy = chassis(rows)
        if 'Score:' in rows[0] and cx is not None:
            h = ground(rows)
            if prev is None:
                prev, prevt = h, now
            elif now - prevt > 0.04:
                v = 0.8 * v + 0.2 * (shift(prev, h) / (now - prevt))
                prev, prevt = h, now
            if now - lastjump > AIR + 0.05:
                L = int(round(v * AIR))
                unsafe = next((k for k in range(1, 40) if not wheels_ok(h, cx + k)), None)
                if unsafe is not None:
                    landing = all(wheels_ok(h, cx + L + d) for d in (0, 1, 2))
                    if unsafe <= 2 or (unsafe <= L and landing):
                        term.tap('w', hold=0.04, after=0)
                        lastjump = now
            # bombs ('U') falling toward the car
            danger = any(rows[y][x] == 'U' for y in range(3, min(ROWS, cy + 4))
                         for x in range(max(0, cx + 3), min(COLS, cx + 20)))
            if danger and not braking:
                term.press('a'); braking = True; brake_t = now
            elif braking and not danger and now - brake_t > 0.3:
                term.release('a'); braking = False
        if now - lastfire > 0.18:
            term.tap('l', hold=0.04, after=0)
            lastfire = now
        time.sleep(0.01)
    return time.time() - t0


def take(game, secs):
    home = tempfile.mkdtemp(prefix='ap-home-')
    env = {'HOME': home, 'USER': 'player', 'LOGNAME': 'player',
           'TERM': 'xterm-256color', 'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'}
    term = Term(['./asciipat'], env, os.path.dirname(game))
    try:
        term.wait_for('PRESS ANY KEY')
        time.sleep(1.5); term.tap('Enter'); time.sleep(2)
        term.tap('Down', after=0.6); term.tap('Enter'); time.sleep(1.5)
        term.tap('Enter')
        term.wait_for('Score:')
        played = drive(term, secs)
    finally:
        term.close()
        shutil.rmtree(home, ignore_errors=True)
    # end 1.5 s before the first car is lost, if one was
    screen = pyte.Screen(COLS, ROWS); stream = pyte.ByteStream(screen)
    end = None
    for t, data in term.events:
        stream.feed(data)
        if cars('\n'.join(screen.display)) == 2:
            end = t - 1.5
            break
    events = [(t, d) for t, d in term.events if end is None or t <= end]
    return played, events


def write_cast(path, events):
    with open(path, 'w') as f:
        f.write(json.dumps({'version': 3, 'term': {'cols': COLS, 'rows': ROWS,
                            'type': 'xterm-256color'}, 'idle_time_limit': 2}) + '\n')
        last = 0.0
        for t, data in events:
            f.write(json.dumps([round(t - last, 6), 'o', data.decode('utf-8', 'replace')]) + '\n')
            last = t


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--takes', type=int, default=1)
    ap.add_argument('--seconds', type=float, default=70)
    ap.add_argument('--out', default=os.path.join(here, 'ascii-patrol.cast'))
    a = ap.parse_args()
    game = os.path.join(os.path.dirname(here), 'asciipat')
    best = None
    for i in range(a.takes):
        played, events = take(game, a.seconds)
        print(f'take {i + 1}: {played:.1f} s of play')
        if best is None or played > best[0]:
            best = (played, events)
    write_cast(a.out, best[1])
    print(f'{os.path.relpath(a.out)}: {best[1][-1][0]:.1f} s')


if __name__ == '__main__':
    main()
