# Build NexaC for Windows
# On Linux: uses clang++ with mingw target, or mingw-w64 fallback (apt install clang mingw-w64)
# On Windows: uses clang++ natively, fallback to g++
# If clang fails to compile, automatically retries with g++/mingw-g++

CXXFLAGS = -std=c++17 -O2 -static -static-libgcc -static-libstdc++

# Detect: cross-compile from Linux, or native on Windows
ifdef MINGW_PREFIX
  CXX = $(MINGW_PREFIX)-g++
  FALLBACK_CXX =
  SUFFIX = .exe
else ifeq ($(OS),Windows_NT)
  CXX := $(shell which clang++ 2>/dev/null || which g++ 2>/dev/null || echo "g++")
  FALLBACK_CXX := $(shell which g++ 2>/dev/null)
  SUFFIX = .exe
else
  CLANG := $(shell which clang++ 2>/dev/null)
  MINGW64 := $(shell which x86_64-w64-mingw32-g++ 2>/dev/null)
  MINGW32 := $(shell which i686-w64-mingw32-g++ 2>/dev/null)
  ifneq ($(CLANG),)
    CXX = clang++
    CXXFLAGS += -target x86_64-w64-mingw32
    FALLBACK_CXX = $(or $(MINGW64),$(MINGW32))
    SUFFIX = .exe
  else ifneq ($(MINGW64),)
    CXX = x86_64-w64-mingw32-g++
    FALLBACK_CXX =
    SUFFIX = .exe
  else ifneq ($(MINGW32),)
    CXX = i686-w64-mingw32-g++
    FALLBACK_CXX =
    SUFFIX = .exe
  else
    $(error clang++ or mingw-w64 required. Install: apt install clang mingw-w64)
  endif
endif

SRCDIR = ..
SOURCES = $(SRCDIR)/NexaC.cpp
HEADERS = $(SRCDIR)/include/Lexer.hpp $(SRCDIR)/include/Parser.hpp \
          $(SRCDIR)/include/Transpiler.hpp $(SRCDIR)/include/Modules.hpp

NexaC$(SUFFIX): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o NexaC$(SUFFIX) || \
	(if [ -n "$(FALLBACK_CXX)" ]; then \
		echo "clang++ failed, retrying with $(FALLBACK_CXX)..."; \
		$(FALLBACK_CXX) $(filter-out -target x86_64-w64-mingw32,$(CXXFLAGS)) $(SOURCES) -o NexaC$(SUFFIX); \
	else \
		exit 1; \
	fi)

.PHONY: clean
clean:
	rm -f NexaC.exe NexaC
