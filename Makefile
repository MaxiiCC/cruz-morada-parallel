CXX      = g++
CXXFLAGS = -Wall -Wextra -O2 -fopenmp -std=c++17
LDFLAGS  = -fopenmp -lssh2 -lcurl

SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN     = cruz_morada

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean

all: $(OBJ_DIR) $(BIN)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)
	@echo ">>> Compilado: $(BIN)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN) resultados.txt log.txt
	@echo ">>> Limpio"
