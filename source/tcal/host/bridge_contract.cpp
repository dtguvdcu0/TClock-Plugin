#include "bridge_contract.h"

namespace tcalendar {

bool ValidateBridgeRequest(const std::wstring& request_json, BridgeRequest& out_req,
                           std::wstring& out_error_code, std::wstring& out_error_message) {
    out_req = BridgeRequest{};
    out_error_code.clear();
    out_error_message.clear();

    std::wstring parse_error;
    if (!ParseJsonObject(request_json, out_req.root, parse_error)) {
        out_error_code = L"VALIDATION_ERROR";
        out_error_message = L"Invalid JSON payload: " + parse_error;
        return false;
    }

    if (!GetStringField(out_req.root, L"apiVersion", out_req.api_version)) {
        out_error_code = L"VALIDATION_ERROR";
        out_error_message = L"Missing or invalid apiVersion";
        return false;
    }
    if (!GetStringField(out_req.root, L"requestId", out_req.request_id)) {
        out_error_code = L"VALIDATION_ERROR";
        out_error_message = L"Missing or invalid requestId";
        return false;
    }
    if (!GetStringField(out_req.root, L"method", out_req.method)) {
        out_error_code = L"VALIDATION_ERROR";
        out_error_message = L"Missing or invalid method";
        return false;
    }

    return true;
}

} // namespace tcalendar
