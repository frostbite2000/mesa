# progs/xdemos/Makefile

TOP = ../..
include $(TOP)/configs/current


INCDIR = $(TOP)/include

LIB_DEP = $(TOP)/$(LIB_DIR)/$(GL_LIB_NAME)

LIBS = -L$(TOP)/$(LIB_DIR) -l$(GL_LIB) $(APP_LIB_DEPS)

PROGS = \
	corender \
	glsync \
	glthreads \
	glxdemo \
	glxgears \
	glxgears_fbconfig \
	glxgears_pixmap \
	glxcontexts \
	glxheads \
	glxinfo \
	glxpixmap \
	glxpbdemo \
	glxsnoop \
	glxswapcontrol \
	manywin \
	offset \
	overlay \
	pbinfo \
	pbdemo \
	sharedtex \
	texture_from_pixmap \
	wincopy \
	xfont \
	xrotfontdemo

# Don't build these by default because of extra library dependencies
EXTRA_PROGS = \
	shape \
	yuvrect_client \
	xdemo



##### RULES #####

.SUFFIXES:
.SUFFIXES: .c

.c: $(LIB_DEP)
	$(CC) -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) $(LDFLAGS) $< $(LIBS) -o $@


##### TARGETS #####

default: $(PROGS)


extra: $(EXTRA_PROGS)


clean:
	-rm -f $(PROGS) $(EXTRA_PROGS)
	-rm -f *.o *~


# special cases
pbinfo: pbinfo.o pbutil.o
	$(CC) $(CFLAGS) $(LDFLAGS) pbinfo.o pbutil.o $(LIBS) -o $@

pbdemo: pbdemo.o pbutil.o
	$(CC) $(CFLAGS) $(LDFLAGS) pbdemo.o pbutil.o $(LIBS) -o $@

pbinfo.o: pbinfo.c pbutil.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) pbinfo.c

pbdemo.o: pbdemo.c pbutil.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) pbdemo.c

pbutil.o: pbutil.c pbutil.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) pbutil.c

glxgears_fbconfig: glxgears_fbconfig.o pbutil.o
	$(CC) $(CFLAGS) $(LDFLAGS) glxgears_fbconfig.o pbutil.o $(LIBS) -o $@

glxgears_fbconfig.o: glxgears_fbconfig.c pbutil.h
	$(CC) -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) -c -I. $(CFLAGS) glxgears_fbconfig.c

xrotfontdemo: xrotfontdemo.o xuserotfont.o
	$(CC) $(CFLAGS) $(LDFLAGS) xrotfontdemo.o xuserotfont.o $(LIBS) -o $@

xuserotfont.o: xuserotfont.c xuserotfont.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) xuserotfont.c

xrotfontdemo.o: xrotfontdemo.c xuserotfont.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) xrotfontdemo.c

corender: corender.o ipc.o
	$(CC) $(CFLAGS) corender.o ipc.o $(LIBS) -o $@

corender.o: corender.c ipc.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) corender.c

ipc.o: ipc.c ipc.h
	$(CC) -c -I. -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) ipc.c

yuvrect_client: yuvrect_client.c
	$(CC) -I$(INCDIR) $(X11_INCLUDES) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -l$(GLU_LIB) -o $@

