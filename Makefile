CXX      = g++
CXXFLAGS = -Wno-multichar -O3 -D NIX
LD       = g++
LDFLAGS  = -pthread
LIBS     = -L/usr/X11/lib -lX11 -lXi -ldl -lm
OBJS     = manual.o mo3.o unmo3.o stb_vorbis.o conf.o gameover.o inter.o \
           twister.o game.o temp.o menu.o assets.o spec_dos.o spec_win.o \
           spec_nix.o spec_web.o
COBJS    = miniaudio/miniaudio.o

.c.o:
	$(CC) -c -O2 -o $*.o $<

.cpp.o:
	$(CXX) -c $(CXXFLAGS) -o $*.o $<

all: depend asciipat

asciipat: $(OBJS) $(COBJS)
	$(LD) -o $@ $(LDFLAGS) $(OBJS) $(COBJS) $(LIBS)

clean:
	rm -f *.o miniaudio/*.o *~ depend

depend: Makefile
	@echo Building dependencies...
	@echo > $@
	@for i in $(OBJS); do \
	    $(CXX) $(CXXFLAGS) -MM $${i%.o}.cpp; \
	done >> $@

sinclude depend
