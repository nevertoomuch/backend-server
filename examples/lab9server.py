import zmq

context = zmq.Context()
socket = context.socket(zmq.REP)
socket.bind("tcp://*:5556")

print("Слушаю на порту 5556...")

counter = 0

try:
    while True:
        message = socket.recv()
        data = message.decode('utf-8')

        counter += 1
        print(f"Получено: {data} Передача: {counter}")
        print("\n")

        with open("received_data.txt", "a", encoding="utf-8") as f:
            f.write(f"{counter}: {data}\n")

        response = "Hello from Server!"
        socket.send(response.encode('utf-8'))

finally:
    socket.close()
    context.term()