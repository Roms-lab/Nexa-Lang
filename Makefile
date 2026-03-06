# Prefer clang++, fallback to g++ if clang not available
CXX := $(shell which clang++ 2>/dev/null || which g++ 2>/dev/null || echo "g++")
CXXFLAGS = -std=c++17 -O2
PREFIX ?= $(HOME)/.local

NexaC: NexaC.cpp include/Lexer.hpp include/Parser.hpp include/Transpiler.hpp include/Modules.hpp include/nexapkg.hpp
	$(CXX) $(CXXFLAGS) NexaC.cpp -o NexaC

install: NexaC
	install -d $(PREFIX)/bin
	install -m 755 NexaC $(PREFIX)/bin/NexaC
	ln -sf NexaC $(PREFIX)/bin/nexapkg
	ln -sf NexaC $(PREFIX)/bin/nexac

win: NexaC.cpp include/Lexer.hpp include/Parser.hpp include/Transpiler.hpp include/Modules.hpp
	$(MAKE) -C WIN

installer: NexaC
	./NexaC Installer/Installer.nxa -o installer

# Build Tests/plugin.nxa as Windows DLL (requires mingw-w64)
dll: NexaC
	./NexaC Tests/plugin.nxa --dll -o Tests/plugin.dll

# Build Tests/plugin.nxa as Linux .so
so: NexaC
	./NexaC Tests/plugin.nxa --shared -o Tests/plugin.so

# Build Examples/Number Guessing Game.nxa as Windows .exe (requires mingw-w64)
win-exe: NexaC
	./NexaC "Examples/Number Guessing Game.nxa" --win -o Tests/NumberGuessingGame.exe

# Build Tests/PkgTest (nexapkg package test)
pkgtest: NexaC
	cd Tests/PkgTest && ../../NexaC nexapkg install && ../../NexaC main.nxa -o pkgtest && ./pkgtest

clean:
	rm -f NexaC nexapkg
	$(MAKE) -C WIN clean

.PHONY: install win installer dll so win-exe clean pkgtest
