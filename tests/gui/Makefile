# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Host tests for Qt-independent helpers of the SavvyCAN windows.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra
RE       := ../../gui/savvycan/re

GUI      := ../../gui/savvycan

canidcalc_test: canidcalc_test.cpp $(RE)/canidcalc.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ canidcalc_test.cpp

pp2canlog_test: pp2canlog_test.cpp $(GUI)/pp2canlogfmt.h
	$(CXX) $(CXXFLAGS) -I $(GUI) -o $@ pp2canlog_test.cpp

canopen_eds_test: canopen_eds_test.cpp $(RE)/canopen_eds.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ canopen_eds_test.cpp

logprint_test: logprint_test.cpp $(GUI)/logprintlayout.h
	$(CXX) $(CXXFLAGS) -I $(GUI) -o $@ logprint_test.cpp

test: canidcalc_test pp2canlog_test canopen_eds_test logprint_test
	./canidcalc_test
	./pp2canlog_test
	./canopen_eds_test ../../tools/canopen_sim.eds
	./logprint_test

clean:
	rm -f canidcalc_test pp2canlog_test canopen_eds_test logprint_test

.PHONY: test clean
