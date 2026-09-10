
SHELL = /bin/sh

# V=0 quiet, V=1 verbose.  other values don't work.
V = 0
V0 = $(V:0=)
Q1 = $(V:1=)
Q = $(Q1:0=@)
ECHO1 = $(V:1=@ exit /b0.)
ECHO = $(ECHO1:0=@ echo)
NULLCMD = exit /b0.

#### Start of system configuration section. ####
top_srcdir = $(topdir)/.
gem_srcdir = $(top_srcdir)/.bundle/gems/win32ole-1.9.2
srcdir = $(gem_srcdir)/ext/win32ole
topdir = ../../../../..
hdrdir = $(top_srcdir)/include
arch_hdrdir = $(extout)/include/$(arch)
PATH_SEPARATOR = ;
VPATH = $(srcdir);$(arch_hdrdir)/ruby;$(hdrdir)/ruby
RUBYLIB =
RUBYOPT = -

DESTDIR = D:
prefix = $(DESTDIR)\ruby-build\ruby40-install
exec_prefix = $(prefix)
rubylibprefix = $(prefix)/lib/$(RUBY_BASE_NAME)
bindir = $(exec_prefix)/bin
sbindir = $(exec_prefix)/sbin
libexecdir = $(exec_prefix)/libexec
datadir = $(prefix)/share
sysconfdir = $(DESTDIR)
sharedstatedir = $(prefix)/com
localstatedir = $(prefix)/var
libdir = $(exec_prefix)/lib
includedir = $(prefix)/include
oldincludedir = $(DESTDIR)/usr/include
infodir = $(datadir)/info
mandir = $(datadir)/man
ridir = $(datadir)/ri
docdir = $(datadir)/doc/$(RUBY_BASE_NAME)
archlibdir = $(libdir)/$(arch)
sitearchlibdir = $(libdir)/$(sitearch)
archincludedir = $(includedir)/$(arch)
sitearchincludedir = $(includedir)/$(sitearch)
rubyarchdir = $(rubylibdir)/$(arch)
rubylibdir = $(rubylibprefix)/$(ruby_version)
sitedir = $(rubylibprefix)/site_ruby
sitearchdir = $(sitelibdir)/$(sitearch)
sitelibdir = $(sitedir)/$(ruby_version)
vendordir = $(rubylibprefix)/vendor_ruby
vendorarchdir = $(vendorlibdir)/$(sitearch)
vendorlibdir = $(vendordir)/$(ruby_version)
rubyhdrdir = $(includedir)/$(RUBY_VERSION_NAME)
sitehdrdir = $(rubyhdrdir)/site_ruby
vendorhdrdir = $(rubyhdrdir)/vendor_ruby
rubyarchhdrdir = $(rubyhdrdir)/$(arch)
sitearchhdrdir = $(sitehdrdir)/$(sitearch)
vendorarchhdrdir = $(vendorhdrdir)/$(sitearch)
archdir = $(rubyarchdir)


CC_WRAPPER = 
CC = cl -nologo -source-charset:utf-8
CXX = $(CC)
LIBRUBY = $(RUBY_SO_NAME).lib
LIBRUBY_A = $(RUBY_SO_NAME)-static.lib
LIBRUBYARG_SHARED = $(LIBRUBY)
LIBRUBYARG_STATIC = $(LIBRUBY_A) $(MAINLIBS)
empty =
OUTFLAG = -Fe$(empty)
COUTFLAG = -Fo$(empty)
CSRCFLAG = -Tc$(empty)

RUBY_EXTCONF_H = extconf.h
cflags   = 
cxxflags = 
optflags = 
debugflags = 
warnflags = 
cppflags = 
CCDLFLAGS = 
CFLAGS   = $(CCDLFLAGS) -MD -Zi -W3 -wd4100 -wd4127 -wd4210 -wd4214 -wd4255 -wd4574  -wd4668 -wd4710 -wd4711 -wd4820 -wd4996  -we4028 -we4142 -we4047 -we4013 -wd5287 -O2sy- -Zc:inline -Zm600  $(ARCH_FLAG)
INCFLAGS = -I. -I$(arch_hdrdir) -I$(hdrdir) -I$(srcdir)
DEFS     = 
CPPFLAGS = -DRUBY_EXTCONF_H=\"$(RUBY_EXTCONF_H)\"  -D_WIN32_WINNT=_WIN32_WINNT_WIN8  
CXXFLAGS = $(CCDLFLAGS) -MD -Zi -W2 -wd4100 -wd4127 -wd4210 -wd4214 -wd4255 -wd4574  -wd4668 -wd4710 -wd4711 -wd4820 -wd4996  -we4028 -we4142 -we4047 -we4013 -wd5287 -O2sy- -Zc:inline -Zm600  $(ARCH_FLAG)
ldflags  = -incremental:no -debug -opt:ref -opt:icf -manifest:embed,ID=2
dldflags = -incremental:no -debug -opt:ref -opt:icf -manifest:embed,ID=2 -dll $(LIBPATH) 
ARCH_FLAG = 
DLDFLAGS = $(ldflags) $(dldflags) $(ARCH_FLAG)
LDSHARED = cl -nologo -source-charset:utf-8 -LD
LDSHAREDXX = $(LDSHARED)
POSTLINK = 
AR = lib -nologo
LD = $(CC)
EXEEXT = .exe

RUBY_INSTALL_NAME = ruby
RUBY_SO_NAME = x64-vcruntime140-ruby400
RUBY_BASE_NAME = ruby
RUBYW_INSTALL_NAME = rubyw
RUBY_VERSION_NAME = $(RUBY_BASE_NAME)-$(ruby_version)

arch = x64-mswin64_140
sitearch = x64-vcruntime140
ruby_version = 4.0.0
ruby = $(topdir:/=\)\miniruby.exe -I'$(topdir)' -I'$(top_srcdir)/lib' -I'$(extout)/$(arch)' -I'$(extout)/common'
RUBY = $(ruby)
BUILTRUBY = $(topdir)/miniruby.exe
ruby_headers = {.;$(VPATH)}$(hdrdir)/ruby.h {.;$(VPATH)}$(hdrdir)/ruby/backward.h {.;$(VPATH)}$(hdrdir)/ruby/ruby.h {.;$(VPATH)}$(hdrdir)/ruby/defines.h {.;$(VPATH)}$(hdrdir)/ruby/missing.h {.;$(VPATH)}$(hdrdir)/ruby/intern.h {.;$(VPATH)}$(hdrdir)/ruby/st.h {.;$(VPATH)}$(hdrdir)/ruby/subst.h $(arch_hdrdir)/ruby/config.h $(RUBY_EXTCONF_H)

RM = $(COMSPEC) /C $(top_srcdir:/=\)\win32\rm.bat
RM_RF = $(COMSPEC) /C $(top_srcdir:/=\)\win32\rm.bat -f -r
RMDIRS = $(COMSPEC) /C $(top_srcdir:/=\)\win32\rmdirs.bat -p
MAKEDIRS = $(COMSPEC) /E:ON /C $(top_srcdir:/=\)\win32\makedirs.bat
INSTALL = @$(RUBY) -run -e install -- -vp
INSTALL_PROG = $(INSTALL) -m 0755
INSTALL_DATA = $(INSTALL) -m 0644
COPY = @$(RUBY) -run -e cp -- -v
TOUCH = exit >

#### End of system configuration section. ####

preload = 
EXTSO = 
libpath = . $(topdir)
LIBPATH = -libpath:. -libpath:$(topdir)
DEFFILE = $(TARGET)-$(arch).def

CLEANFILES = mkmf.log $(DEFFILE)
DISTCLEANFILES = vc*.pdb
DISTCLEANDIRS = 

extout = $(topdir)/.ext
extout_prefix = $(extout)$(target_prefix)/
target_prefix = 
LOCAL_LIBS = 
LIBS = $(LIBRUBYARG_SHARED) advapi32.lib kernel32.lib user32.lib uuid.lib oleaut32.lib ole32.lib  user32.lib 
ORIG_SRCS = win32ole.c win32ole_error.c win32ole_event.c win32ole_method.c win32ole_param.c win32ole_record.c win32ole_type.c win32ole_typelib.c win32ole_variable.c win32ole_variant.c win32ole_variant_m.c
SRCS = $(ORIG_SRCS) 
OBJS = win32ole.obj win32ole_error.obj win32ole_event.obj win32ole_method.obj win32ole_param.obj win32ole_record.obj win32ole_type.obj win32ole_typelib.obj win32ole_variable.obj win32ole_variant.obj win32ole_variant_m.obj
HDRS = $(srcdir)/win32ole.h $(srcdir)/win32ole_error.h $(srcdir)/win32ole_event.h $(srcdir)/win32ole_method.h $(srcdir)/win32ole_param.h $(srcdir)/win32ole_record.h $(srcdir)/win32ole_type.h $(srcdir)/win32ole_typelib.h $(srcdir)/win32ole_variable.h $(srcdir)/win32ole_variant.h $(srcdir)/win32ole_variant_m.h
LOCAL_HDRS = 
TARGET = win32ole
TARGET_NAME = win32ole
TARGET_ENTRY = Init_$(TARGET_NAME)
DLLIB = $(TARGET).so
EXTSTATIC = 
STATIC_LIB = $(TARGET).lib

TARGET_TOPDIR = $(topdir)/.bundle
TIMESTAMP_DIR = $(TARGET_TOPDIR)/.timestamp
BINDIR        = $(extout)/bin
RUBYCOMMONDIR = $(extout)/common
RUBYLIBDIR    = $(RUBYCOMMONDIR)$(target_prefix)
RUBYARCHDIR   = $(extout)/$(arch)$(target_prefix)
HDRDIR        = $(extout)/include/ruby$(target_prefix)
ARCHHDRDIR    = $(extout)/include/$(arch)/ruby$(target_prefix)
TARGET_GEM_DIR = $(TARGET_TOPDIR)/extensions/$(gem_platform)/$(ruby_version)/win32ole-1.9.2
TARGET_SO_DIR = $(TARGET_GEM_DIR)$(target_prefix)/
TARGET_SO     = $(TARGET_SO_DIR)$(DLLIB)
CLEANLIBS     = $(TARGET_SO)
CLEANOBJS     = $(OBJS) $(OBJS:.obj=.bc) $(OBJS:.obj=.i) $(OBJS:.obj=.s) $(TARGET)-$(arch).exp $(TARGET)-$(arch).lib $(TARGET)-$(arch).pdb *.bak
TARGET_SO_DIR_TIMESTAMP = $(TIMESTAMP_DIR)/$(gem_platform)/$(ruby_version)/.gems.-.win32ole-1.9.2.time

gem_platform = x64-mswin64-140

# default target
all:

gem = win32ole-1.9.2

build_complete = $(TARGET_GEM_DIR)/gem.build_complete
install-so: build_complete
clean-so:: clean-build_complete

build_complete: $(build_complete)
$(build_complete): $(TARGET_SO)
	$(Q) $(TOUCH) $@

clean-build_complete:
	-$(Q)$(RM) $(build_complete)

install: gemspec
clean: clean-gemspec

gemspec = $(TARGET_TOPDIR)/specifications/$(gem).gemspec
$(gemspec): $(gem_srcdir)/.bundled.$(gem).gemspec
	$(Q) $(MAKEDIRS) $(@D)
	$(Q) $(COPY) $(gem_srcdir)/.bundled.$(gem).gemspec $@

gemspec: $(gemspec)

clean-gemspec:
	-$(Q)$(RM) $(gemspec)

LN_S = false
CP_R =  -r

all:    install
static: all
.PHONY: all install static install-so install-rb
.PHONY: clean clean-so clean-static clean-rb

clean-static::
clean-rb-default::
clean-rb::
clean-so::
clean: clean-so clean-static clean-rb-default clean-rb
		-$(Q)$(RM_RF) $(CLEANLIBS:/=\) $(CLEANOBJS:/=\) $(CLEANFILES:/=\) .*.time

distclean-rb-default::
distclean-rb::
distclean-so::
distclean-static::
distclean: clean distclean-so distclean-static distclean-rb-default distclean-rb
		-$(Q)$(RM) Makefile $(RUBY_EXTCONF_H) conftest.* mkmf.log exts.mk
		-$(Q)$(RM) core ruby$(EXEEXT) *~ $(DISTCLEANFILES:/=\)
		-$(Q)$(RMDIRS) $(DISTCLEANDIRS:/=\) 2> NUL || exit /b0

realclean: distclean
install: install-so install-rb

install-so: $(TARGET_SO)
clean-so::
	-$(Q)$(RM) $(TARGET_SO:/=\) $(TARGET_SO_DIR_TIMESTAMP:/=\)
	-$(Q)$(RM_RF) $(CLEANLIBS:/=\)
	-$(Q)$(RMDIRS) $(TARGET_SO_DIR:/=\) 2> NUL || exit /b0
clean-static::
	-$(Q)$(RM) $(STATIC_LIB)
install-rb: pre-install-rb do-install-rb install-rb-default
install-rb-default: pre-install-rb-default do-install-rb-default
pre-install-rb: Makefile
pre-install-rb-default: Makefile
do-install-rb:
do-install-rb-default:
pre-install-rb-default:
	@$(NULLCMD)
$(TARGET_SO_DIR_TIMESTAMP):
	$(Q) $(MAKEDIRS) $(@D) $(TARGET_SO_DIR)
	$(Q) $(TOUCH) $@

site-install: site-install-so site-install-rb
site-install-so: install-so
site-install-rb: install-rb

.SUFFIXES: .c .m .cc .mm .cxx .cpp .obj .asm

{$(srcdir)}.cc.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(srcdir)}.cc.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cc.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cc.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cc.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cc.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

.cc.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

.cc.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(srcdir)}.mm.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(srcdir)}.mm.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.mm.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.mm.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.mm.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.mm.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

.mm.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

.mm.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(srcdir)}.cxx.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(srcdir)}.cxx.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cxx.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cxx.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cxx.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cxx.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

.cxx.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

.cxx.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(srcdir)}.cpp.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(srcdir)}.cpp.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cpp.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(arch_hdrdir)/ruby}.cpp.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cpp.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

{$(hdrdir)/ruby}.cpp.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

.cpp.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CXX) $(INCFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c -Tp$(<:\=/)

.cpp.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Fa$(@) -c -Tp$(<:\=/)

{$(srcdir)}.c.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(srcdir)}.c.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

{$(arch_hdrdir)/ruby}.c.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(arch_hdrdir)/ruby}.c.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

{$(hdrdir)/ruby}.c.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(hdrdir)/ruby}.c.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

.c.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

.c.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

{$(srcdir)}.m.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(srcdir)}.m.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

{$(arch_hdrdir)/ruby}.m.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(arch_hdrdir)/ruby}.m.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

{$(hdrdir)/ruby}.m.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

{$(hdrdir)/ruby}.m.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

.m.obj:
	$(ECHO) compiling $(<:\=/)
	$(Q) $(CC) $(INCFLAGS) $(CFLAGS) $(CPPFLAGS) $(COUTFLAG)$(@) -c $(CSRCFLAG)$(<:\=/)

.m.asm:
	$(ECHO) translating $(<:\=/)
	$(Q) $(CC) $(CFLAGS) $(CPPFLAGS) -Fa$(@) -c $(CSRCFLAG)$(<:\=/)

$(TARGET_SO): $(DEFFILE) $(OBJS) Makefile $(TARGET_SO_DIR_TIMESTAMP)
	$(ECHO) linking shared-object $(DLLIB)
	-$(Q)$(RM) $(@:/=\)
	$(Q) $(LDSHARED) -Fe$(@) $(OBJS) $(LIBS) $(LOCAL_LIBS) -link $(DLDFLAGS) -implib:$(*F:.so=)-$(arch).lib -pdb:$(*F:.so=)-$(arch).pdb -def:$(DEFFILE)

$(STATIC_LIB): $(OBJS)
	-$(Q)$(RM) $(@:/=\)
	$(ECHO) linking static-library $(@:\=/)
	$(Q) $(AR) -machine:x64 -out:$@ $(OBJS)

$(DEFFILE): 
	$(ECHO) generating $(@:\=/)
	$(Q) (echo EXPORTS && echo $(TARGET_ENTRY)) > $@

###
$(OBJS): $(hdrdir)/ruby/win32.h

$(OBJS): $(RUBY_EXTCONF_H)

WIN32OLE_HEADERS = $(HDRS) $(ruby_headers)
win32ole.obj : {.;$(VPATH)}win32ole.c $(WIN32OLE_HEADERS)
win32ole_variant_m.obj : {.;$(VPATH)}win32ole_variant_m.c $(WIN32OLE_HEADERS)
win32ole_typelib.obj : {.;$(VPATH)}win32ole_typelib.c $(WIN32OLE_HEADERS)
win32ole_type.obj : {.;$(VPATH)}win32ole_type.c $(WIN32OLE_HEADERS)
win32ole_variable.obj : {.;$(VPATH)}win32ole_variable.c $(WIN32OLE_HEADERS)
win32ole_method.obj : {.;$(VPATH)}win32ole_method.c $(WIN32OLE_HEADERS)
win32ole_param.obj : {.;$(VPATH)}win32ole_param.c $(WIN32OLE_HEADERS)
win32ole_variant.obj : {.;$(VPATH)}win32ole_variant.c $(WIN32OLE_HEADERS)
win32ole_event.obj : {.;$(VPATH)}win32ole_event.c $(WIN32OLE_HEADERS)
win32ole_record.obj : {.;$(VPATH)}win32ole_record.c $(WIN32OLE_HEADERS)
win32ole_error.obj : {.;$(VPATH)}win32ole_error.c $(WIN32OLE_HEADERS)
