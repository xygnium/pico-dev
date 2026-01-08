#!/usr/bin/env python3

import socket

HOST="127.0.0.1"
PORT=9997
BUFSIZE=2048

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

inString = input()
sock.sendto(inString.encode('utf-8'), (HOST, PORT))
payload, addr = sock.recvfrom(BUFSIZE)
print(payload.decode())
sock.close()
