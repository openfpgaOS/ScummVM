SCUMMVM = ../../src/scummvm/scummvm
CXX     = g++
CXXFLAGS = -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare \
           -Wno-missing-field-initializers \
           -fno-exceptions \
           -I. -I$(SCUMMVM) -I$(SCUMMVM)/engines -I$(SCUMMVM)/backends \
           -DPOSIX -DUNIX -DLINUX -DSCUMM_LITTLE_ENDIAN \
           -DHAVE_CONFIG_H

# Minimum source set.  Add files here as link errors uncover more deps.
SRCS = test.cpp stubs.cpp \
       $(SCUMMVM)/audio/decoders/wmapro.cpp \
       $(SCUMMVM)/audio/decoders/raw.cpp \
       $(SCUMMVM)/audio/timestamp.cpp \
       $(SCUMMVM)/math/mdct.cpp \
       $(SCUMMVM)/math/fft.cpp \
       $(SCUMMVM)/math/cosinetables.cpp \
       $(SCUMMVM)/math/sinetables.cpp \
       $(SCUMMVM)/math/sinewindows.cpp \
       $(SCUMMVM)/common/textconsole.cpp \
       $(SCUMMVM)/common/util.cpp \
       $(SCUMMVM)/common/path.cpp \
       $(SCUMMVM)/common/str.cpp \
       $(SCUMMVM)/common/str-base.cpp \
       $(SCUMMVM)/common/memorypool.cpp \
       $(SCUMMVM)/common/hashmap.cpp \
       $(SCUMMVM)/common/punycode.cpp \
       $(SCUMMVM)/common/stream.cpp \
       $(SCUMMVM)/common/dbcs-str.cpp \
       $(SCUMMVM)/common/ustr.cpp \
       $(SCUMMVM)/common/unicode-bidi.cpp \
       $(SCUMMVM)/common/streamdebug.cpp \
       $(SCUMMVM)/common/str-enc.cpp \
       $(SCUMMVM)/common/translation.cpp \
       $(SCUMMVM)/common/encodings/singlebyte.cpp

wmapro_test: $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -f wmapro_test
.PHONY: clean
