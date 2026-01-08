#!/usr/bin/env python3


import socket

HOST="127.0.0.1"
PORT=9997
BUFSIZE=2048

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((HOST, PORT))
print("listening on ", HOST, ":", str(PORT))

while True:
    payload, client_addr = sock.recvfrom(BUFSIZE)
    print("echoing to ", str(client_addr), ":", str(payload))
    sent = sock.sendto(payload, client_addr)



