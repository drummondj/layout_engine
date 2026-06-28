DEF_TABNAME     = def
DEF_BISON_SRCS  = def.y

# Use YACC_CMD and YACC_FLAGS variables if provided, otherwise set defaults.
YACC_CMD ?= bison
YACC_FLAGS ?= -y

FAKE_ALL: all

LIBTARGET =	libdef.a

HEADERS =	 \
			def.tab.h \
			lex.h \
			lex.cpph

PUBLIC_HDRS = \
			defiAssertion.hpp \
		   	defiBlockage.hpp \
			defiComponent.hpp \
			defiDebug.hpp \
			defiDefs.hpp \
			defiFPC.hpp \
			defiFill.hpp \
			defiGroup.hpp \
			defiIOTiming.hpp \
			defiKRDefs.hpp \
			defiMisc.hpp \
			defiNet.hpp \
			defiNonDefault.hpp \
			defiPartition.hpp \
			defiPath.hpp \
			defiPinCap.hpp \
			defiPinProp.hpp \
			defiProp.hpp \
			defiPropType.hpp \
			defiRegion.hpp \
			defiRowTrack.hpp \
			defiScanchain.hpp \
			defiSite.hpp \
			defiSlot.hpp \
			defiTimingDisable.hpp \
			defiUser.hpp \
			defiUtil.hpp \
			defiVia.hpp \
			defrCallBacks.hpp \
                        defrData.hpp \
			defrReader.hpp \
                        defrSettings.hpp \
			defwWriter.hpp \
			defwWriterCalls.hpp

LIBSRCS =	\
			def.tab.cpp \
			def_keywords.cpp \
			defiAssertion.cpp \
			defiBlockage.cpp \
			defiComponent.cpp \
			defiDebug.cpp \
			defiFPC.cpp \
			defiFill.cpp \
			defiGroup.cpp \
			defiIOTiming.cpp \
			defiMisc.cpp \
			defiNet.cpp \
			defiNonDefault.cpp \
			defiPartition.cpp \
			defiPath.cpp \
			defiPinCap.cpp \
			defiPinProp.cpp \
			defiProp.cpp \
			defiPropType.cpp \
			defiRegion.cpp \
			defiRowTrack.cpp \
			defiScanchain.cpp \
			defiSite.cpp \
			defiSlot.cpp \
			defiTimingDisable.cpp \
			defiUtil.cpp \
			defiVia.cpp \
                        defrCallbacks.cpp \
                        defrData.cpp \
			defrReader.cpp \
                        defrSettings.cpp \
			defwWriter.cpp \
			defwWriterCalls.cpp

def.tab.cpp : $(DEF_BISON_SRCS)
	$(YACC_CMD) $(YACC_FLAGS) -v -p$(DEF_TABNAME)yy -d $(DEF_BISON_SRCS)
	mv y.tab.h def.tab.h
	mv y.tab.c $(DEF_TABNAME).tab.cpp ;

include ../template.mk
