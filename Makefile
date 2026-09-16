CXX := g++
CXXFLAGS ?= -O2 -Wall -Wextra
PYTHON ?= python3
REPEATS ?= 3
JOBS ?= 4

.PHONY: all clean test benchmark

all: forge-client forge-worker

forge-client: forge-client.cpp job-support.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread $< $(LDFLAGS) $(LDLIBS) -o $@

forge-worker: forge-worker.cpp job-support.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread $< $(LDFLAGS) $(LDLIBS) -o $@

test:
	$(PYTHON) test-failures.py
	$(PYTHON) test-examples.py

benchmark:
	$(PYTHON) test-examples.py --benchmark --repeats $(REPEATS) --jobs $(JOBS) --output benchmark-results.json

clean:
	rm -f forge-client forge-worker app dependency-app flag-app complex-app
	rm -rf returned received worker-jobs cache
