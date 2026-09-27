# Benchmark-only: generate the MicroPython embed package with the json module.
# Invoke: make -f embed.mk MICROPYTHON_TOP=<source> PACKAGE_DIR=<output>
SRC_QSTR += $(MICROPYTHON_TOP)/extmod/modjson.c
include $(MICROPYTHON_TOP)/ports/embed/embed.mk
