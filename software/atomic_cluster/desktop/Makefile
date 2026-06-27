# Desktop offline test harness for the Atomic Cluster core.
# Builds a portable CLI that runs the exact pedal DSP on a WAV file.

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -I..
TARGET    = cluster

$(TARGET): main.cpp ../atomic_cluster_core.h ../shy_fft.h dr_wav.h
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

clean:
	rm -f $(TARGET) *.wav

.PHONY: clean
