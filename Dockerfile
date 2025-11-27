FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    g++ cmake build-essential \
    && apt-get clean

WORKDIR /app
COPY . .

RUN g++ -std=c++17 -O2 -pthread -Iinclude src/*.cpp -o my_redis_server

EXPOSE 6379

CMD ["./my_redis_server", "6379"]
