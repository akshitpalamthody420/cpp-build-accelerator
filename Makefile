CXX := g++
CXXFLAGS := -std=c++20 -pthread

.PHONY: all clean

all: forge-client forge-worker

forge-client: forge-client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

forge-worker: forge-worker.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f forge-client forge-worker app dependency-app flag-app complex-app
	rm -rf returned received worker-jobs cache
