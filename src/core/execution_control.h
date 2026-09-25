#ifndef TNY_EXECUTION_CONTROL_H
#define TNY_EXECUTION_CONTROL_H

#include "backends/openai/openai.h"

/* Private execution-server wire objects. Only nested tool/subagent controls
 * cross this boundary. Request strings borrow the decoded JSON document;
 * response strings are independently owned, including on callback return. */
yyjson_mut_val *tny_execution_control_encode_request(yyjson_mut_doc *doc,
                                                     const tny_openai_control_request *request);
bool tny_execution_control_decode_request(yyjson_val *value, tny_openai_control_request *request);
yyjson_mut_val *tny_execution_control_encode_response(yyjson_mut_doc *doc,
                                                      const tny_openai_control_response *response);
bool tny_execution_control_decode_response(yyjson_val *value,
                                           tny_openai_control_response *response);
void tny_execution_control_response_free(tny_openai_control_response *response);

#endif
