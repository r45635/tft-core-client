# Host build: unit tests and the command-line example. The ESP32 build goes through
# PlatformIO (library.json); nothing here is needed for it.
CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O1 -g
ARDUINOJSON_VERSION := 7.4.2
INC := -Iinclude -Ithird_party -Iports/posix
LIB_SRC := src/buffer.cpp src/digest.cpp src/moo.cpp src/sood.cpp src/ws.cpp src/zones.cpp src/client.cpp \
           ports/posix/posix_io.cpp
TEST_SRC := test/unit/test_main.cpp test/unit/test_moo.cpp test/unit/test_sood.cpp test/unit/test_ws.cpp \
            test/unit/test_zones.cpp test/unit/test_client.cpp
JSON := third_party/ArduinoJson.h

.PHONY: all test zones-host clean
all: test zones-host

# ArduinoJson as one header, same version the firmware pins.
$(JSON):
	@mkdir -p third_party
	curl -fsSL -o $@ https://github.com/bblanchon/ArduinoJson/releases/download/v$(ARDUINOJSON_VERSION)/ArduinoJson-v$(ARDUINOJSON_VERSION).h

build/test: $(JSON) $(LIB_SRC) $(TEST_SRC) $(wildcard include/tftcore/*.h) test/unit/check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) -Itest/unit $(LIB_SRC) $(TEST_SRC) -o $@

test: build/test
	./build/test

build/zones-host: $(JSON) $(LIB_SRC) examples/zones_host/main.cpp $(wildcard include/tftcore/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) $(LIB_SRC) examples/zones_host/main.cpp -o $@

zones-host: build/zones-host

clean:
	rm -rf build
