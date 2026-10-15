# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Command line front end to the SavvyCAN reverse engineering analysis.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
RE       := ../../gui/savvycan/re

canre_cli: canre_cli.cpp $(RE)/aitools.h $(RE)/canre.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ canre_cli.cpp

clean:
	rm -f canre_cli canre_cli.exe

.PHONY: clean
