# $Id: Makefile.in,v 1.139 2025/10/18 13:33:46 tom Exp $
##############################################################################
# Copyright 2018-2024,2025 Thomas E. Dickey                                  #
# Copyright 1998-2015,2016 Free Software Foundation, Inc.                    #
#                                                                            #
# Permission is hereby granted, free of charge, to any person obtaining a    #
# copy of this software and associated documentation files (the "Software"), #
# to deal in the Software without restriction, including without limitation  #
# the rights to use, copy, modify, merge, publish, distribute, distribute    #
# with modifications, sublicense, and/or sell copies of the Software, and to #
# permit persons to whom the Software is furnished to do so, subject to the  #
# following conditions:                                                      #
#                                                                            #
# The above copyright notice and this permission notice shall be included in #
# all copies or substantial portions of the Software.                        #
#                                                                            #
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR #
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   #
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    #
# THE ABOVE COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER      #
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    #
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        #
# DEALINGS IN THE SOFTWARE.                                                  #
#                                                                            #
# Except as contained in this notice, the name(s) of the above copyright     #
# holders shall not be used in advertising or otherwise to promote the sale, #
# use or other dealings in this Software without prior written               #
# authorization.                                                             #
##############################################################################
#
# Author: Thomas E. Dickey
#
#  Simple makefile for c++ window class demo

# turn off _all_ suffix rules; we'll generate our own
.SUFFIXES:

SHELL		= /bin/sh





x		= 
o		= .o

MODEL		= ../obj_s
top_srcdir	= ..
srcdir		= .
prefix		= /home/alxd/narnia/termai/vendor/ncurses-src/../ncurses-install
exec_prefix	= ${prefix}
bindir		= ${exec_prefix}/bin
libdir		= ${exec_prefix}/lib
includedir	= ${prefix}/include
includesubdir	= /ncursesw

INCLUDEDIR	= $(DESTDIR)$(includedir)$(includesubdir)
BINDIR		= $(DESTDIR)$(bindir)
LIBDIR		= $(DESTDIR)$(libdir)

PACKAGE		= ncursesw

LIBTOOL		= 
LIBTOOL_OPTS	=  
LIBTOOL_CLEAN	= 
LIBTOOL_COMPILE	= 
LIBTOOL_LINK	= ${CC}
LIBTOOL_INSTALL	= 
LIBTOOL_UNINSTALL = 
LIBTOOL_VERSION = -version-number
LT_UNDEF	= 

INSTALL		= /usr/bin/install -c
INSTALL_LIB	= $(INSTALL) -m 644
INSTALL_DATA	= ${INSTALL} -m 644
INSTALL_OPT_P	= 

AR		= ar
ARFLAGS		= -curvU
AWK		= gawk
LD		= ld
LN_S		= ln -s -f

CXX_AR		= $(AR)
CXX_ARFLAGS	= $(ARFLAGS)
RANLIB		= ranlib

CXX		= /usr/bin/g++
CPP		= /usr/bin/g++ -E
CXXFLAGS	= -O2 
CXXLIBS		= 

INCDIR		= ../include
CPPFLAGS	= -DHAVE_CONFIG_H -DBUILDING_NCURSES_CXX -I../c++ -I. -I../include -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 -DNDEBUG

CTAGS		= 
ETAGS		= 

CC		= ${CXX}
CCFLAGS		= $(CPPFLAGS) $(CXXFLAGS)

CFLAGS_LIBTOOL	= $(CCFLAGS)
CFLAGS_NORMAL	= $(CCFLAGS) -DNCURSES_STATIC
CFLAGS_DEBUG	= $(CCFLAGS) -DNCURSES_STATIC -g -DTRACE
CFLAGS_PROFILE	= $(CCFLAGS) -DNCURSES_STATIC -pg
CFLAGS_SHARED	= $(CCFLAGS) -fPIC

CFLAGS_DEFAULT	= $(CFLAGS_SHARED)

NCURSES_MAJOR	= 6
NCURSES_MINOR	= 6
REL_VERSION     = 6.6
ABI_VERSION	= 6

LOCAL_LIBDIR	= /home/alxd/narnia/termai/vendor/ncurses-src/lib

LINK		=  $(LIBTOOL_LINK) 
SHLIB_DIRS	= -L../lib
SHLIB_LIST	= $(SHLIB_DIRS) \
		-lformw \
		-lmenuw \
		-lpanelw \
		-lncursesw -ldl  -ltinfow

LIBROOT		= ncurses++

LIBNAME_LIBTOOL	= lib$(LIBROOT)w.la
LIBNAME_NORMAL	= lib$(LIBROOT)w.a
LIBNAME		= lib$(LIBROOT)w.a

LIBRARIES	=  ../lib/libncurses++w.a ../lib/libncurses++w_g.a

LINT		= lint
LINT_OPTS	= -chapbrxzgFS -v -Ac11
LINT_LIBS	= -lncurses 

LINK_FLAGS	=  -L../lib -l$(LIBROOT)w
RPATH_LIST	= ${libdir}
MK_SHARED_LIB	= ${CC} ${LDFLAGS} ${CFLAGS} -shared -Wl,-soname,`basename $@ .${REL_VERSION}`.${ABI_VERSION},-stats,-lc -o $@

LINK_LIBTOOL	=  -L../lib $(LIBRARIES)
LINK_NORMAL	= $(LINK_FLAGS)
LINK_DEBUG	= $(LINK_FLAGS)
LINK_PROFILE	= $(LINK_FLAGS)
LINK_SHARED	= $(LINK_FLAGS)

# flags for test-programs
TEST_LIBS	= -lutil 
TEST_ARGS	=  -L../lib -lformw -lmenuw -lpanelw -lncursesw -ltinfow 
TEST_LDFLAGS	= $(TEST_ARGS)  \
	 $(TEST_LIBS)   $(CXXLIBS)

LDFLAGS_LIBTOOL	= $(TEST_LDFLAGS) $(CFLAGS_LIBTOOL)
LDFLAGS_NORMAL	= $(TEST_LDFLAGS) $(CFLAGS_NORMAL)
LDFLAGS_DEBUG	= $(TEST_LDFLAGS) $(CFLAGS_DEBUG)
LDFLAGS_PROFILE	= $(TEST_LDFLAGS) $(CFLAGS_PROFILE)
LDFLAGS_SHARED	= $(TEST_LDFLAGS) $(CFLAGS_SHARED) 

LDFLAGS_DEFAULT	= $(LINK_SHARED) $(LDFLAGS_SHARED)

# flags for library built by this makefile
LDFLAGS		= $(TEST_ARGS)  \
	 $(TEST_LIBS)  $(CXXLIBS)

AUTO_SRC	= \
		etip.h

################################################################################

.PHONY :	all
.PHONY :	check
.PHONY :	clean
.PHONY :	depend
.PHONY :	distclean
.PHONY :	install
.PHONY :	install.libs
.PHONY :	libs
.PHONY :	mostlyclean
.PHONY :	realclean
.PHONY :	sources
.PHONY :	uninstall
.PHONY :	uninstall.libs

all \
libs ::		$(AUTO_SRC) $(LIBRARIES)

#all ::		demo$x

depend :

sources :	$(AUTO_SRC)

tags:
	$(CTAGS) *.[h] *.cc

$(LIBDIR) :
	mkdir -p $@

mostlyclean ::
	-rm -f core tags TAGS *~ *.bak *.i *.ii *.ln *.atac trace

clean :: mostlyclean
	-$(SHELL) -c "if test -n '$x' ; then $(MAKE) clean x=''; fi"
	-rm -rf $(MODEL)/SunWS_cache
	-$(LIBTOOL_CLEAN) rm -f demo$x $(AUTO_SRC) $(LIBRARIES) $(OBJS_DEMO)
	-rm -rf .libs *.dSYM

distclean :: clean
	-rm -f Makefile

realclean :: distclean

###############################################################################

HEADER_DEPS	=  \
	etip.h \
	../include/curses.h \
	../include/eti.h \
	../include/form.h \
	../include/menu.h \
	../include/ncurses_cfg.h \
	../include/ncurses_def.h \
	../include/ncurses_dll.h \
	../include/panel.h \
	../include/unctrl.h \
	$(INCDIR)/nc_string.h \
	$(srcdir)/cursesp.h \
	$(srcdir)/cursesw.h \
	$(srcdir)/cursslk.h \
	$(srcdir)/internal.h

cursesw_h	= $(srcdir)/cursesw.h \
		  $(HEADER_DEPS)

cursesp_h	= $(srcdir)/cursesp.h \
		  $(cursesw_h)

cursesf_h	= $(srcdir)/cursesf.h \
		  $(cursesp_h)

cursesm_h	= $(srcdir)/cursesm.h \
		  $(cursesp_h)

cursslk_h	= $(srcdir)/cursslk.h \
		  $(cursesw_h)

cursesapp_h	= $(srcdir)/cursesapp.h \
		  $(cursslk_h)

$(INCDIR)/form.h :
	( cd ../form && $(MAKE) $@ )

$(INCDIR)/menu.h :
	( cd ../menu && $(MAKE) $@ )

$(INCDIR)/panel.h :
	( cd ../panel && $(MAKE) $@ )

###############################################################################

OBJS_DEMO = $(MODEL)/demo$o

$(MODEL)/demo$o : $(srcdir)/demo.cc \
		$(HEADER_DEPS) \
		$(cursesf_h) \
		$(cursesm_h) \
		$(cursesapp_h)
	@echo 'compiling demo (obj_s)'
	@( cd $(MODEL) && $(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEFAULT) -c $(top_srcdir)/c++/demo.cc -o $@ )

demo$x:	$(OBJS_DEMO) \
	$(LIBRARIES)  \
	../lib/libformw.so ../lib/libmenuw.so ../lib/libpanelw.so ../lib/libncursesw.so ../lib/libtinfow.so
	 $(LINK) -o $@ $(OBJS_DEMO) $(LDFLAGS_DEFAULT)

etip.h:	$(srcdir)/etip.h.in $(srcdir)/edit_cfg.sh
	cp $(srcdir)/etip.h.in $@
	$(SHELL) $(srcdir)/edit_cfg.sh ../include/ncurses_cfg.h $@

# Verify that each header-file can be compiled without including another.
check ::
	@$(SHELL) -c "for header in *.h;\
		do \
			[ \$${header} = etip.h ] && continue; \
			echo \"** testing \$${header}\" ; \
			echo \"#include <\$${header}>\" >headers.cc; \
			echo \"int main(int argc, char **argv) { (void) argc; (void) argv; return 0; }\" >>headers.cc; \
			$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) headers.cc; \
		done"
	-@rm -f headers.*

###############################################################################
# The remainder of this file is automatically generated during configuration
###############################################################################

# Generated by CF_LIB_RULES
resulting.map: 
	gawk 'BEGIN { skip = 1; last=""; } /deprecated in ABI6/ { next; }{ sub("NCURSES([WT]+)?", "&6"); } { if ( last != "" && ( skip == 0 || $$0 !~ /}/ ) ) { print last; } skip = 0; last = $$0; } END { print last; }' < "" >$@

distclean::
	rm -f resulting.map

# generated by mk-0th.awk
#   libname:    c++w
#   subsets:    ticlib+termlib+ext_tinfo base+widechar+ext_funcs

.SUFFIXES: .c .cc .h .i .ii
.c.i :
	$(CPP) $(CPPFLAGS) $< >$@
.cc.ii :
	$(CPP) $(CPPFLAGS) $< >$@
.h.i :
	$(CPP) $(CPPFLAGS) $< >$@

C_SRC = \
	$(srcdir)/cursesf.cc \
	$(srcdir)/cursesm.cc \
	$(srcdir)/cursesw.cc \
	$(srcdir)/cursespad.cc \
	$(srcdir)/cursesp.cc \
	$(srcdir)/cursslk.cc \
	$(srcdir)/cursesapp.cc \
	$(srcdir)/cursesmain.cc

clean ::
	rm -f llib-lc++w.*

realclean ::
	rm -f llib-lc++w

llib-lc++w : $(C_SRC)
	cproto -a -l -DNCURSES_ENABLE_STDBOOL_H=0 -DLINT $(CPPFLAGS) $(C_SRC) >$@

lintlib ::
	sh $(srcdir)/../misc/makellib c++w $(CPPFLAGS)

lint ::
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(C_SRC) $(LINT_LIBS)





# overriding model from SHARED to match CXX_MODEL

# generated by mk-1st.awk (subset=base+widechar+ext_funcs)
#  name:            ncurses++w
#  traces:          DEBUG
#  MODEL:           NORMAL
#  CXX_MODEL:       NORMAL
#  LIB_SUFFIX:      w
#  ABI_SUFFIX:      w
#  model:           obj_s
#  prefix:          lib
#  suffix:          .a
#  subset:          base+widechar+ext_funcs
#  driver:          no
#  ShlibVer:        rel
#  ShlibVerInfix:   no
#  SymLink:         ln -s -f
#  TermlibRoot:     tinfow
#  TermlibSuffix:   .so
#  ReLink:          yes
#  ReRanlib:        yes
#  DoLinks:         no
#  rmSoLocs:        no
#  ldconfig:        /usr/bin/ldconfig
#  make_phony:      yes
#  overwrite:       no
#  depend:          ../include/ncurses_cfg.h
#  host:            x86_64-pc-linux-gnu
#  libtool_version: -version-number

NORMAL_OBJS = \
	../obj_s/cursesf$o \
	../obj_s/cursesm$o \
	../obj_s/cursesw$o \
	../obj_s/cursespad$o \
	../obj_s/cursesp$o \
	../obj_s/cursslk$o \
	../obj_s/cursesapp$o \
	../obj_s/cursesmain$o

$(NORMAL_OBJS) : ../include/ncurses_cfg.h

../lib/libncurses++w.a : $(NORMAL_OBJS)
	$(CXX_AR) $(CXX_ARFLAGS) $@ $?
	$(RANLIB) $@

install \
install.libs \
install.ncurses++w :: $(LIBDIR) ../lib/libncurses++w.a
	@echo installing ../lib/libncurses++w.a as $(LIBDIR)/libncurses++w.a
	$(INSTALL_DATA) ../lib/libncurses++w.a $(LIBDIR)/libncurses++w.a
	$(RANLIB) $(LIBDIR)/libncurses++w.a

uninstall \
uninstall.libs \
uninstall.ncurses++w ::
	@echo uninstalling $(LIBDIR)/libncurses++w.a
	-@rm -f $(LIBDIR)/libncurses++w.a

clean ::
	-rm -f ../lib/libncurses++w.a

mostlyclean::
	-rm -f $(NORMAL_OBJS)

# generated by mk-2nd.awk
#   model:      obj_s
#   MODEL:      SHARED
#   echo:       yes
#   subset:     base+widechar+ext_funcs
#   crenames:   yes
#   cxxrenames: yes
#   traces:     DEBUG
#   srcdir:     .

../obj_s/cursesf$o :	$(srcdir)/cursesf.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesf_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesf.cc -o ../obj_s/cursesf$o

../obj_s/cursesm$o :	$(srcdir)/cursesm.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesm_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesm.cc -o ../obj_s/cursesm$o

../obj_s/cursesw$o :	$(srcdir)/cursesw.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesw_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesw.cc -o ../obj_s/cursesw$o

../obj_s/cursespad$o :	$(srcdir)/cursespad.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesw_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursespad.cc -o ../obj_s/cursespad$o

../obj_s/cursesp$o :	$(srcdir)/cursesp.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesp.cc -o ../obj_s/cursesp$o

../obj_s/cursslk$o :	$(srcdir)/cursslk.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursslk.cc -o ../obj_s/cursslk$o

../obj_s/cursesapp$o :	$(srcdir)/cursesapp.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesapp.cc -o ../obj_s/cursesapp$o

../obj_s/cursesmain$o :	$(srcdir)/cursesmain.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h) \
			$(INCDIR)/nc_alloc.h
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_SHARED) -c ../c++/cursesmain.cc -o ../obj_s/cursesmain$o


# generated by mk-1st.awk (subset=base+widechar+ext_funcs)
#  name:            ncurses++w
#  traces:          DEBUG
#  MODEL:           DEBUG
#  CXX_MODEL:       DEBUG
#  LIB_SUFFIX:      w
#  ABI_SUFFIX:      w
#  model:           obj_g
#  prefix:          lib
#  suffix:          _g.a
#  subset:          base+widechar+ext_funcs
#  driver:          no
#  ShlibVer:        rel
#  ShlibVerInfix:   no
#  SymLink:         ln -s -f
#  TermlibRoot:     tinfow
#  TermlibSuffix:   .so
#  ReLink:          yes
#  ReRanlib:        yes
#  DoLinks:         yes
#  rmSoLocs:        no
#  ldconfig:        /usr/bin/ldconfig
#  make_phony:      yes
#  overwrite:       no
#  depend:          ../include/ncurses_cfg.h
#  host:            x86_64-pc-linux-gnu
#  libtool_version: -version-number

DEBUG_OBJS = \
	../obj_g/cursesf$o \
	../obj_g/cursesm$o \
	../obj_g/cursesw$o \
	../obj_g/cursespad$o \
	../obj_g/cursesp$o \
	../obj_g/cursslk$o \
	../obj_g/cursesapp$o \
	../obj_g/cursesmain$o

$(DEBUG_OBJS) : ../include/ncurses_cfg.h

../lib/libncurses++w_g.a : $(DEBUG_OBJS)
	$(CXX_AR) $(CXX_ARFLAGS) $@ $?
	$(RANLIB) $@

install \
install.libs \
install.ncurses++w :: $(LIBDIR) ../lib/libncurses++w_g.a
	@echo installing ../lib/libncurses++w_g.a as $(LIBDIR)/libncurses++w_g.a
	$(INSTALL_DATA) ../lib/libncurses++w_g.a $(LIBDIR)/libncurses++w_g.a
	$(RANLIB) $(LIBDIR)/libncurses++w_g.a

uninstall \
uninstall.libs \
uninstall.ncurses++w ::
	@echo uninstalling $(LIBDIR)/libncurses++w_g.a
	-@rm -f $(LIBDIR)/libncurses++w_g.a

clean ::
	-rm -f ../lib/libncurses++w_g.a

mostlyclean::
	-rm -f $(DEBUG_OBJS)

# generated by mk-2nd.awk
#   model:      obj_g
#   MODEL:      DEBUG
#   echo:       yes
#   subset:     base+widechar+ext_funcs
#   crenames:   yes
#   cxxrenames: yes
#   traces:     DEBUG
#   srcdir:     .

../obj_g/cursesf$o :	$(srcdir)/cursesf.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesf_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesf.cc -o ../obj_g/cursesf$o

../obj_g/cursesm$o :	$(srcdir)/cursesm.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesm_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesm.cc -o ../obj_g/cursesm$o

../obj_g/cursesw$o :	$(srcdir)/cursesw.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesw_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesw.cc -o ../obj_g/cursesw$o

../obj_g/cursespad$o :	$(srcdir)/cursespad.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesw_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursespad.cc -o ../obj_g/cursespad$o

../obj_g/cursesp$o :	$(srcdir)/cursesp.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesp.cc -o ../obj_g/cursesp$o

../obj_g/cursslk$o :	$(srcdir)/cursslk.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursslk.cc -o ../obj_g/cursslk$o

../obj_g/cursesapp$o :	$(srcdir)/cursesapp.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h)
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesapp.cc -o ../obj_g/cursesapp$o

../obj_g/cursesmain$o :	$(srcdir)/cursesmain.cc \
			$(HEADER_DEPS) \
			$(internal_h) \
			$(cursesapp_h) \
			$(INCDIR)/nc_alloc.h
	$(LIBTOOL_COMPILE) $(CXX) $(CFLAGS_DEBUG) -c ../c++/cursesmain.cc -o ../obj_g/cursesmain$o


# generated by mk-hdr.awk
#  subset:     ticlib+termlib+ext_tinfo base+widechar+ext_funcs
#  compat:     yes

${INCLUDEDIR} :
	mkdir -p $@

install \
install.libs \
install.includes :: ${AUTO_SRC} ${INCLUDEDIR} \
		$(srcdir)/cursesapp.h \
		$(srcdir)/cursesf.h \
		$(srcdir)/cursesm.h \
		$(srcdir)/cursesp.h \
		$(srcdir)/cursesw.h \
		$(srcdir)/cursslk.h \
		etip.h
	@ (cd ${INCLUDEDIR} && rm -f cursesapp.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursesapp.h
	@ (cd ${INCLUDEDIR} && rm -f cursesf.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursesf.h
	@ (cd ${INCLUDEDIR} && rm -f cursesm.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursesm.h
	@ (cd ${INCLUDEDIR} && rm -f cursesp.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursesp.h
	@ (cd ${INCLUDEDIR} && rm -f cursesw.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursesw.h
	@ (cd ${INCLUDEDIR} && rm -f cursslk.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} $(srcdir)/cursslk.h
	@ (cd ${INCLUDEDIR} && rm -f etip.h) ; ../headers.sh ${INSTALL_DATA} ${INCLUDEDIR} ${srcdir} etip.h

uninstall \
uninstall.libs \
uninstall.includes ::
	-@ (cd ${INCLUDEDIR} && rm -f cursesapp.h)
	-@ (cd ${INCLUDEDIR} && rm -f cursesf.h)
	-@ (cd ${INCLUDEDIR} && rm -f cursesm.h)
	-@ (cd ${INCLUDEDIR} && rm -f cursesp.h)
	-@ (cd ${INCLUDEDIR} && rm -f cursesw.h)
	-@ (cd ${INCLUDEDIR} && rm -f cursslk.h)
	-@ (cd ${INCLUDEDIR} && rm -f etip.h)
