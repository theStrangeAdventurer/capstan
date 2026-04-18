# $Id: Makefile.in,v 1.121 2025/10/18 13:34:20 tom Exp $
##############################################################################
# Copyright 2020-2024,2025 Thomas E. Dickey                                  #
# Copyright 1998-2016,2018 Free Software Foundation, Inc.                    #
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
# Author: Thomas E. Dickey 1996-on
#
# Makefile for ncurses source code.
#
# This makes the ncurses utility programs.
#
# The variable 'srcdir' refers to the source-distribution, and can be set with
# the configure script by "--srcdir=DIR".
#
# The rules are organized to produce the libraries for the configured models,
# and the programs with the configured default model.

# turn off _all_ suffix rules; we'll generate our own
.SUFFIXES:

SHELL		= /bin/sh

THIS		= Makefile



TOP_MFLAGS	= DESTDIR="$(DESTDIR)" RPATH_LIST="$(RPATH_LIST)"

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
datarootdir	= ${prefix}/share
datadir		= ${datarootdir}
includesubdir	= /ncursesw

INCLUDEDIR	= $(DESTDIR)$(includedir)$(includesubdir)
BINDIR		= $(DESTDIR)$(bindir)

PACKAGE		= ncursesw

LIBTOOL		= 
LIBTOOL_OPTS	= 
LIBTOOL_CLEAN	= 
LIBTOOL_COMPILE	= 
LIBTOOL_LINK	= ${CC}
LIBTOOL_INSTALL	= 
LIBTOOL_UNINSTALL = 

INSTALL		= /usr/bin/install -c
INSTALL_PROG	= ${INSTALL} -s
transform	= s,x,x,

AWK		= gawk
LN_S		= ln -s -f

CTAGS		= 
ETAGS		= 

CC		= gcc
CPP		= gcc -E
CFLAGS		= -O2 --param max-inline-insns-single=1200 

INCDIR		= $(top_srcdir)/include
NCUDIR		= $(top_srcdir)/ncurses
CPPFLAGS	= -DHAVE_CONFIG_H -DACTUAL_TIC="\"$(actual_tic)\"" -I../progs -I../ncurses -I$(top_srcdir)/ncurses -I. -I../include -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 -DNDEBUG

CCFLAGS		= $(CPPFLAGS) $(CFLAGS)

CFLAGS_LIBTOOL	= $(CCFLAGS)
CFLAGS_NORMAL	= $(CCFLAGS) -DNCURSES_STATIC
CFLAGS_DEBUG	= $(CCFLAGS) -DNCURSES_STATIC -g -DTRACE
CFLAGS_PROFILE	= $(CCFLAGS) -DNCURSES_STATIC -pg
CFLAGS_SHARED	= $(CCFLAGS) -fPIC

CFLAGS_DEFAULT	= $(CFLAGS_SHARED)

REL_VERSION	= 6.6
ABI_VERSION	= 6
LOCAL_LIBDIR	= /home/alxd/narnia/termai/vendor/ncurses-src/lib

LD		= ld
LINK		=  $(LIBTOOL_LINK)
LDFLAGS		=  
RPATH_LIST	= ${libdir}

LDFLAGS_LIBTOOL	= $(LDFLAGS) $(CFLAGS_LIBTOOL)
LDFLAGS_NORMAL	= $(LDFLAGS) $(CFLAGS_NORMAL)
LDFLAGS_DEBUG	= $(LDFLAGS) $(CFLAGS_DEBUG)
LDFLAGS_PROFILE	= $(LDFLAGS) $(CFLAGS_PROFILE)
LDFLAGS_SHARED	= $(LDFLAGS) $(CFLAGS_SHARED) 

LDFLAGS_DEFAULT	= $(LDFLAGS_SHARED)

LIBS_TIC	= -L../lib  -lncursesw -ltinfow   
LDFLAGS_TIC	= -L../lib $(LDFLAGS_SHARED) $(LIBS_TIC)

LIBS_TINFO	=  -ltinfow   
LDFLAGS_TINFO	= -L../lib $(LDFLAGS_SHARED) $(LIBS_TINFO)

LINT		= lint
LINT_OPTS	= -chapbrxzgFS -v -Ac11
LINT_LIBS	= -lncurses 

AUTO_SRC = \
	termsort.h \
	transform.h

# tic relies on direct access to the terminfo database
GET_PROGS = infocmp$x clear$x tabs$x tput$x tset$x toe$x
PUT_PROGS =  tic$x
PROGS = $(PUT_PROGS) $(GET_PROGS)

# Default library, for linking applications
DEPS_CURSES = ../lib/libncursesw.so

HEADER_DEPS	= \
	$(INCDIR)/nc_access.h \
	$(INCDIR)/nc_alloc.h \
	$(INCDIR)/nc_panel.h \
	$(INCDIR)/nc_string.h \
	$(INCDIR)/nc_termios.h \
	$(INCDIR)/nc_tparm.h \
	$(INCDIR)/term_entry.h \
	$(INCDIR)/tic.h \
	$(NCUDIR)/curses.priv.h \
	$(NCUDIR)/new_pair.h \
	$(NCUDIR)/term.priv.h \
	$(srcdir)/progs.priv.h \
	../include/curses.h \
	../include/ncurses_cfg.h \
	../include/ncurses_def.h \
	../include/ncurses_dll.h \
	../include/term.h \
	../include/unctrl.h

################################################################################

.PHONY :	all
.PHONY :	check
.PHONY :	clean
.PHONY :	distclean
.PHONY :	install
.PHONY :	install.libs
.PHONY :	install.progs
.PHONY :	libs
.PHONY :	lint
.PHONY :	mostlyclean
.PHONY :	realclean
.PHONY :	sources
.PHONY :	uninstall
.PHONY :	uninstall.libs
.PHONY :	uninstall.progs

all:		$(AUTO_SRC) $(PROGS)

sources:	$(AUTO_SRC)

install: 	$(AUTO_SRC) install.progs
uninstall: uninstall.progs

# this line simplifies the configure-script
libs \
install.libs \
uninstall.libs:

TRANSFORM = sed 's/$x$$//'|sed '$(transform)'|sed 's/$$/$x/'

# transformed names for installing files
actual_captoinfo = `echo captoinfo$x| $(TRANSFORM)`
actual_clear     = `echo clear$x|     $(TRANSFORM)`
actual_infocmp   = `echo infocmp$x|   $(TRANSFORM)`
actual_infotocap = `echo infotocap$x| $(TRANSFORM)`
actual_init      = `echo init$x|      $(TRANSFORM)`
actual_reset     = `echo reset$x|     $(TRANSFORM)`
actual_tabs      = `echo tabs$x|      $(TRANSFORM)`
actual_tic       = `echo tic$x|       $(TRANSFORM)`
actual_toe       = `echo toe$x|       $(TRANSFORM)`
actual_tput      = `echo tput$x|      $(TRANSFORM)`
actual_tset      = `echo tset$x|      $(TRANSFORM)`

# transformed names for comparing at runtime
define_captoinfo = `echo captoinfo|   $(TRANSFORM)`
define_clear     = `echo clear|       $(TRANSFORM)`
define_infotocap = `echo infotocap|   $(TRANSFORM)`
define_init      = `echo init|        $(TRANSFORM)`
define_reset     = `echo reset|       $(TRANSFORM)`

transform.h :
	echo "#ifndef __TRANSFORM_H"					>$@
	echo "#define __TRANSFORM_H 1"					>>$@
	echo "#include <progs.priv.h>"					>>$@
	echo "extern bool same_program(const char *, const char *);"	>>$@
	-$(SHELL) -c 'if test -n "$x" ; then echo "#define SUFFIX_IGNORED \"$x\"">>$@; fi'
	echo "#define PROG_CAPTOINFO \"$(define_captoinfo)\""		>>$@
	echo "#define PROG_INFOTOCAP \"$(define_infotocap)\""		>>$@
	echo "#define PROG_CLEAR     \"$(define_clear)\""		>>$@
	echo "#define PROG_RESET     \"$(define_reset)\""		>>$@
	echo "#define PROG_INIT      \"$(define_init)\""		>>$@
	echo "#endif /* __TRANSFORM_H */"				>>$@

install.progs: $(AUTO_SRC) $(PROGS) $(BINDIR)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) tic$x     $(BINDIR)/$(actual_tic)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) toe$x     $(BINDIR)/$(actual_toe)
	@echo "linking $(actual_infotocap) to $(actual_tic)"
	-@rm -f $(BINDIR)/$(actual_infotocap)
	( cd $(BINDIR) && $(LN_S) $(actual_tic) $(actual_infotocap) )
	@echo "linking $(actual_captoinfo) to $(actual_tic)"
	-@rm -f $(BINDIR)/$(actual_captoinfo)
	( cd $(BINDIR) && $(LN_S) $(actual_tic) $(actual_captoinfo) )
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) infocmp$x $(BINDIR)/$(actual_infocmp)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) clear$x   $(BINDIR)/$(actual_clear)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) tabs$x    $(BINDIR)/$(actual_tabs)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) tput$x    $(BINDIR)/$(actual_tput)
	$(LIBTOOL_INSTALL) $(INSTALL_PROG) tset$x    $(BINDIR)/$(actual_tset)
	@echo "linking $(actual_reset) to $(actual_tset)"
	-@rm -f $(BINDIR)/$(actual_reset)
	( cd $(BINDIR) && $(LN_S) $(actual_tset) $(actual_reset) )

uninstall.progs:
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_tic)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_toe)
	-@rm -f $(BINDIR)/$(actual_captoinfo)
	-@rm -f $(BINDIR)/$(actual_infotocap)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_infocmp)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_clear)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_tabs)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_tput)
	-@$(LIBTOOL_CLEAN) rm -f $(BINDIR)/$(actual_tset)
	-@rm -f $(BINDIR)/$(actual_reset)

$(BINDIR) :
	mkdir -p $@

#
# Utilities normally built by make all start here
#

DEPS_TIC = \
	$(MODEL)/tic$o \
	$(MODEL)/dump_entry$o \
	$(MODEL)/tparm_type$o \
	$(MODEL)/transform$o

tic$x: $(DEPS_TIC) $(DEPS_CURSES) transform.h
	 $(LINK) $(DEPS_TIC) $(LDFLAGS_TIC) -o $@

DEPS_TOE = \
	$(MODEL)/toe$o

toe$x: $(DEPS_TOE) $(DEPS_CURSES)
	 $(LINK) $(DEPS_TOE) $(LDFLAGS_TIC) -o $@

DEPS_CLEAR = \
	$(MODEL)/clear$o \
	$(MODEL)/clear_cmd$o \
	$(MODEL)/tty_settings$o

clear$x: $(DEPS_CLEAR) $(DEPS_CURSES)
	 $(LINK) $(DEPS_CLEAR) $(LDFLAGS_TINFO) -o $@

DEPS_TABS = \
	$(MODEL)/tabs$o \
	$(MODEL)/tty_settings$o

tabs$x: $(DEPS_TABS) $(DEPS_TABS)
	 $(LINK) $(DEPS_TABS) $(LDFLAGS_TINFO) -o $@

DEPS_TPUT = \
	$(MODEL)/tput$o \
	$(MODEL)/clear_cmd$o \
	$(MODEL)/reset_cmd$o \
	$(MODEL)/tparm_type$o \
	$(MODEL)/transform$o \
	$(MODEL)/tty_settings$o

tput$x: $(DEPS_TPUT) $(DEPS_CURSES) transform.h
	 $(LINK) $(DEPS_TPUT) $(LDFLAGS_TINFO) -o $@

DEPS_INFOCMP = \
	$(MODEL)/infocmp$o \
	$(MODEL)/dump_entry$o

infocmp$x: $(DEPS_INFOCMP) $(DEPS_CURSES)
	 $(LINK) $(DEPS_INFOCMP) $(LDFLAGS_TIC) -o $@

DEPS_TSET = \
	$(MODEL)/tset$o \
	$(MODEL)/reset_cmd$o \
	$(MODEL)/transform$o \
	$(MODEL)/tty_settings$o

tset$x: $(DEPS_TSET) $(DEPS_CURSES) transform.h
	 $(LINK) $(DEPS_TSET) $(LDFLAGS_TINFO) -o $@

termsort.h: $(srcdir)/MKtermsort.sh
	$(SHELL) $(srcdir)/MKtermsort.sh $(AWK) $(srcdir)/../include/Caps >$@

#
# Utility productions start here
#

tags:
	$(CTAGS) *.[ch]

#TAGS:
#	$(ETAGS) *.[ch]

mostlyclean ::
	-rm -f core tags TAGS *~ *.bak *.i *.ln *.atac trace

clean :: mostlyclean
	-$(SHELL) -c "if test -n '$x' ; then $(MAKE) clean x=''; fi"
	-rm -f $(AUTO_SRC)
	-rm -f $(PROGS)
	-rm -rf .libs *.dSYM

distclean :: clean
	-rm -f Makefile

realclean :: distclean

# These rules are used to allow "make -n" to work on a clean directory-tree
../include/curses.h \
../include/hashsize.h \
../include/ncurses_def.h \
../include/parametrized.h \
../include/term.h :
	( cd ../include && $(MAKE) $(TOP_MFLAGS) )
$(DEPS_CURSES) :
	( cd ../ncurses && $(MAKE) $(TOP_MFLAGS) )

lint:
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/tic.c     $(srcdir)/dump_entry.c $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/toe.c                    $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/clear.c                          $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/infocmp.c $(srcdir)/dump_entry.c $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/tabs.c                           $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/tput.c                           $(LINT_LIBS)
	$(LINT) $(LINT_OPTS) $(CPPFLAGS) $(srcdir)/tset.c                           $(LINT_LIBS)

# Verify that each header-file can be compiled without including another.
check:
	@$(SHELL) -c "for header in *.h;\
		do \
			echo \"** testing \$${header}\" ; \
			echo \"#include <\$${header}>\" >headers.c; \
			echo \"int main(void) { return 0; }\" >>headers.c; \
			$(CC) -c $(CFLAGS) $(CPPFLAGS) headers.c; \
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
#   libname:    progsw
#   subsets:    ticlib+termlib+ext_tinfo base+widechar+ext_funcs

.SUFFIXES: .c .cc .h .i .ii
.c.i :
	$(CPP) $(CPPFLAGS) $< >$@
.cc.ii :
	$(CPP) $(CPPFLAGS) $< >$@
.h.i :
	$(CPP) $(CPPFLAGS) $< >$@

C_SRC = \
	$(srcdir)/clear.c \
	$(srcdir)/tic.c \
	$(srcdir)/toe.c \
	$(srcdir)/clear_cmd.c \
	$(srcdir)/dump_entry.c \
	$(srcdir)/infocmp.c \
	$(srcdir)/reset_cmd.c \
	$(srcdir)/tabs.c \
	$(srcdir)/tparm_type.c \
	$(srcdir)/tput.c \
	$(srcdir)/tset.c \
	$(srcdir)/transform.c \
	$(srcdir)/tty_settings.c

lintlib :
	@echo no action needed







# generated by mk-1st.awk (subset=base+widechar+ext_funcs)
#  name:            w
#  traces:          DEBUG
#  MODEL:           SHARED
#  CXX_MODEL:       NORMAL
#  LIB_SUFFIX:      w
#  ABI_SUFFIX:      w
#  model:           obj_s
#  prefix:          lib
#  suffix:          .so
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
#  depend:          ../include/ncurses_cfg.h ./progs.priv.h
#  host:            x86_64-pc-linux-gnu
#  libtool_version: -version-number

SHARED_OBJS = \
	../obj_s/clear$o \
	../obj_s/tic$o \
	../obj_s/toe$o \
	../obj_s/clear_cmd$o \
	../obj_s/dump_entry$o \
	../obj_s/infocmp$o \
	../obj_s/reset_cmd$o \
	../obj_s/tabs$o \
	../obj_s/tparm_type$o \
	../obj_s/tput$o \
	../obj_s/tset$o \
	../obj_s/transform$o \
	../obj_s/tty_settings$o

$(SHARED_OBJS) : ../include/ncurses_cfg.h ./progs.priv.h

mostlyclean::
	-rm -f $(SHARED_OBJS)

clean ::
	-rm -f $(SHARED_OBJS)

# generated by mk-2nd.awk
#   model:      obj_s
#   MODEL:      SHARED
#   echo:       yes
#   subset:     base+widechar+ext_funcs
#   crenames:   yes
#   cxxrenames: yes
#   traces:     DEBUG
#   srcdir:     .

../obj_s/clear$o :	$(srcdir)/clear.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/clear.c -o ../obj_s/clear$o

../obj_s/tic$o :	$(srcdir)/tic.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h \
			$(srcdir)/dump_entry.h \
			$(srcdir)/tparm_type.h \
			../include/parametrized.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tic.c -o ../obj_s/tic$o

../obj_s/toe$o :	$(srcdir)/toe.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/toe.c -o ../obj_s/toe$o

../obj_s/clear_cmd$o :	$(srcdir)/clear_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/clear_cmd.c -o ../obj_s/clear_cmd$o

../obj_s/dump_entry$o :	$(srcdir)/dump_entry.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h \
			../include/capdefaults.c \
			../include/parametrized.h \
			termsort.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/dump_entry.c -o ../obj_s/dump_entry$o

../obj_s/infocmp$o :	$(srcdir)/infocmp.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/infocmp.c -o ../obj_s/infocmp$o

../obj_s/reset_cmd$o :	$(srcdir)/reset_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/reset_cmd.c -o ../obj_s/reset_cmd$o

../obj_s/tabs$o :	$(srcdir)/tabs.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tabs.c -o ../obj_s/tabs$o

../obj_s/tparm_type$o :	$(srcdir)/tparm_type.c \
			$(HEADER_DEPS) \
			$(srcdir)/tparm_type.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tparm_type.c -o ../obj_s/tparm_type$o

../obj_s/tput$o :	$(srcdir)/tput.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tparm_type.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tput.c -o ../obj_s/tput$o

../obj_s/tset$o :	$(srcdir)/tset.c \
			$(HEADER_DEPS) \
			../include/termcap.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tset.c -o ../obj_s/tset$o

../obj_s/transform$o :	$(srcdir)/transform.c \
			$(HEADER_DEPS) \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/transform.c -o ../obj_s/transform$o

../obj_s/tty_settings$o :	$(srcdir)/tty_settings.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_SHARED) -c ../progs/tty_settings.c -o ../obj_s/tty_settings$o


# generated by mk-1st.awk (subset=base+widechar+ext_funcs)
#  name:            w
#  traces:          DEBUG
#  MODEL:           NORMAL
#  CXX_MODEL:       NORMAL
#  LIB_SUFFIX:      w
#  ABI_SUFFIX:      w
#  model:           objects
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
#  DoLinks:         yes
#  rmSoLocs:        no
#  ldconfig:        /usr/bin/ldconfig
#  make_phony:      yes
#  overwrite:       no
#  depend:          ../include/ncurses_cfg.h ./progs.priv.h
#  host:            x86_64-pc-linux-gnu
#  libtool_version: -version-number

NORMAL_OBJS = \
	../objects/clear$o \
	../objects/tic$o \
	../objects/toe$o \
	../objects/clear_cmd$o \
	../objects/dump_entry$o \
	../objects/infocmp$o \
	../objects/reset_cmd$o \
	../objects/tabs$o \
	../objects/tparm_type$o \
	../objects/tput$o \
	../objects/tset$o \
	../objects/transform$o \
	../objects/tty_settings$o

$(NORMAL_OBJS) : ../include/ncurses_cfg.h ./progs.priv.h

mostlyclean::
	-rm -f $(NORMAL_OBJS)

clean ::
	-rm -f $(NORMAL_OBJS)

# generated by mk-2nd.awk
#   model:      objects
#   MODEL:      NORMAL
#   echo:       yes
#   subset:     base+widechar+ext_funcs
#   crenames:   yes
#   cxxrenames: yes
#   traces:     DEBUG
#   srcdir:     .

../objects/clear$o :	$(srcdir)/clear.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/clear.c -o ../objects/clear$o

../objects/tic$o :	$(srcdir)/tic.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h \
			$(srcdir)/dump_entry.h \
			$(srcdir)/tparm_type.h \
			../include/parametrized.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tic.c -o ../objects/tic$o

../objects/toe$o :	$(srcdir)/toe.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/toe.c -o ../objects/toe$o

../objects/clear_cmd$o :	$(srcdir)/clear_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/clear_cmd.c -o ../objects/clear_cmd$o

../objects/dump_entry$o :	$(srcdir)/dump_entry.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h \
			../include/capdefaults.c \
			../include/parametrized.h \
			termsort.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/dump_entry.c -o ../objects/dump_entry$o

../objects/infocmp$o :	$(srcdir)/infocmp.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/infocmp.c -o ../objects/infocmp$o

../objects/reset_cmd$o :	$(srcdir)/reset_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/reset_cmd.c -o ../objects/reset_cmd$o

../objects/tabs$o :	$(srcdir)/tabs.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tabs.c -o ../objects/tabs$o

../objects/tparm_type$o :	$(srcdir)/tparm_type.c \
			$(HEADER_DEPS) \
			$(srcdir)/tparm_type.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tparm_type.c -o ../objects/tparm_type$o

../objects/tput$o :	$(srcdir)/tput.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tparm_type.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tput.c -o ../objects/tput$o

../objects/tset$o :	$(srcdir)/tset.c \
			$(HEADER_DEPS) \
			../include/termcap.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tset.c -o ../objects/tset$o

../objects/transform$o :	$(srcdir)/transform.c \
			$(HEADER_DEPS) \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/transform.c -o ../objects/transform$o

../objects/tty_settings$o :	$(srcdir)/tty_settings.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_NORMAL) -c ../progs/tty_settings.c -o ../objects/tty_settings$o


# generated by mk-1st.awk (subset=base+widechar+ext_funcs)
#  name:            w
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
#  depend:          ../include/ncurses_cfg.h ./progs.priv.h
#  host:            x86_64-pc-linux-gnu
#  libtool_version: -version-number

DEBUG_OBJS = \
	../obj_g/clear$o \
	../obj_g/tic$o \
	../obj_g/toe$o \
	../obj_g/clear_cmd$o \
	../obj_g/dump_entry$o \
	../obj_g/infocmp$o \
	../obj_g/reset_cmd$o \
	../obj_g/tabs$o \
	../obj_g/tparm_type$o \
	../obj_g/tput$o \
	../obj_g/tset$o \
	../obj_g/transform$o \
	../obj_g/tty_settings$o

$(DEBUG_OBJS) : ../include/ncurses_cfg.h ./progs.priv.h

mostlyclean::
	-rm -f $(DEBUG_OBJS)

clean ::
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

../obj_g/clear$o :	$(srcdir)/clear.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/clear.c -o ../obj_g/clear$o

../obj_g/tic$o :	$(srcdir)/tic.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h \
			$(srcdir)/dump_entry.h \
			$(srcdir)/tparm_type.h \
			../include/parametrized.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tic.c -o ../obj_g/tic$o

../obj_g/toe$o :	$(srcdir)/toe.c \
			$(HEADER_DEPS) \
			$(INCDIR)/hashed_db.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/toe.c -o ../obj_g/toe$o

../obj_g/clear_cmd$o :	$(srcdir)/clear_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/clear_cmd.c -o ../obj_g/clear_cmd$o

../obj_g/dump_entry$o :	$(srcdir)/dump_entry.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h \
			../include/capdefaults.c \
			../include/parametrized.h \
			termsort.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/dump_entry.c -o ../obj_g/dump_entry$o

../obj_g/infocmp$o :	$(srcdir)/infocmp.c \
			$(HEADER_DEPS) \
			$(srcdir)/dump_entry.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/infocmp.c -o ../obj_g/infocmp$o

../obj_g/reset_cmd$o :	$(srcdir)/reset_cmd.c \
			$(HEADER_DEPS) \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/reset_cmd.c -o ../obj_g/reset_cmd$o

../obj_g/tabs$o :	$(srcdir)/tabs.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tabs.c -o ../obj_g/tabs$o

../obj_g/tparm_type$o :	$(srcdir)/tparm_type.c \
			$(HEADER_DEPS) \
			$(srcdir)/tparm_type.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tparm_type.c -o ../obj_g/tparm_type$o

../obj_g/tput$o :	$(srcdir)/tput.c \
			$(HEADER_DEPS) \
			$(srcdir)/clear_cmd.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tparm_type.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tput.c -o ../obj_g/tput$o

../obj_g/tset$o :	$(srcdir)/tset.c \
			$(HEADER_DEPS) \
			../include/termcap.h \
			$(srcdir)/reset_cmd.h \
			$(srcdir)/tty_settings.h \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tset.c -o ../obj_g/tset$o

../obj_g/transform$o :	$(srcdir)/transform.c \
			$(HEADER_DEPS) \
			transform.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/transform.c -o ../obj_g/transform$o

../obj_g/tty_settings$o :	$(srcdir)/tty_settings.c \
			$(HEADER_DEPS) \
			$(srcdir)/tty_settings.h
	$(LIBTOOL_COMPILE) $(CC) $(CFLAGS_DEBUG) -c ../progs/tty_settings.c -o ../obj_g/tty_settings$o


.PHONY :	depend
depend : ${AUTO_SRC}
	makedepend -- ${CPPFLAGS} -- ${C_SRC}

# DO NOT DELETE THIS LINE -- make depend depends on it.
