import socket

client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

client_socket.connect(('localhost', 12345))

client_socket.sendall(b'Hello, server!')

response = client_socket.recv(1024)
print(f"Получен ответ: {response}") 

client_socket.close()
