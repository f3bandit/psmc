#!/usr/bin/env python3
# PocketStation probe over Phosphor's front ports (card_raw, menu core extension v2).
# Copyright (c) 2026 f3bandit. GPL-2.0-or-later.
#
# Read-only: ID, 58h version, 5Ah status, 5Bh FUNC 00h (date/time) and 5Bh FUNC 01h (memory
# read, used here for the BIOS at 04000000h). Nothing is written to the unit.
# Usage: ps_probe.py [port] [--bios FILE] [--tcp HOST:PORT]
#   port 1 or 2 (default 1): the SS1 front port, through Phosphor's API socket
#   --tcp: a virtual PocketStation (sim/ps_sim --serve, the Windows app) instead; it speaks the
#          same card_raw line, so the probe is identical (the port number is ignored)
import json, socket, sys

SOCK = '/tmp/phosphor-main.sock'


class Phosphor:
    def __init__(self, tcp=None):
        if tcp:
            host, port = tcp.rsplit(':', 1)
            self.s = socket.create_connection((host, int(port)))
            self.s.settimeout(120)       # the sim runs slower than real time
        else:
            self.s = socket.socket(socket.AF_UNIX)
            self.s.connect(SOCK)
            self.s.settimeout(5)
        self.tcp = tcp
        self.buf = b''

    def cmd(self, line):
        self.s.sendall((line + '\n').encode())
        while b'\n' not in self.buf:
            self.buf += self.s.recv(65536)
        out, self.buf = self.buf.split(b'\n', 1)
        return json.loads(out)

    def raw(self, port, data, tries=10):
        hexs = bytes(data).hex()
        for _ in range(tries):
            r = self.cmd(('card_raw %s' % hexs) if self.tcp else ('card_raw %d %s' % (port, hexs)))
            if not r.get('ok'):
                raise RuntimeError(r.get('error'))
            if r['stop'] != 'no card':       # FFh / no ack on the first byte: busy, try again
                return r['count'], bytes.fromhex(r['data']), r['stop']
        return r['count'], bytes.fromhex(r['data']), r['stop']


def show(name, sent, got, stop):
    print('%-28s %3d/%-3d bytes  stop=%s' % (name, len(got), len(sent), stop))
    for i in range(0, len(got), 16):
        print('    tx ' + ' '.join('%02X' % b for b in sent[i:i + 16]))
        print('    rx ' + ' '.join('%02X' % b for b in got[i:i + 16]))


def main():
    port = 1
    bios_out = tcp = None
    args = sys.argv[1:]
    if args and args[0] in ('1', '2'):
        port = int(args.pop(0))
    while args:
        a = args.pop(0)
        if a == '--bios' and args: bios_out = args.pop(0)
        elif a == '--tcp' and args: tcp = args.pop(0)
        else:
            print(__doc__ or 'usage: ps_probe.py [1|2] [--bios FILE] [--tcp HOST:PORT]'); return 2

    p = Phosphor(tcp)
    if not tcp:
        print('ports on:', p.cmd('ports on'))

    probes = [
        ('53h ID', [0x81, 0x53] + [0] * 8),
        ('58h version', [0x81, 0x58, 0, 0, 0]),
        ('5Ah status', [0x81, 0x5A] + [0] * 19),
        ('5Bh FUNC 00h date/time', [0x81, 0x5B, 0x00, 0, 0] + [0] * 8 + [0]),
    ]
    for name, tx in probes:
        n, rx, stop = p.raw(port, tx)
        show(name, tx, rx, stop)

    # 5Bh FUNC 01h: 81 5B 01 LEN1 [addr x4 LSB first, len] LEN2 [data x len] FF
    def mem_read(addr, length=0x80):
        tx = [0x81, 0x5B, 0x01, 0, addr & 0xFF, addr >> 8 & 0xFF, addr >> 16 & 0xFF, addr >> 24 & 0xFF,
              length, 0] + [0] * length + [0]
        n, rx, stop = p.raw(port, tx)
        return tx, rx, stop

    tx, rx, stop = mem_read(0x04000000)
    show('5Bh FUNC 01h @04000000', tx, rx, stop)

    if bios_out:
        bios = bytearray()
        for off in range(0, 0x4000, 0x80):
            tx, rx, stop = mem_read(0x04000000 + off)
            if len(rx) != len(tx) or rx[3] != 0x05 or rx[9] != 0x80:
                print('memory read failed at %04X: stop=%s' % (off, stop))
                return 1
            bios += rx[10:10 + 0x80]
        open(bios_out, 'wb').write(bios)
        print('BIOS: %d bytes to %s' % (len(bios), bios_out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
