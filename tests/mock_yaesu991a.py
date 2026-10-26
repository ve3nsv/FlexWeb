"""A serial-port FT-991A CAT simulator for hardware-independent integration tests.

Only the core documented commands are implemented; unknown commands return ?.
"""
import os
import pty
import select
import termios
import threading


class Mock991A:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        attr = termios.tcgetattr(self.master)
        attr[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(self.master, termios.TCSANOW, attr)
        self.commands = []
        self.freq_a, self.freq_b = 14230000, 7171000
        self.mode, self.ptt = '2', False
        self.running = False
        self.thread = None

    def respond(self, cmd):
        self.commands.append(cmd)
        if cmd == 'ID': return 'ID0670;'
        if cmd in ('FA', 'FB'):
            return f'{cmd}{(self.freq_a if cmd == "FA" else self.freq_b):09d};'
        if len(cmd) == 11 and cmd[:2] in ('FA', 'FB') and cmd[2:].isdigit():
            if cmd[:2] == 'FA': self.freq_a = int(cmd[2:])
            else: self.freq_b = int(cmd[2:])
            return None
        if cmd == 'MD0': return f'MD0{self.mode};'
        if len(cmd) == 4 and cmd.startswith('MD0') and cmd[-1] in '123456789ABCDE':
            self.mode = cmd[-1]
            return None
        if cmd == 'TX': return f'TX{int(self.ptt)};'
        if cmd in ('TX0', 'TX1'):
            self.ptt = cmd == 'TX1'
            return None
        if cmd == 'SM0': return 'SM0120;'
        if cmd.startswith('RM') and len(cmd) == 3 and cmd[2] in '345678': return cmd + '040;'
        if cmd == 'AG0': return 'AG0128;'
        if cmd == 'PC': return 'PC030;'
        if cmd == 'RA0': return 'RA00;'
        if cmd == 'AC': return 'AC001;'
        return '?;'

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        buf = bytearray()
        while self.running:
            try:
                if not select.select([self.master], [], [], .1)[0]: continue
                data = os.read(self.master, 4096)
                if not data: break
                buf.extend(data)
                while b';' in buf:
                    raw, _, buf = buf.partition(b';')
                    answer = self.respond(raw.decode('ascii', errors='ignore'))
                    if answer: os.write(self.master, answer.encode('ascii'))
            except OSError:
                break

    def stop(self):
        self.running = False
        if self.thread: self.thread.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)
