
#ifdef NIX

#ifndef __APPLE__
#define HAVE_X11
#endif

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <dlfcn.h>
#endif
#include <pthread.h>

#include "miniaudio/miniaudio.h"

static bool raw_keyboard = false;
static bool use_xterm_256 = true;

// key presses and releases from a terminal speaking the kitty keyboard
// protocol (sw.kovidgoyal.net/kitty/keyboard-protocol)
static bool kitty_keyboard = false;

#ifdef HAVE_X11
// libX11 and libXi are loaded at run time, so the game also starts
// where they are not installed
static struct
{
	Display* (*OpenDisplay)(const char*);
	int (*CloseDisplay)(Display*);
	int (*Pending)(Display*);
	int (*NextEvent)(Display*, XEvent*);
	Bool (*GetEventData)(Display*, XGenericEventCookie*);
	void (*FreeEventData)(Display*, XGenericEventCookie*);
	Status (*kbGetIndicatorState)(Display*, unsigned int, unsigned int*);
	int (*GetInputFocus)(Display*, Window*, int*);
	int (*SelectInput)(Display*, Window, long);
	Bool (*kbSetDetectableAutoRepeat)(Display*, Bool, Bool*);
	Bool (*QueryExtension)(Display*, const char*, int*, int*, int*);
	int (*Flush)(Display*);
	int (*ISelectEvents)(Display*, Window, XIEventMask*, int);
} x11;

#define XOpenDisplay x11.OpenDisplay
#define XCloseDisplay x11.CloseDisplay
#define XPending x11.Pending
#define XNextEvent x11.NextEvent
#define XGetEventData x11.GetEventData
#define XFreeEventData x11.FreeEventData
#define XkbGetIndicatorState x11.kbGetIndicatorState
#define XGetInputFocus x11.GetInputFocus
#define XSelectInput x11.SelectInput
#define XkbSetDetectableAutoRepeat x11.kbSetDetectableAutoRepeat
#define XQueryExtension x11.QueryExtension
#define XFlush x11.Flush
#define XISelectEvents x11.ISelectEvents

static bool load_x11()
{
	void* x = dlopen("libX11.so.6", RTLD_NOW|RTLD_LOCAL);
	void* xi = x ? dlopen("libXi.so.6", RTLD_NOW|RTLD_LOCAL) : 0;
	if (!xi)
		return false;

	#define X11_SYM(lib,f,name) if (!(*(void**)&x11.f = dlsym(lib,name))) return false;
	X11_SYM(x, OpenDisplay, "XOpenDisplay")
	X11_SYM(x, CloseDisplay, "XCloseDisplay")
	X11_SYM(x, Pending, "XPending")
	X11_SYM(x, NextEvent, "XNextEvent")
	X11_SYM(x, GetEventData, "XGetEventData")
	X11_SYM(x, FreeEventData, "XFreeEventData")
	X11_SYM(x, kbGetIndicatorState, "XkbGetIndicatorState")
	X11_SYM(x, GetInputFocus, "XGetInputFocus")
	X11_SYM(x, SelectInput, "XSelectInput")
	X11_SYM(x, kbSetDetectableAutoRepeat, "XkbSetDetectableAutoRepeat")
	X11_SYM(x, QueryExtension, "XQueryExtension")
	X11_SYM(x, Flush, "XFlush")
	X11_SYM(xi, ISelectEvents, "XISelectEvents")
	#undef X11_SYM
	return true;
}

static bool use_xi = false;
static int xi_opcode = -1;
static Display* dpy = 0;
static Window win; // current focus
static Window root; // default root
static Window victim; // our victim
#else
static void* dpy = 0;
#endif

#include <string.h>

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>


#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <errno.h>

extern char** environ;
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef FIONREAD
	// CYGWIN
	#include <sys/socket.h>
#elif defined(__APPLE__)
	#include <fcntl.h>
#else
	// LINUX
	#include <linux/kd.h>
	#include <fcntl.h>
	#define LINUX
#endif

#ifdef MIN
#undef MIN
#endif

#ifdef MAX
#undef MAX
#endif

#include "spec.h"
#include "conf.h"
#include "menu.h"

void DBG(const char* str)
{
}


bool has_key_releases()
{
	return dpy!=0 || raw_keyboard || kitty_keyboard;
}

int fopen_s(FILE** f, const char* path, const char* mode)
{
	if (!f)
		return -1;
	*f = fopen(path,mode);
	return *f==0;
}

unsigned int get_time()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    unsigned int tm = now.tv_sec*1000 + now.tv_usec/1000;
    if (!tm)
		tm=1;
    return tm;
}

static char read_buf[3];
static int  read_len=0;

static void kitty_pump();
static int kitty_events();

bool get_input_len(int* r)
{
	if (kitty_keyboard)
	{
		kitty_pump();
		*r = kitty_events();
		return true;
	}

#ifdef HAVE_X11
	if (dpy)
	{
		*r = XPending(dpy);
		return true;
	}
#endif

	ioctl( fileno(stdin), FIONREAD, r);
	*r+=read_len;
	return true;
}

char read_char()
{
	while (1)
	{
		if (!read_len)
		{
			int nm=0;
			get_input_len(&nm);
			if (!nm)
				return 0;
			read_buf[0] = getchar();
			read_len++;
		}

		if (read_buf[0]==27)
		{
			if (read_len==1)
			{
				int nm=0;
				get_input_len(&nm);
				if (!nm)
				{
					read_len=0;
					return 27;
				}
				read_buf[1] = getchar();
				read_len=2;
			}

			if (read_buf[1]!='[')
			{
				read_len--;
				if (read_len)
					memmove(read_buf, read_buf+1, read_len);
				return 27;
			}

			if (read_len==2)
			{
				int nm=0;
				get_input_len(&nm);
				if (!nm)
					return 0;
				read_buf[2]=getchar();
				read_len=3;
			}

			char ch=0;
			switch (read_buf[2])
			{
				case 'A':
					ch = KBD_UP;
					break;
				case 'B':
					ch = KBD_DN;
					break;
				case 'C':
					ch = KBD_RT;
					break;
				case 'D':
					ch = KBD_LT;
					break;

				case 72: // LX
					ch = KBD_HOM;
					break;
				case 70: // LX
					ch = KBD_END;
					break;
			}

			if (ch)
			{
				read_len=0;
				return ch;
			}

			int nm=0;
			get_input_len(&nm);
			if (!nm)
				return 0;
			char tilde = getchar();

			switch (read_buf[2])
			{
				case 55: // rxvt
				case 49:
					ch = KBD_HOM;
					break;
				case 50:
					ch = KBD_INS;
					break;
				case 51:
					ch = KBD_DEL;
					break;
				case 56: // rxvt
				case 52:
					ch = KBD_END;
					break;
				case 53:
					ch = KBD_PUP;
					break;
				case 54:
					ch = KBD_PDN;
					break;
			}

			read_len=0;
			if (tilde!='~')
			{
				read_buf[0] = tilde;
				read_len = 1;
			}

			return ch;
		}
		else
		{
			char ch = read_buf[0];
			read_len--;
			if (read_len)
				memmove(read_buf, read_buf+1, read_len);

			if (ch==10)
				ch=13; // enter
			if (ch==127)
				ch=8;  // bk space

			return ch;
		}
	}
}


static const int key_map[]=
{
	0,0,0,0,0,0,0,0,

	 0,27,'!1','@2','#3','$4','%5','^6','&7','*8','(9',')0','_-','+=',8,
	   '\t','Qq','Ww','Ee','Rr','Tt','Yy','Uu','Ii','Oo','Pp','{[','}]',13,
	        0,'Aa','Ss','Dd','Ff','Gg','Hh','Jj','Kk','Ll',':;','\"\'',
   '~`',0,'|\\','Zz','Xx','Cc','Vv','Bb','Nn','Mm','<,','>.','?/',0,
};

// kitty keyboard protocol: every key arrives as CSI ... u (or CSI ... ~ and
// CSI 1;... A-D, H, F for the cursor and editing keys), with its
// modifiers and whether it was pressed, repeated or released
static unsigned char kitty_buf[512];
static int kitty_len = 0;
static CON_INPUT kitty_queue[256];
static int kitty_head = 0;
static int kitty_tail = 0;

static int kitty_events()
{
	return (kitty_tail - kitty_head) & 255;
}

static void kitty_push(int type, char ch, bool down)
{
	if (((kitty_tail+1) & 255) == kitty_head)
		return; // full, drop

	CON_INPUT* ir = kitty_queue + kitty_tail;
	ir->EventType = type;
	if (type == CON_INPUT_FOC)
		ir->Event.FocusEvent.bSetFocus = down;
	else
	{
		ir->Event.KeyEvent.bKeyDown = down;
		ir->Event.KeyEvent.uChar.AsciiChar = ch;
	}
	kitty_tail = (kitty_tail+1) & 255;
}

// US layout shift for the punctuation keys
static char kitty_shift(int c)
{
	static const char plain[] = "`1234567890-=[]\\;',./";
	static const char shift[] = "~!@#$%^&*()_+{}|:\"<>?";
	if (c>='a' && c<='z')
		return c-'a'+'A';
	const char* p = strchr(plain,c);
	return p ? shift[p-plain] : c;
}

// one sequence's parameters: up to 3 fields of up to 3 ':' parts each
static void kitty_key(const char* par, char final)
{
	int f[3][3] = {{0,0,0},{1,1,0},{0,0,0}};
	bool given[3][3] = {{false}};
	int fi = 0, pi = 0;
	for (const char* c = par; *c; c++)
	{
		if (*c==';')
		{
			if (++fi>2) break;
			pi = 0;
		}
		else
		if (*c==':')
		{
			if (++pi>2) pi = 2;
		}
		else
		if (*c>='0' && *c<='9')
		{
			if (!given[fi][pi])
				f[fi][pi] = 0;
			given[fi][pi] = true;
			f[fi][pi] = f[fi][pi]*10 + (*c-'0');
		}
	}

	int key = given[0][0] ? f[0][0] : 1;
	int mods = (given[1][0] ? f[1][0] : 1) - 1;
	int ev = given[1][1] ? f[1][1] : 1;
	bool down = ev != 3;
	bool shift = mods & 1;
	bool ctrl = mods & 4;
	bool caps = mods & 64;

	char ch = 0;
	switch (final)
	{
		case 'A': ch = KBD_UP; break;
		case 'B': ch = KBD_DN; break;
		case 'C': ch = KBD_RT; break;
		case 'D': ch = KBD_LT; break;
		case 'H': ch = KBD_HOM; break;
		case 'F': ch = KBD_END; break;
		case '~':
			switch (key)
			{
				case 1: case 7: ch = KBD_HOM; break;
				case 2: ch = KBD_INS; break;
				case 3: ch = KBD_DEL; break;
				case 4: case 8: ch = KBD_END; break;
				case 5: ch = KBD_PUP; break;
				case 6: ch = KBD_PDN; break;
			}
			break;
		case 'u':
			if (key==13 || key==57414)
				ch = 13;
			else
			if (key==9)
				ch = '\t';
			else
			if (key==127 || key==8)
				ch = 8;
			else
			if (key==27)
				ch = 27;
			else
			if (key>=57399 && key<=57408)
				ch = '0' + key-57399; // keypad digits
			else
			if (key>=57409 && key<=57413)
				ch = "./*-+"[key-57409]; // keypad operators
			else
			if (key>=57417 && key<=57426)
			{
				static const char kp[] = {KBD_LT,KBD_RT,KBD_UP,KBD_DN,KBD_PUP,KBD_PDN,KBD_HOM,KBD_END,KBD_INS,KBD_DEL};
				ch = kp[key-57417];
			}
			else
			if (key>=32 && key<127)
			{
				bool upper = shift;
				if (key>='a' && key<='z' && caps)
					upper = !upper;
				if (shift && given[0][1] && f[0][1]>=32 && f[0][1]<127 && !(key>='a' && key<='z' && caps))
					ch = f[0][1];
				else
				if (upper)
					ch = kitty_shift(key);
				else
					ch = key;

				if (ctrl && down && (key=='c' || key=='C'))
				{
					raise(SIGINT);
					return;
				}
			}
			break;
	}

	if (ch)
		kitty_push(CON_INPUT_KBD, ch, down);
}

static void kitty_pump()
{
	int avail = 0;
	ioctl( 0, FIONREAD, &avail);
	while (avail>0 && kitty_len < (int)sizeof(kitty_buf))
	{
		int n = read(0, kitty_buf+kitty_len, sizeof(kitty_buf)-kitty_len);
		if (n<=0)
			break;
		kitty_len += n;
		avail -= n;
	}

	int i = 0;
	while (i<kitty_len)
	{
		unsigned char c = kitty_buf[i];
		if (c!=27)
		{
			// plain byte (pasted text): a press and a release
			char ch = c==10 ? 13 : c==127 ? 8 : c;
			kitty_push(CON_INPUT_KBD, ch, true);
			kitty_push(CON_INPUT_KBD, ch, false);
			i++;
			continue;
		}

		if (i+1>=kitty_len)
			break; // wait for the rest

		if (kitty_buf[i+1]!='[')
		{
			kitty_push(CON_INPUT_KBD, 27, true);
			kitty_push(CON_INPUT_KBD, 27, false);
			i++;
			continue;
		}

		int j = i+2;
		while (j<kitty_len && (kitty_buf[j]<0x40 || kitty_buf[j]>0x7E))
			j++;
		if (j>=kitty_len)
		{
			if (kitty_len - i >= 64)
				i = kitty_len; // garbage, drop it
			break;
		}

		char par[64];
		int len = j-(i+2);
		if (len>63)
			len = 63;
		memcpy(par, kitty_buf+i+2, len);
		par[len] = 0;
		char final = kitty_buf[j];
		i = j+1;

		if (final=='I' && !len)
			kitty_push(CON_INPUT_FOC, 0, true);
		else
		if (final=='O' && !len)
			kitty_push(CON_INPUT_FOC, 0, false);
		else
		if (len && (par[0]<'0' || par[0]>';'))
			continue; // a reply such as CSI ? ... u, not a key
		else
			kitty_key(par, final);
	}

	if (i)
	{
		memmove(kitty_buf, kitty_buf+i, kitty_len-i);
		kitty_len -= i;
	}
}

bool spec_read_input(CON_INPUT* ir, int n, int* r)
{
	int nm = 0;
	get_input_len(&nm);

	n = n>nm ? nm : n;

	static int caps=0;
	static int l_shift=0;
	static int r_shift=0;

	static int ctrl=0;

	if (raw_keyboard)
	{
		int i;
		for (i=0; i<n; i++)
		{
			int res;
			unsigned char scan;

			res = read(0, &scan, 1);
			if (!res)
			{
				break;
			}

			switch (scan & 0x7F)
			{
				case 0xe0: // escape code
					scan=0;
					break;

				case 0x2a:
					l_shift = scan<0x80;
					scan=0;
					break;

				case 0x36:
					r_shift = scan<0x80;
					scan=0;
					break;

				case 0x1d:
					// l-ctrl is 0x1d
					// r-ctrl is prefixed with 0xe0
					ctrl = scan<0x80;
					scan=0;
					break;

				case 66:
					// capslock
					scan=0;
					break;
			}

			if (scan)
			{
				ir[i].EventType = CON_INPUT_KBD;
				ir[i].Event.KeyEvent.bKeyDown = (scan & 0x80)==0;
				ir[i].Event.KeyEvent.uChar.AsciiChar = 0;

				int c = (scan+8)&0x7f;

				switch (c)
				{
					case 65: ir[i].Event.KeyEvent.uChar.AsciiChar = ' '; break;
					case 'n': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_HOM; break;
					case 'o': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_UP; break;
					case 'p': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PUP; break;
					case 'q': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_LT; break;
					case 'r': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_RT; break;
					case 's': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_END; break;
					case 't': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DN; break;
					case 'u': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PDN; break;
					case 'v': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_INS; break;
					case 'w': ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DEL; break;

					default:
					{
						if (c*sizeof(int)<sizeof(key_map))
						{
							int s = (l_shift+r_shift) > 0;
							if ((key_map[c]&0xFF) >='a' && (key_map[c]&0xFF) <='z')
								s ^= caps;

							if (s && (key_map[c]&0xFF00))
								ir[i].Event.KeyEvent.uChar.AsciiChar = (key_map[c]>>8)&0xFF;
							else
								ir[i].Event.KeyEvent.uChar.AsciiChar = key_map[c]&0xFF;
						}
					}
				}

				if ( ctrl &&
					 (ir[i].Event.KeyEvent.uChar.AsciiChar == 'c' ||
					  ir[i].Event.KeyEvent.uChar.AsciiChar == 'C') )
				{
					// do not put ^C
					i--;
					raise(SIGINT);
				}
			}
			else
			{
				// skip
				i--;
			}
		}

		if (r)
			*r=i;
		return true;
	}

	if (kitty_keyboard)
	{
		kitty_pump();
		int i=0;
		while (i<n && kitty_head!=kitty_tail)
		{
			ir[i++] = kitty_queue[kitty_head];
			kitty_head = (kitty_head+1) & 255;
		}
		if (r)
			*r = i;
		return true;
	}

	if (!dpy)
	{
		for (int i=0; i<n; i++)
		{
			int ch = read_char();
			if (ch<=0)
			{
				if (r)
					*r = i;
				return true;
			}

			ir[i].EventType = CON_INPUT_KBD;
			ir[i].Event.KeyEvent.bKeyDown = true;
			ir[i].Event.KeyEvent.uChar.AsciiChar = ch;
		}

		if (r)
			*r = n;

	    return true;
	}

#ifdef HAVE_X11
	// in non grabbed way: eat all from stdin if submitted
	int discard;
	ioctl( fileno(stdin), FIONREAD, &discard);
	while (discard)
	{
		char chunk[256];
		int bytes = discard > 256 ? 256 : discard;
		discard -= fread(chunk,1,bytes,stdin);
	}

	int i=0;
	for (int j=0; j<nm && i<n; j++)
	{
		XEvent ev;

		XGenericEventCookie *cookie = &ev.xcookie;
		XNextEvent(dpy, &ev);

		if (use_xi)
		{
	            if (cookie->type != GenericEvent ||
        		cookie->extension != xi_opcode)
	                continue;

	            if (XGetEventData(dpy, cookie))
	            {

			if (cookie->evtype == 2 || cookie->evtype == 3)
			{
			    XIDeviceEvent* key = (XIDeviceEvent*)cookie->data;

				ir[i].EventType = CON_INPUT_KBD;
				ir[i].Event.KeyEvent.bKeyDown = cookie->evtype == 2;
				ir[i].Event.KeyEvent.uChar.AsciiChar = 0;

				int c = key->detail;

				switch (c&0x7f)
				{
					case 0xe0+8: // escape code
						break;

					case 0x2a+8:
						l_shift = ir[i].Event.KeyEvent.bKeyDown;
					break;

					case 0x36+8:
						r_shift = ir[i].Event.KeyEvent.bKeyDown;
					break;

					case 66:
					{
						//if (ir[i].Event.KeyEvent.bKeyDown)
						{
							unsigned int n=0;
							XkbGetIndicatorState(dpy, XkbUseCoreKbd, &n);
							caps = n&1;
						}
						break;
					}

					case  65: ir[i].Event.KeyEvent.uChar.AsciiChar = ' '; break;
					case 110: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_HOM; break;
					case 111: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_UP; break;
					case 112: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PUP; break;
					case 113: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_LT; break;
					case 114: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_RT; break;
					case 115: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_END; break;
					case 116: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DN; break;
					case 117: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PDN; break;
					case 118: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_INS; break;
					case 119: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DEL; break;

					default:
						if (c*sizeof(int)<sizeof(key_map))
						{
							int s = (l_shift+r_shift) > 0;
							if ((key_map[c]&0xFF) >='a' && (key_map[c]&0xFF) <='z')
								s ^= caps;

							if (s && (key_map[c]&0xFF00))
								ir[i].Event.KeyEvent.uChar.AsciiChar = (key_map[c]>>8)&0xFF;
							else
								ir[i].Event.KeyEvent.uChar.AsciiChar = key_map[c]&0xFF;
						}
				}

				i++;
				break;

			}

	                XFreeEventData(dpy, &ev.xcookie);
			continue;
	            }
		    // continue;
		}

		switch(ev.type)
		{
			case FocusIn:
				{
					unsigned int n=0;
					XkbGetIndicatorState(dpy, XkbUseCoreKbd, &n);
					caps = n&1;
				}
				break;

			case FocusOut:

				int revert;
				XGetInputFocus(dpy, &win, &revert);

				if (win == PointerRoot)
                		    win = root;

				XSelectInput(dpy, win, KeyPressMask|KeyReleaseMask|FocusChangeMask);

				if (ev.xfocus.window == victim)
				{
					ir[i].EventType = CON_INPUT_FOC;
					ir[i].Event.FocusEvent.bSetFocus = 0;
					i++;
				}

			    break;

			case KeyPress:
			case KeyRelease:
			{
				if (ev.xkey.window != victim)
					break;

				ir[i].EventType = CON_INPUT_KBD;
				ir[i].Event.KeyEvent.bKeyDown = ev.type == KeyPress;
				ir[i].Event.KeyEvent.uChar.AsciiChar = 0;

				int c = ev.xkey.keycode;

				switch (c&0x7f)
				{
					case 0xe0+8: // escape code
						break;

					case 0x2a+8:
						l_shift = ir[i].Event.KeyEvent.bKeyDown;
					break;

					case 0x36+8:
						r_shift = ir[i].Event.KeyEvent.bKeyDown;
					break;

					case 66:
					{
						//if (ir[i].Event.KeyEvent.bKeyDown)
						{
							unsigned int n=0;
							XkbGetIndicatorState(dpy, XkbUseCoreKbd, &n);
							caps = n&1;
						}
						break;
					}

					case  65: ir[i].Event.KeyEvent.uChar.AsciiChar = ' '; break;
					case 110: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_HOM; break;
					case 111: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_UP; break;
					case 112: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PUP; break;
					case 113: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_LT; break;
					case 114: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_RT; break;
					case 115: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_END; break;
					case 116: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DN; break;
					case 117: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_PDN; break;
					case 118: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_INS; break;
					case 119: ir[i].Event.KeyEvent.uChar.AsciiChar = KBD_DEL; break;


					default:
						if (c*sizeof(int)<sizeof(key_map))
						{
							int s = (l_shift+r_shift) > 0;
							if ((key_map[c]&0xFF) >='a' && (key_map[c]&0xFF) <='z')
								s ^= caps;

							if (s && (key_map[c]&0xFF00))
								ir[i].Event.KeyEvent.uChar.AsciiChar = (key_map[c]>>8)&0xFF;
							else
								ir[i].Event.KeyEvent.uChar.AsciiChar = key_map[c]&0xFF;
						}
				}

				i++;
				break;
			}
		}
	}

	if (r)
		*r = i;
#endif

	return true;
}

void sleep_ms(int ms)
{
	usleep(ms*1000);
}

void vsync_wait()
{
	sleep_ms(8);
}

static struct termios old, cur;
static int old_keyboard_mode;

/* Initialize new terminal i/o settings */
static void initTermios()
{
	tcgetattr(0, &old); /* grab old terminal i/o settings */

#ifdef LINUX
    struct termios tty_attr;
    int flags;

    /* save old keyboard mode */
    if (ioctl(0, KDGKBMODE, &old_keyboard_mode) >= 0)
	{

		/* make stdin non-blocking */
		// originaly before (save old keyboard mode)
		flags = fcntl(0, F_GETFL);
		flags |= O_NONBLOCK;
		fcntl(0, F_SETFL, flags);

		/* turn off buffering, echo and key processing */
		tty_attr = old;
		tty_attr.c_lflag &= ~(ICANON | ECHO | ISIG);
		tty_attr.c_iflag &= ~(ISTRIP | INLCR | ICRNL | IGNCR | IXON | IXOFF);
		tcsetattr(0, TCSANOW, &tty_attr);

		ioctl(0, KDSKBMODE, K_MEDIUMRAW);
		raw_keyboard = true;
	}
	else
#endif
	{
		raw_keyboard = false;

		setbuf(stdin, NULL);
		cur = old;
		cur.c_lflag |=  ISIG;
		cur.c_iflag &= ~IGNBRK;
		cur.c_iflag |=  BRKINT;
		cur.c_lflag &= ~ICANON; /* disable buffered i/o */
		cur.c_lflag &= ~ECHO; /* disable echo mode */
		tcsetattr(0, TCSANOW, &cur);
	}
}

/* Restore old terminal i/o settings */
static void resetTermios(void)
{
	tcsetattr(0, TCSAFLUSH, &old);
#ifndef __APPLE__
	if (raw_keyboard)
		ioctl(0, KDSKBMODE, old_keyboard_mode);
#endif
}

static int ansi_key_autorep(bool on)
{
    if (on)
	return printf("\x1B[?8h");
    else
	return printf("\x1B[?8l");
}

static int ansi_set_wrap(bool on)
{
    if (on)
	return printf("\x1B[?7h");
    else
	return printf("\x1B[?7l");
}

static int ansi_show_cursor(bool show)
{
    if (show)
	return printf("\x1B[?25h");
    else
	return printf("\x1B[?25l");
}

static int ansi_goto_xy(int x, int y)
{
    return printf("\x1B[%d;%df",y+1,x+1);
}

static int ansi_set_color(int fg, int bg)
{
    // 8 color pal
    return printf("\x1B[%d;%dm",fg%8+30,bg%8+40);
}

static int ansi_set_color_ex(int fg, int bg)
{
	// LINUX text console
	if (use_xterm_256)
	    return printf("\x1B[38;5;%d;48;5;%dm",fg%256,bg%256);

	// bold & blink
	return printf("\x1B[%d;%d;%s;%sm",fg%8+30,bg%8+40,fg<8?"22":"1",bg<8?"25":"5");
}

static int ansi_set_color_rgb(int fg, int bg)
{
    // RGB color
    return printf("\x1B[38;2;%d;%d;%d;48;2;%d;%d;%d;m",
    	fg&0xff,(fg>>8)&0xff,(fg>>16)&0xff,
    	bg&0xff,(bg>>8)&0xff,(bg>>16)&0xff);
}

static int ansi_def_color()
{
    return printf("\x1B[0m");
}

static int ansi_screen(bool alt)
{
	if (alt)
		return printf("\x1B[?1049h");
	else
		return printf("\x1B[?1049l");
}

static void get_wh(int* w, int* h)
{
    winsize ws;
    ioctl(0, TIOCGWINSZ, &ws);

    if (w)
	*w = ws.ws_col;
    if (h)
	*h = ws.ws_row;
}


void free_sound();

void terminal_done()
{
#ifdef HAVE_X11
	if (dpy)
	{
		XCloseDisplay(dpy);
		dpy = 0;
	}
#endif

	if (kitty_keyboard)
	{
		printf("\x1B[?1004l\x1B[<u");
		kitty_keyboard = false;
	}

	resetTermios();

    ansi_def_color();
    ansi_show_cursor(true);
    ansi_set_wrap(true);
    ansi_key_autorep(true);

	ansi_screen(false);
	fflush(stdout);

	// most likely will fail, so put it at the end.
	free_sound();
}

static void terminal_signal(int signum)
{
	terminal_done();
	_exit(128 + signum);
}


// asks for the kitty keyboard protocol's flags, then for the primary
// device attributes, which every terminal answers; only a terminal
// that knows the protocol answers the first
static bool detect_kitty()
{
	printf("\x1B[?u\x1B[c");
	fflush(stdout);

	char buf[256];
	int len = 0;
	unsigned int start = get_time();
	while (get_time()-start < 500 && len < (int)sizeof(buf)-1)
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(0,&fds);
		struct timeval tv = {0, 50000};
		if (select(1,&fds,0,0,&tv)<=0)
			continue;
		int n = read(0, buf+len, sizeof(buf)-1-len);
		if (n<=0)
			break;
		len += n;
		buf[len] = 0;

		// done at the device attributes: CSI ? ... c
		char* da = strstr(buf,"\x1B[?");
		bool done = false;
		while (da)
		{
			char* e = da+3;
			while (*e && ((*e>='0' && *e<='9') || *e==';'))
				e++;
			if (*e=='c')
				done = true;
			da = strstr(da+1,"\x1B[?");
		}
		if (done)
			break;
	}
	buf[len] = 0;

	char* q = strstr(buf,"\x1B[?");
	while (q)
	{
		char* e = q+3;
		while (*e>='0' && *e<='9')
			e++;
		if (*e=='u' && e>q+3)
			return true;
		q = strstr(q+1,"\x1B[?");
	}
	return false;
}

#ifdef HAVE_X11
static void select_events(Display *dpy, Window win)
{
    XIEventMask evmasks[2];
    unsigned char mask1[(XI_LASTEVENT + 7)/8];
    unsigned char mask2[(XI_LASTEVENT + 7)/8];

    memset(mask1, 0, sizeof(mask1));

    /* select for button and key events from all master devices */
//    XISetMask(mask1, XI_ButtonPress);
//    XISetMask(mask1, XI_ButtonRelease);
    XISetMask(mask1, XI_KeyPress);
    XISetMask(mask1, XI_KeyRelease);

    evmasks[0].deviceid = XIAllMasterDevices;
    evmasks[0].mask_len = sizeof(mask1);
    evmasks[0].mask = mask1;

    memset(mask2, 0, sizeof(mask2));

    /* Select for motion from the default cursor */
    XISetMask(mask2, XI_Motion);

    evmasks[1].deviceid = 2; /* the default cursor */
    evmasks[1].mask_len = sizeof(mask2);
    evmasks[1].mask = mask2;

    XISelectEvents(dpy, win, evmasks, 2);
    XFlush(dpy);
}
#endif



int terminal_init(int argc, char* argv[], int* dw, int* dh)
{
	// todo:
	// try to setup ansi palette
	// try to enable mouse events (enough to track moves when left button is down only)

	const char* term = getenv("TERM");
	if (term && strcmp(term,"linux")==0)
		use_xterm_256 = false;

    initTermios();

	LoadConf();

    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_handler = terminal_signal;
    act.sa_flags = 0;
    sigaction(SIGHUP,  &act, NULL);
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGXCPU, &act, NULL);
    sigaction(SIGXFSZ, &act, NULL);
    sigaction(SIGIO,   &act, NULL);
    sigaction(SIGPIPE, &act, NULL);
    sigaction(SIGALRM, &act, NULL);

    int cols,rows;
    get_wh(&cols,&rows);

    printf("W=%d, H=%d\n",cols,rows);

	const char* wayland = getenv("WAYLAND_DISPLAY");
	const char* display = getenv("DISPLAY");

	if (raw_keyboard)
	{
		printf("Using RAW Keyboard... excelent!\n");
	}
	else
	if (detect_kitty())
	{
		// disambiguate, report press/repeat/release, alternate keys,
		// all keys as escape codes; and focus in/out
		printf("\x1B[>15u\x1B[?1004h");
		kitty_keyboard = true;
	}
#ifdef HAVE_X11
	else
	if (!(wayland && wayland[0]) && display && display[0] && load_x11())
	{
		// X11 gives key releases; under Wayland it would not see the
		// terminal's keys at all
		printf("Falling back to X11 input\n");
		dpy = XOpenDisplay(NULL);


		if (!dpy)
		{
			printf("unable to connect to X11 display\nfalling back to sticky stdin!\n");
		}
		else
		{



			Bool supported = false;
			XkbSetDetectableAutoRepeat(dpy, True, &supported);

			int revert;

			XGetInputFocus(dpy, &win, &revert);

			if (win == PointerRoot)
				win = root;

			if (win)
			{
			    int event, error;
			    if (XQueryExtension(dpy, "XInputExtension", &xi_opcode,&event,&error))
			    {
				select_events(dpy,win);
				use_xi = true;
			    }
			    else
			    {

				printf("connecting input to window = %lu\n",win);

				victim = win;
				root = DefaultRootWindow(dpy);
				XSelectInput(dpy, win, FocusChangeMask|KeyPressMask|KeyReleaseMask);

				/*
				XGrabKeyboard(dpy, win,
									  False,
									  GrabModeAsync,
									  GrabModeAsync,
									  CurrentTime);
				*/

				XFlush(dpy);
			    }
			}
			else
			{
				XCloseDisplay(dpy);
				dpy = 0;
				printf("unable to get x11 active window\nfalling back to sticky stdin!\n");
			}
		}
	}
#endif

	ansi_screen(true);
    ansi_set_wrap(false);
    ansi_show_cursor(false);
    ansi_key_autorep(false);

	*dw=cols;
	*dh=rows;

	return 0;
}

void get_terminal_wh(int* dw, int* dh)
{
	get_wh(dw,dh);
}

void free_con_output(CON_OUTPUT* screen)
{
	char* arr = (char*)screen->arr;
	if (arr)
		delete [] arr;

	screen->arr = 0;
}

// terminal size at the last write; a change clears the terminal and
// draws everything again
static int written_w = -1;
static int written_h = -1;

// Each changed row is drawn at its own position: the screen is centred
// on a terminal bigger than the game's largest (160x50) and clipped,
// not scrolled, on one smaller than its smallest (80x25)
int screen_write(CON_OUTPUT* screen, int dw, int dh, int sx, int sy, int sw, int sh)
{
	int w = screen->w;
	int h = screen->h;
	char* buf = screen->buf;
	char* color = screen->color;
	char* arr = (char*)screen->arr;

	int tw = 0, th = 0;
	get_wh(&tw,&th);
	int ox = tw>sw ? (tw-sw)/2 : 0;
	int oy = th>sh ? (th-sh)/2 : 0;

	int size = 0;

	if (tw!=written_w || th!=written_h)
	{
		written_w = tw;
		written_h = th;
		if (arr)
		{
			delete [] arr;
			screen->arr = arr = 0;
		}
		size += ansi_def_color();
		size += printf("\x1B[2J");
	}

	int cols = sw;
	if (ox+cols>tw)
		cols = tw-ox;

	int cl = 256;
	if (!color)
		size += ansi_set_color_ex(0,15);

	for (int y=0; y<sh && oy+y<th; y++)
	{
		char* row = buf+(w+1)*y;
		char* row_cl = color ? color+(w+1)*y : 0;

		if (arr && memcmp(row,arr+(w+1)*y,w+1)==0 &&
			(!color || memcmp(row_cl,arr+(w+1)*(y+h),w+1)==0))
			continue;

		size += ansi_goto_xy(ox,oy+y);

		if (!color)
		{
			size += fwrite(row,1,cols,stdout);
			continue;
		}

		int from = 0;
		for (int x=0; x<=cols; x++)
		{
			if (x<cols && (unsigned char)row_cl[x]==cl)
				continue;
			if (x>from)
				size += fwrite(row+from,1,x-from,stdout);
			if (x<cols)
			{
				cl = (unsigned char)row_cl[x];
				size += ansi_set_color_ex(cl&0x0F,(cl>>4)&0x0F);
			}
			from = x;
		}
	}

	int bytes = (w+1)*h*(color ? 2 : 1);
	if (!arr)
		screen->arr = arr = new char[bytes];
	memcpy(arr,buf,bytes);

	fflush(stdout);
	return size;
}

void terminal_loop()
{
	while (modal)
	{
		if (modal->Run()==-2)
			break;
	}
}

void write_fs()
{
}

static void make_dirs(const char* dir)
{
	char path[1024];
	snprintf(path,sizeof(path),"%s",dir);
	for (char* c=path+1; *c; c++)
	{
		if (*c=='/')
		{
			*c=0;
			mkdir(path,0755);
			*c='/';
		}
	}
	mkdir(path,0755);
}

// Player files live in $XDG_DATA_HOME/ascii-patrol (~/.local/share/ascii-patrol)
static const char* data_dir()
{
	static char dir[1024]="";

	if (!dir[0])
	{
		const char* xdg = getenv("XDG_DATA_HOME");
		const char* home = getenv("HOME");
		if (xdg && xdg[0]=='/')
			snprintf(dir,sizeof(dir),"%s/ascii-patrol",xdg);
		else
		if (home && home[0])
			snprintf(dir,sizeof(dir),"%s/.local/share/ascii-patrol",home);
		else
			strcpy(dir,".");
		make_dirs(dir);
	}

	return dir;
}

// $<env>/<name> if set, else a file an older version left in $HOME,
// else the data folder
static void player_path(char* path, int size, const char* env, const char* name)
{
	const char* dir = getenv(env);
	if (dir && dir[0])
	{
		snprintf(path,size,"%s/%s",dir,name);
		return;
	}

	const char* home = getenv("HOME");
	if (home && home[0])
	{
		snprintf(path,size,"%s/%s",home,name);
		if (access(path,F_OK)==0)
			return;
	}

	snprintf(path,size,"%s/%s",data_dir(),name);
}

const char* conf_path()
{
	static char path[1024]="";

	if (!path[0])
		player_path(path,sizeof(path),"ASCII_PATROL_CONFIG_DIR","asciipat.cfg");

	return path;
}

const char* record_path()
{
	static char path[1024]="";

	if (!path[0])
		player_path(path,sizeof(path),"ASCII_PATROL_RECORD_DIR","asciipat.rec");

	return path;
}

const char* shot_path()
{
	static char path[1024]="";

	if (!path[0])
	{
		const char* dir = getenv("ASCII_PATROL_SHOT_DIR");
		if (!dir || !dir[0])
			dir = data_dir();
		snprintf(path,sizeof(path),"%s/", dir);
	}

	return path;
}

static const char* rank_path()
{
	static char path[1024]="";

	if (!path[0])
		snprintf(path,sizeof(path),"%s/asciipat.rnk", data_dir());

	return path;
}

static pthread_mutex_t hiscore_cs = PTHREAD_MUTEX_INITIALIZER;
static pthread_t hiscore_thread = 0;
static HISCORE hiscore_data[2]={{0,0,0,"",""},{0,0,0,"",""}};
static int hiscore_queue=0;
static int hiscore_running=0;

static HISCORE hiscore_async = {0,0,0,"",""};
static bool hiscore_ready = false;

// system() would ignore SIGINT and SIGQUIT while curl runs and then put
// back what they were before, undoing the handlers terminal_init() sets
// meanwhile, so Ctrl+C would no longer restore the terminal
static int run_shell(const char* cmd)
{
	const char* argv[] = {"sh", "-c", cmd, 0};
	pid_t pid;
	if (posix_spawn(&pid, "/bin/sh", 0, 0, (char* const*)argv, environ))
		return -1;

	int status = 0;
	while (waitpid(pid, &status, 0)<0)
	{
		if (errno!=EINTR)
			return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void* hiscore_proc(void* p)
{
	int err = 0;
	bool end = false;

	const char* path = rank_path();

	while (!end)
	{
		fflush(0);
		err = run_shell(hiscore_data->buf);

		// check if we need to discard this data and re-fetch
		pthread_mutex_lock(&hiscore_cs);

		HISCORE tmp={0,0,0,"",""};

		if (!err)
		{
			FILE* f = 0;
			fopen_s(&f,path,"rb");
			if (f)
			{
				char line[56];
				if (fgets(line,56,f))
				{
					if ( sscanf(line,"%d/%d",&tmp.ofs,&tmp.tot) != 2 )
					{
						fclose(f);
						err = -2;
					}
					else
					{
						char* id = strchr(line,' ');
						if (id)
						{
							id++;
							char* id_end = strchr(id,' ');
							int id_len = id_end-id;
							if (id_len>15)
								id_len=15;
							memcpy(tmp.id,id,id_len);
							tmp.id[id_len]=0;
						}
					}
				}

				if (!err)
				{
					tmp.siz = 0;
					while (fgets(line,56,f))
					{
						/*
						// no need to put terminator before avatar
						line[45]=0;
						*/
						memcpy(tmp.buf + 55*tmp.siz, line, 55);
						tmp.siz++;
					}

					// change last cr to eof
					tmp.buf[55 * tmp.siz -1] = 0;
				}

				fclose(f);
			}
			else
				err = -3;
		}
		else
			err = -1;

		if (!err)
		{
			// prevent overwriting id if no new id is returned
			if (hiscore_async.id[0] && !tmp.id[0])
				memcpy(tmp.id,hiscore_async.id,16);

			hiscore_async = tmp;
			hiscore_ready = true;
		}

		hiscore_queue--;

		end = (hiscore_queue==0);

		if (!end)
			hiscore_data[0] = hiscore_data[1];

		pthread_mutex_unlock(&hiscore_cs);

		if (end)
			break;

		sleep_ms(250); // save OS from being flooded by pg-up/dn key auto-repeat
	}

	*(int*)p = 0; // clear running flag

	pthread_exit((void*)(intptr_t)err);
	return (void*)(intptr_t)err;
}

void request_hiscore(const char* cmd)
{
	// need to handle 4 situations
	// 1. no thread is running, copy data[1] to data[0] and create new thread
	// 2. thread is running but queue len is 0, wait for thread to finish, and crate new as above
	// 3. thread is running and queue len is 1, pump up len to 2 and let it run
	// 4. thread is running and queue len is 2, just let it run

	if (!hiscore_thread)
	{
		memset(&(hiscore_data[0]),0,sizeof(HISCORE));
		strcpy_s(hiscore_data[0].buf,65*55,cmd);

		hiscore_queue = 1;
		hiscore_running = 1;
		pthread_create(&hiscore_thread, 0, hiscore_proc, &hiscore_running);
	}
	else
	{
		pthread_mutex_lock(&hiscore_cs);

		if (hiscore_queue == 0)
		{
			// we are bit late
			pthread_mutex_unlock(&hiscore_cs);

			pthread_join(hiscore_thread, 0);
			hiscore_thread=0;

			memset(&(hiscore_data[0]),0,sizeof(HISCORE));
			strcpy_s(hiscore_data[0].buf,65*55,cmd);

			hiscore_queue = 1;
			hiscore_running = 1;
			pthread_create(&hiscore_thread, 0, hiscore_proc, &hiscore_running);
		}
		else
		{
			if (hiscore_queue == 1)
				hiscore_queue = 2;

			memset(&(hiscore_data[1]),0,sizeof(HISCORE));
			strcpy_s(hiscore_data[1].buf,65*55,cmd);

			pthread_mutex_unlock(&hiscore_cs);
		}
	}
}

void get_hiscore(int ofs, const char* id)
{
	if (hiscore_sync)
		return;

	const char* path = rank_path();

	char cmd[1024];

	if (id && id[0])
	{
		snprintf(cmd,sizeof(cmd),
			"curl --silent -o \"%s\" \"http://ascii-patrol.com/rank.php?rank=%d&id=%s\" 2>/dev/null"
			,path,ofs+1,id);
	}
	else
	{
		snprintf(cmd,sizeof(cmd),
			"curl --silent -o \"%s\" \"http://ascii-patrol.com/rank.php?rank=%d\" 2>/dev/null"
			,path,ofs+1);
	}

	request_hiscore(cmd);
}

void post_hiscore()
{
	const char* path = rank_path();

	char cmd[1024];
	snprintf(cmd,sizeof(cmd),
		"curl --silent -o \"%s\" -F \"rec=@%s\" \"http://ascii-patrol.com/rank.php\" 2>/dev/null"
		,path,
		record_path());

	request_hiscore(cmd);

	hiscore_sync = true; // prevents removal of post by get requests from queue
}

bool update_hiscore()
{
	if (!hiscore_thread)
		return true;

	if (!hiscore_running)
	{
		pthread_join(hiscore_thread,0);
		hiscore_thread=0;

		if (hiscore_ready)
		{
			hiscore_ready = false;

			// prevent overwriting id if no new id is returned
			if (hiscore.id[0]!=0 && hiscore_async.id[0]==0)
				memcpy(hiscore_async.id,hiscore.id,16);

			hiscore = hiscore_async;
		}

		hiscore_sync = false;

		return true;
	}

	pthread_mutex_lock(&hiscore_cs);

	if (hiscore_queue == 0)
	{
		// we are bit late
		pthread_mutex_unlock(&hiscore_cs);

		pthread_join(hiscore_thread,0);

		hiscore_thread=0;

		if (hiscore_ready)
		{
			hiscore_ready = false;
			hiscore = hiscore_async;
		}

		hiscore_sync = false;

		return true;
	}
	else
	{
		if (hiscore_ready)
		{
			hiscore_ready = false;
			hiscore = hiscore_async;
		}

		pthread_mutex_unlock(&hiscore_cs);
	}

	return false;
}

void app_exit()
{
	terminal_done();
	_exit(0);
}

static ma_device audio_device;
static bool audio_open = false;

static pthread_mutex_t audio_cs = PTHREAD_MUTEX_INITIALIZER;

// silence until conf fetch!
static int mo3_gain = 0; //100;
static int sfx_gain = 0; //100;

struct VOICE
{
	unsigned int id;

	int vol; // global vol at the play time

	int gain;
	int pan;
	bool loop;

	int pos;

	unsigned int fade_start;
	unsigned int fade_len;

	VOICE* next;
};

struct SAMPLE
{
	short* data;
	int size;
};

static SAMPLE sfx_buf[256]={0};
static VOICE* sfx_voice = 0;
static VOICE* sfx_fade = 0;
static pthread_mutex_t sfx_cs = PTHREAD_MUTEX_INITIALIZER;

static bool sfx_mix(VOICE* v, short* buf, int samples, int gain)
{
	int size = sfx_buf[v->id].size;
	short* data = sfx_buf[v->id].data;
	int vol = v->gain*v->vol*gain/10000;

	if (v->loop)
	{
		for (int i=0; i<samples; i++)
		{
			// todo: SATURATE to +/- 32767 !!!
			buf[2*i+0] += vol*data[v->pos]/100;
			buf[2*i+1] += vol*data[v->pos]/100;

			v->pos++;
			if (v->pos>=size)
				v->pos-=size;
		}
		return true;
	}

	for (int i=0; i<samples; i++)
	{
		// todo: SATURATE to +/- 32767 !!!
		buf[2*i+0] += vol*data[v->pos]/100;
		buf[2*i+1] += vol*data[v->pos]/100;

		v->pos++;
		if (v->pos>=size)
			return false;
	}

	return true;
}

static void audio_data_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
	const int nch=2;
	const int frq=44100;
	const int smp=frq/4; // 0.25s buf

	short* pull = (short*)out;

	int samples = frames;

	pthread_mutex_lock(&sfx_cs);
	int mo3_vol = mo3_gain;

	unsigned int fade = get_time();

	while (samples)
	{
		int s = samples;
		if (s>smp)
			s=smp;

		pull_sound(nch,frq,pull,s);

		// attenuate
		for (int i=0; i<2*s; i++)
			pull[i] = pull[i] * mo3_vol / 100;

		// mix-in sfx!
		VOICE** p = &sfx_voice;
		VOICE* v = sfx_voice;
		while (v)
		{
			VOICE* n = v->next;
			if (sfx_mix(v,pull,s,100))
			{
				p=&v->next;
			}
			else
			{
				*p=n;
				delete v;
			}
			v=n;
		}

		p=&sfx_fade;
		v = sfx_fade;
		while (v)
		{
			VOICE* n = v->next;
			// calc fade val
			unsigned int dt = fade - v->fade_start;
			if (dt>=v->fade_len)
			{
				*p=n;
				delete v;
			}
			else
			{
				int gain = v->gain*v->vol/100 - dt*100 / v->fade_len;
				if (sfx_mix(v,pull,s,gain))
				{
					p=&v->next;
				}
				else
				{
					*p=n;
					delete v;
				}
			}

			v=n;
		}

		pull+=nch*s;
		samples-=s;
	}

	pthread_mutex_unlock(&sfx_cs);
}

// miniaudio picks the system's sound (PulseAudio, PipeWire, ALSA, Core Audio);
// without one the game runs silent
void init_sound()
{
	ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
	cfg.playback.format = ma_format_s16;
	cfg.playback.channels = 2;
	cfg.sampleRate = 44100;
	cfg.periodSizeInMilliseconds = 20;
	cfg.dataCallback = audio_data_cb;

	if (ma_device_init(0, &cfg, &audio_device) != MA_SUCCESS)
		return;

	load_player();

	unsigned int id[96];
	int count = get_sfx_ids(id);

	for (int i=0; i<count; i++)
	{
		int size=0;
		short* data = get_sfx_sample(id[i],&size);
		sfx_buf[ id[i] ].data = data;
		sfx_buf[ id[i] ].size = size;
	}

	if (ma_device_start(&audio_device) != MA_SUCCESS)
	{
		ma_device_uninit(&audio_device);
		return;
	}

	audio_open = true;
}

void free_sound()
{
	if (audio_open)
	{
		ma_device_uninit(&audio_device);
		audio_open = false;

		stop_sfx(-1);
		sfx_voice = sfx_fade;
		stop_sfx(-1);
	}
}

void set_gain(int mo3, int sfx)
{
	// todo:
	// we should use interlocked swaps

	if (mo3>=0)
	{
		mo3_gain = mo3;
	}
	if (sfx>=0)
	{
		sfx_gain = sfx;
	}
}

void lock_player()
{
	pthread_mutex_lock(&audio_cs);
}

void unlock_player()
{
	pthread_mutex_unlock(&audio_cs);
}


bool play_sfx(unsigned int id, void** voice, bool loop, int vol, int pan)
{
	if (!audio_open)
		return false;

	pthread_mutex_lock(&sfx_cs);

	if (sfx_buf[id].data)
	{
		VOICE* v = new VOICE;

		v->id = id; // data & size is @ sfx_buf[v->id]
		v->next = sfx_voice;

		v->vol  = sfx_gain;
		v->gain = vol;
		v->pan  = 0;
		v->loop = loop;
		v->pos  = 0;

		sfx_voice = v;

		if (voice)
			*voice = v;

		pthread_mutex_unlock(&sfx_cs);
		return true;
	}

	pthread_mutex_unlock(&sfx_cs);
	return false;
}

bool stop_sfx(int fade)
{
	pthread_mutex_lock(&sfx_cs);
	if (fade<=0)
	{
		// just kill all
		VOICE* v = sfx_voice;
		while (v)
		{
			VOICE* n = v->next;
			delete v;
			v=n;
		}
		sfx_voice = 0;

		// kill all current fades
		v = sfx_fade;
		while (v)
		{
			VOICE* n = v->next;
			delete v;
			v=n;
		}
		sfx_fade = 0;
	}
	else
	{
		unsigned int fade_begin = get_time();
		// append all to fade list and assign timers

		VOICE* v = sfx_fade;
		unsigned int end = fade_begin + fade;
		while (v)
		{
			unsigned int e = v->fade_start + v->fade_len;
			if ( (int)(e-end) > 0 )
			{
				int delta = (fade*v->fade_len)/(fade_begin - v->fade_start) - fade;
				v->fade_start = fade_begin - delta;
				v->fade_len = fade + delta;
			}

			v=v->next;
		}

		v = sfx_voice;
		while (v)
		{
			VOICE* n = v->next;

			v->fade_start = fade_begin;
			v->fade_len = fade;

			v->next = sfx_fade;
			sfx_fade = v;

			v=n;
		}
		sfx_voice = 0;
	}

	pthread_mutex_unlock(&sfx_cs);
	return true;
}

bool stop_sfx(void* voice, int fade)
{
	pthread_mutex_lock(&sfx_cs);
	if (fade<=0)
	{
		VOICE** p = &sfx_voice;
		VOICE* v = sfx_voice;

		while (v)
		{
			VOICE* n = v->next;

			if (voice == (void*)v)
			{
				*p = n;
				delete v;

				pthread_mutex_unlock(&sfx_cs);
				return true;
			}

			p=&v->next;
			v=n;
		}
	}
	else
	{
		unsigned int fade_begin = get_time();
		// append all to fade list and assign timers

		VOICE** p = &sfx_voice;
		VOICE* v = sfx_voice;

		while (v)
		{
			VOICE* n = v->next;

			if (voice == (void*)v)
			{
				*p = n;
				break;
			}

			p=&v->next;
			v=n;
		}

		if (v)
		{
			v->fade_start = fade_begin;
			v->fade_len = fade;
			v->next = sfx_fade;
			sfx_fade = v;

			pthread_mutex_unlock(&sfx_cs);
			return true;
		}
	}

	pthread_mutex_unlock(&sfx_cs);
	return false;
}

bool set_sfx_params(void* voice, int vol, int pan)
{
	pthread_mutex_lock(&sfx_cs);

	VOICE* v = sfx_voice;

	while (v)
	{
		if (voice == (void*)v)
		{
			v->gain = vol;
			v->pan = pan;

			pthread_mutex_unlock(&sfx_cs);

			return true;
		}

		v=v->next;
	}

	pthread_mutex_unlock(&sfx_cs);
	return false;
}

#endif
