import CTny

var spec = tny_tool_spec_v1()
var result = tny_tool_result_v1()
var capabilities = tny_capabilities_v1()
precondition(TNY_TOOL_INVOKE_ASYNC == 1)
precondition(TNY_CUSTOM_TOOL_MAX_COUNT == 64)
_ = spec
_ = result
_ = capabilities
