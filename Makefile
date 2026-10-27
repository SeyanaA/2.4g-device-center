CXX      ?= g++
WINDRES  ?= windres

CXXFLAGS := -std=c++20 -m64 -O2 -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00
LDFLAGS  := -municode -mwindows -static -s -Wl,--gc-sections
LDLIBS   := -lgdi32 -lgdiplus -luser32 -lshell32 -ladvapi32 -lhid -lsetupapi -lcomctl32 -lole32

SRCS := $(filter-out src/resource.rc,$(wildcard src/*.cpp))
OBJS := $(SRCS:.cpp=.o)
RES  := src/resource.o

all: WirelessCenter.exe

src/resource.o: src/resource.rc src/app.manifest resources/app.ico
	$(WINDRES) -I resources -I src $< -o $@

WirelessCenter.exe: $(OBJS) $(RES)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(OBJS) $(RES) WirelessCenter.exe

.PHONY: all clean
