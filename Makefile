# Measurement tools. `make` builds all three into this directory.
CFLAGS ?= -O2 -Wall -Wextra

all: ratesweep ratecheck ratesteal

ratesweep: measure/ratesweep.c          # usbdevfs, driver detached
	$(CC) $(CFLAGS) -o $@ $<

ratecheck: measure/ratecheck.c          # through ALSA
	$(CC) $(CFLAGS) -o $@ $< -lasound

ratesteal: measure/ratesteal.c          # through ALSA, two clients
	$(CC) $(CFLAGS) -o $@ $< -lasound -lm

clean:
	rm -f ratesweep ratecheck ratesteal

.PHONY: all clean
