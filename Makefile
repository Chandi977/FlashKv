# Compiler
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -MMD -MP

# OS-specific flags
ifeq ($(OS),Windows_NT)
    # Windows
    LDFLAGS = -lws2_32
else
    # Linux / macOS
    CXXFLAGS += -pthread
    LDFLAGS =
endif

SRC_DIR = src
BUILD_DIR = build

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TARGET = my_redis_server

all: $(TARGET)

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile source files into object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link object files into the final executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Include dependency files if they exist
-include $(DEPS)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Rebuild everything from scratch
rebuild: clean all

# Run the server
run: all
	./$(TARGET)
