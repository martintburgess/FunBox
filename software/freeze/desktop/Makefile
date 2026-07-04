# Desktop offline test harness for the Freeze core.
# Builds a portable CLI that runs the exact pedal DSP on a WAV file.

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -I..
TARGET    = freeze

$(TARGET): main.cpp ../freeze_core.h ../shy_fft.h dr_wav.h
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

clean:
	rm -f $(TARGET) *.wav

.PHONY: clean
