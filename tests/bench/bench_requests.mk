# Overlay: make -f Makefile -f /absolute/bench_requests.mk ... request-benchmark
# Reuse the complete allocator-instrumented graph, never the exported shared ABI.
REQUEST_HARNESS ?= $(dir $(lastword $(MAKEFILE_LIST)))bench_requests.c
REQUEST_BIN := $(BUILD)/bench-requests
REQUEST_OBJ := $(BUILD)/bench-requests.o

.PHONY: request-benchmark request-benchmark-config
request-benchmark: $(REQUEST_BIN)

$(REQUEST_OBJ): $(REQUEST_HARNESS) | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(FAULT_PIC_CFLAGS) -MMD -MP -c -o $@ $<

$(REQUEST_BIN): $(REQUEST_OBJ) $(FAULT_PIC_OBJS)
	$(CXX) -o $@ $^ $(REL_LDFLAGS)

request-benchmark-config:
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' \
	 'CFLAGS=$(FAULT_PIC_CFLAGS)' 'CXXFLAGS=$(FAULT_PIC_CXXFLAGS)' \
	 'LDFLAGS=$(REL_LDFLAGS)' 'SOURCES=$(LIB_SRC) $(TP)' 'SANITIZE=$(SANITIZE)'

-include $(REQUEST_OBJ:.o=.d)
