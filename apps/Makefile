#
# Trismedia -- A telephony toolkit for Linux.
# 
# Makefile for PBX applications
#
# Copyright (C) 1999-2006, Digium, Inc.
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#

-include $(ASTTOPDIR)/menuselect.makeopts $(ASTTOPDIR)/menuselect.makedeps

MODULE_PREFIX=app
MENUSELECT_CATEGORY=APPS
MENUSELECT_DESCRIPTION=Applications

MENUSELECT_OPTS_app_directory:=$(MENUSELECT_OPTS_app_voicemail)
ifneq ($(findstring ODBC_STORAGE,$(MENUSELECT_OPTS_app_voicemail)),)
  MENUSELECT_DEPENDS_app_voicemail+=$(MENUSELECT_DEPENDS_ODBC_STORAGE)
endif
ifneq ($(findstring IMAP_STORAGE,$(MENUSELECT_OPTS_app_voicemail)),)
  MENUSELECT_DEPENDS_app_voicemail+=$(MENUSELECT_DEPENDS_IMAP_STORAGE)
endif

all: _all

include $(ASTTOPDIR)/Makefile.moddir_rules

$(if $(filter app_conference,$(EMBEDDED_MODS)),modules.link,app_conference.so): $(subst .c,.o,$(wildcard appconference/*.c))
$(subst .c,.o,$(wildcard appconference/*.c)): _ASTCFLAGS+=$(call MOD_ASTCFLAGS,app_conference)

ifneq ($(findstring $(OSARCH), mingw32 cygwin ),)
  LIBS+= -lres_features.so -lres_ael_share.so -lres_monitor.so -lres_speech.so
  LIBS+= -lres_smdi.so
endif

