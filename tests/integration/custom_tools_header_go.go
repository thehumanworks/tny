package main

/*
#include "tny/tny.h"
*/
import "C"

func main() {
	var spec C.tny_tool_spec_v1
	var result C.tny_tool_result_v1
	var capabilities C.tny_capabilities_v1
	_ = spec
	_ = result
	_ = capabilities
	if C.TNY_TOOL_INVOKE_ASYNC != 1 || C.TNY_CUSTOM_TOOL_MAX_COUNT != 64 {
		panic("unexpected custom-tool ABI constants")
	}
}
