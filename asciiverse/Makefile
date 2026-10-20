# Builds asciipat for the Asciiverse catalog, without CMake.
#
#   make -f asciiverse/Makefile                       Linux, macOS
#   make -f asciiverse/Makefile PLATFORM=win \
#     CXX=x86_64-w64-mingw32-g++ CC=x86_64-w64-mingw32-gcc
#
# Linux and macOS use spec_nix.cpp with miniaudio, which loads the
# system's sound at run time; X11's headers are needed on Linux, its
# libraries are not. Windows uses spec_win.cpp (console API and
# DirectSound).

PLATFORM ?= nix
CXXFLAGS ?= -O2
CFLAGS   ?= -O2
LDFLAGS  ?=

SRC = manual.cpp mo3.cpp unmo3.cpp stb_vorbis.cpp conf.cpp gameover.cpp \
      inter.cpp twister.cpp game.cpp temp.cpp menu.cpp assets.cpp

ifeq ($(PLATFORM),win)
EXE   = .exe
DEFS  = -DWIN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS
SRC  += spec_win.cpp
CSRC  =
LIBS  = -static -ldsound -lwinmm -lgdi32 -ldxguid -luuid
else
EXE   =
DEFS  = -DNIX
SRC  += spec_nix.cpp
CSRC  = miniaudio/miniaudio.c
LIBS  = -pthread -ldl -lm
endif

OUT  = build/$(PLATFORM)
OBJS = $(SRC:%.cpp=$(OUT)/%.o) $(CSRC:%.c=$(OUT)/%.o)

asciipat$(EXE): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OUT)/%.o: %.cpp spec.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Wno-multichar $(DEFS) -c -o $@ $<

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build asciipat asciipat.exe

.PHONY: clean
