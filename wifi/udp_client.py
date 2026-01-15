#!/usr/bin/env python3

import socket

#HOST="127.0.0.1"
HOST="192.168.1.121"
PORT=8080
BUFSIZE=1024

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

print("Enter command:")
cmd = input()
print("cmd=", cmd)
sock.sendto(cmd.encode('ascii'), (HOST, PORT))
#sock.sendto(inString.encode('utf-8'), (HOST, PORT))
#payload, addr = sock.recvfrom(BUFSIZE)
#print(payload.decode())
sock.close()
