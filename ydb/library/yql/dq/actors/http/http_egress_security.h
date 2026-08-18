#pragma once

#include <util/datetime/base.h>

#include <ydb/library/actors/http/http.h>
#include <util/generic/string.h>
#include <util/generic/map.h>
#include <util/generic/set.h>
#include <util/generic/vector.h>
#include <util/generic/algorithm.h>

#include <algorithm>

namespace NYql::NDq::NHttpEgress {

// Security configuration for HTTP egress.
struct TEgressSecurityConfig {
    // Allowed hosts (empty = deny all).
    TSet<TString> AllowedHosts;

    // Denied hosts (checked after allowed list; takes precedence).
    TSet<TString> DeniedHosts;

    // Maximum request body size in bytes (default 1MB).
    ui64 MaxRequestBodySize = 1024 * 1024;

    // Maximum response body size in bytes (default 10MB).
    ui64 MaxResponseBodySize = 10 * 1024 * 1024;

    // Maximum total headers size in bytes (default 16KB).
    ui64 MaxHeadersSize = 16 * 1024;

    // Maximum number of in-flight requests globally.
    ui64 MaxInFlightRequests = 1000;

    // Maximum number of in-flight requests per host.
    ui64 MaxInFlightRequestsPerHost = 100;

    // Default timeout for requests.
    TDuration DefaultTimeout = TDuration::Seconds(30);

    // Maximum allowed timeout.
    TDuration MaxTimeout = TDuration::Seconds(300);
};

// Reserved header names that cannot be overridden by user-supplied headers.
// These are managed by the platform for auth and transport.
/// Resolve hostname to IP and check if the resolved IP is blocked.
/// Returns the resolved IP on success, or empty string if resolution failed or IP is blocked.
/// This provides DNS rebinding protection by resolving the host before the request.
TString ResolveAndPinHost(TStringBuf host, const TEgressSecurityConfig& config);

/// Redacts sensitive header values for safe logging.
/// Headers like Authorization, Cookie, Proxy-Authorization are replaced with "[REDACTED]".
TString RedactSensitiveHeaders(const NHttp::THeadersBuilder& headers);

/// Checks if a header name contains sensitive information that should be redacted.
bool IsSensitiveHeader(TStringBuf headerName);

inline bool IsReservedHeader(TStringBuf headerName) {
    static const TVector<TString> Reserved = {
        "authorization",
        "host",
        "content-length",
        "connection",
        "transfer-encoding",
        "proxy-authorization",
        "proxy-connection",
        "proxy-host",
        "proxy-port",
    };
    // Case-insensitive check.
    TString lowerName;
    lowerName.reserve(headerName.size());
    for (auto c : headerName) {
        lowerName += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto& r : Reserved) {
        if (r == lowerName) {
            return true;
        }
    }
    return false;
}

// Validates header name and value for injection prevention.
// Returns true if valid, false if the header should be rejected.
inline bool ValidateHeader(TStringBuf name, TStringBuf value) {
    // Empty name is invalid.
    if (name.empty()) {
        return false;
    }
    // Check for CR/LF characters (header injection / response splitting).
    if (name.find('\r') != TString::npos || name.find('\n') != TString::npos) {
        return false;
    }
    if (value.find('\r') != TString::npos || value.find('\n') != TString::npos) {
        return false;
    }
    return true;
}

// Result of SSRF protection check, distinguishing between true SSRF attacks
// (blocked IPs) and policy violations (host not in allowlist).
enum class ESSRFResult {
    Allowed,        // URL is allowed
    BlockedIP,      // URL contains a blocked IP (true SSRF attack)
    BlockedPolicy,  // Host not in allowlist (policy violation, not necessarily attack)
};

// Checks if the given URL passes SSRF protection.
// Returns ESSRFResult::Allowed if the URL is allowed, otherwise the specific
// block reason.  This allows callers to increment the correct counter:
// SSRFBlocks for actual SSRF attacks (BlockedIP) vs DeniedHosts for policy
// violations (BlockedPolicy).
ESSRFResult CheckSSRFProtection(TStringBuf url, const TEgressSecurityConfig& config);

// Checks if the host is in the allowlist (or denylist).
// Returns true if allowed.
bool CheckHostPolicy(TStringBuf host, const TEgressSecurityConfig& config);

} // namespace NYql::NDq::NHttpEgress
