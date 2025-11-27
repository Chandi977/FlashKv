FROM ubuntu:22.04

# Required tools
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

# Create working directory
WORKDIR /app

# Copy source code
COPY . /app

# Build your server
RUN g++ -std=c++17 -O2 \
    -pthread \
    -Iinclude \
    src/*.cpp \
    -o my_redis_server

# Expose the port that your server listens on
EXPOSE 6379

# Start the server
CMD ["./my_redis_server"]
