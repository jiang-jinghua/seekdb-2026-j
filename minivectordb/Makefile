CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Isrc
TARGET := minivectordb
SRC := $(wildcard src/*.cpp)

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
