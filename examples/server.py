import socket

server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

server_socket.bind(('localhost', 12345))

server_socket.listen(1)

print("Сервер запущен и ожидает подключений...")

client_socket, client_address = server_socket.accept()
print(f"Подключение установлено с {client_address}")

data = client_socket.recv(1024)
print(f"Получены данные: {data}")

client_socket.sendall(b'Hello, client!')
print("Ответ отправлен клиенту")

client_socket.close()
server_socket.close() 