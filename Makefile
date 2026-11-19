CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Iinclude
LDFLAGS  ?=
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:.cpp=.o)
BIN      := minivectordb

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(BIN)
	@bash tests/run_tests.sh

clean:
	rm -f $(OBJ) $(BIN)
