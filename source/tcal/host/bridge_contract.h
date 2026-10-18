#pragma once

#include <string>

#include "json_strict.h"

namespace tcalendar {

struct BridgeRequest {
    std::wstring api_version;
    std::wstring request_id;
    std::wstring method;
    JsonObject root;
};

bool ValidateBridgeRequest(const std::wstring& request_json, BridgeRequest& out_req,
                           std::wstring& out_error_code, std::wstring& out_error_message);

} // namespace tcalendar
