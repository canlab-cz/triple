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

canre_test: canre_test.cpp $(RE)/canre.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ canre_test.cpp

aitools_test: aitools_test.cpp $(RE)/aitools.h $(RE)/canre.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ aitools_test.cpp

canopen_block_test: canopen_block_test.cpp $(RE)/canopen_block.h
	$(CXX) $(CXXFLAGS) -I $(RE) -o $@ canopen_block_test.cpp

test: canidcalc_test pp2canlog_test canopen_eds_test logprint_test canre_test aitools_test canopen_block_test
	./canidcalc_test
	./pp2canlog_test
	./canopen_eds_test ../../tools/canopen_sim.eds
	./canopen_block_test
	./logprint_test
	./canre_test > canre_test.out || (cat canre_test.out; false)
	tail -1 canre_test.out
	./aitools_test > aitools_test.out || (cat aitools_test.out; false)
	tail -1 aitools_test.out
	./aitools_test --defs | python3 -c "import json,sys; d=json.load(sys.stdin); print('tool definitions: %d valid JSON tools' % len(d))"

clean:
	rm -f canidcalc_test pp2canlog_test canopen_eds_test logprint_test canre_test canre_test.out aitools_test aitools_test.out canopen_block_test

.PHONY: test clean
