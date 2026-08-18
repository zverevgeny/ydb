#include "http_egress_security.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ydb/library/actors/http/http.h>

#include <algorithm>

namespace NYql::NDq::NHttpEgress {

namespace {

// Strip IPv6 brackets if present (CrackURL returns host with brackets for IPv6).
TString StripIPv6Brackets(TStringBuf host) {
    if (host.StartsWith("[") && host.EndsWith("]")) {
        return TString(host.SubStr(1, host.size() - 2));
    }
    return TString(host);
}

// Check if an IP address is in a blocked range (RFC1918, loopback, link-local, cloud metadata).
bool IsBlockedIP(TStringBuf ip) {
    // 0.0.0.0 — special-use address
    if (ip == "0.0.0.0") {
        return true;
    }
    // Loopback: 127.0.0.0/8
    if (ip.StartsWith("127.")) {
        return true;
    }
    // Link-local: 169.254.0.0/16 (includes cloud metadata 169.254.169.254)
    if (ip.StartsWith("169.254.")) {
        return true;
    }
    // RFC1918 private ranges
    // 10.0.0.0/8
    if (ip.StartsWith("10.")) {
        return true;
    }
    // 172.16.0.0/12
    if (ip.StartsWith("172.")) {
        // Check second octet: 16-31
        auto dotPos = ip.find('.', 4);
        if (dotPos != TString::npos) {
            try {
                int secondOctet = std::stoi(TString(ip.SubStr(4, dotPos - 4)));
                if (secondOctet >= 16 && secondOctet <= 31) {
                    return true;
                }
            } catch (...) {
                // If parsing fails, be conservative and block.
                return true;
            }
        }
    }
    // 192.168.0.0/16
    if (ip.StartsWith("192.168.")) {
        return true;
    }
    // IPv6 loopback ::1
    if (ip == "::1") {
        return true;
    }
    // IPv6 link-local fe80::/10 (case-insensitive).
    // fe80::/10 covers fe80:: through febf::, i.e. first byte 0xFE, second byte 0x80–0xBF.
    // In hex notation: fe8x:: through febx:: where x is any hex digit.
    if (ip.size() >= 4) {
        char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(ip[0])));
        char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(ip[1])));
        char c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(ip[2])));
        char c3 = static_cast<char>(std::tolower(static_cast<unsigned char>(ip[3])));
        if (c0 == 'f' && c1 == 'e'
            && ((c2 >= '8' && c2 <= '9') || (c2 >= 'a' && c2 <= 'b'))
            && ((c3 >= '0' && c3 <= '9') || (c3 >= 'a' && c3 <= 'f'))) {
            return true;
        }
    }
    // IPv4-mapped IPv6 ::ffff:x.x.x.x — extract the IPv4 part and re-check.
    // Case-insensitive: compare lowercased prefix.
    if (ip.size() >= 7) {
        TString lowerPrefix;
        lowerPrefix.reserve(7);
        for (size_t i = 0; i < 7; ++i) {
            lowerPrefix += static_cast<char>(std::tolower(static_cast<unsigned char>(ip[i])));
        }
        if (lowerPrefix == "::ffff:") {
            return IsBlockedIP(TString(ip.SubStr(7)));
        }
    }
    return false;
}

} // namespace

ESSRFResult CheckSSRFProtection(TStringBuf url, const TEgressSecurityConfig& config) {
    // Extract scheme and host from URL.
    TStringBuf scheme, host, uri;
    if (!NHttp::CrackURL(url, scheme, host, uri)) {
        // Can't parse URL — reject as policy violation.
        return ESSRFResult::BlockedPolicy;
    }

    // Only allow http and https schemes.
    TString lowerScheme;
    lowerScheme.reserve(scheme.size());
    for (auto c : scheme) {
        lowerScheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowerScheme != "http" && lowerScheme != "https") {
        return ESSRFResult::BlockedPolicy;
    }

    // A bracketed host is always an IPv6 literal (CrackURL keeps the brackets).
    // We must treat it as an IP directly, because IsIPv6() rejects forms that
    // contain dots (e.g. IPv4-mapped ::ffff:127.0.0.1) and would otherwise let
    // them fall through to the hostname policy.
    const bool bracketedIPv6 = host.StartsWith("[") && host.EndsWith("]");
    TString hostStr = StripIPv6Brackets(host);
    if (bracketedIPv6 || NHttp::IsIPv4(hostStr) || NHttp::IsIPv6(hostStr)) {
        // Block private/reserved IPs — this is a true SSRF block.
        if (IsBlockedIP(hostStr)) {
            return ESSRFResult::BlockedIP;
        }
        // If an allowlist is configured, IP addresses must also be explicitly
        // allowed.  This prevents bypassing the allowlist by using a raw IP
        // instead of a hostname.
        if (!config.AllowedHosts.empty()) {
            return CheckHostPolicy(hostStr, config) ? ESSRFResult::Allowed : ESSRFResult::BlockedPolicy;
        }
        // No allowlist → allow any non-blocked IP.
        return ESSRFResult::Allowed;
    }

    // For hostname-based URLs, check the host policy.
    // NOTE: DNS rebinding protection (resolving the IP before connecting and
    // validating the resolved IP) is NOT implemented in Phase 1.
    // It is planned for Phase 6 (hardening).  Until then, hostname-based URLs
    // are only validated against the allowlist, and the actual resolved IP is
    // not checked.  This means a DNS rebinding attack could bypass SSRF
    // protection if an allowed hostname temporarily resolves to a blocked IP.
    return CheckHostPolicy(hostStr, config) ? ESSRFResult::Allowed : ESSRFResult::BlockedPolicy;
}

bool CheckHostPolicy(TStringBuf host, const TEgressSecurityConfig& config) {
    // If no allowed hosts are configured, deny all.
    if (config.AllowedHosts.empty()) {
        return false;
    }

    TString lowerHost;
    lowerHost.reserve(host.size());
    for (auto c : host) {
        lowerHost += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Check denied list first (takes precedence).
    for (const auto& denied : config.DeniedHosts) {
        // Lowercase the config entry for case-insensitive comparison.
        TString lowerDenied;
        lowerDenied.reserve(denied.size());
        for (auto c : denied) {
            lowerDenied += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowerDenied == lowerHost) {
            return false;
        }
    }

    // Check allowed list.
    for (const auto& allowed : config.AllowedHosts) {
        // Lowercase the config entry for case-insensitive comparison.
        TString lowerAllowed;
        lowerAllowed.reserve(allowed.size());
        for (auto c : allowed) {
            lowerAllowed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowerAllowed == lowerHost) {
            return true;
        }
        // Support wildcard subdomains: *.example.com matches foo.example.com
        if (lowerAllowed.StartsWith("*.")) {
            TString suffix = lowerAllowed.substr(1); // ".example.com"
            if (lowerHost.EndsWith(suffix) && lowerHost.size() > suffix.size()) {
                return true;
            }
        }
    }

    return false;
}

bool IsSensitiveHeader(TStringBuf headerName) {
    // List of headers that may contain sensitive information.
    static const TVector<TString> Sensitive = {
        "authorization",
        "cookie",
        "proxy-authorization",
        "x-api-key",
        "x-auth-token",
        "x-access-token",
    };
    
    TString lowerName;
    lowerName.reserve(headerName.size());
    for (auto c : headerName) {
        lowerName += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    for (const auto& sensitive : Sensitive) {
        if (lowerName == sensitive) {
            return true;
        }
    }
    return false;
}

TString RedactSensitiveHeaders(const NHttp::THeadersBuilder& headers) {
    TString result;
    bool first = true;
    for (const auto& [name, value] : headers.Data) {
        if (!first) {
            result += "; ";
        }
        first = false;
        result += name;
        result += ": ";
        if (IsSensitiveHeader(name)) {
            result += "[REDACTED]";
        } else {
            result += value;
        }
    }
    return result;
}

// TODO(Phase 6): ResolveAndPinHost is not wired into the egress actor.
// It exists as a stub for DNS rebinding protection, which is planned for Phase 6.
// The function uses a blocking getaddrinfo() call and must be replaced with an
// async DNS resolution through the HTTP proxy before being connected.
TString ResolveAndPinHost(TStringBuf host, const TEgressSecurityConfig& /* config */) {
    // If the host is already an IP address, return it directly.
    if (NHttp::IsIPv4(TString(host)) || NHttp::IsIPv6(TString(host))) {
        return TString(host);
    }

    // For hostname-based URLs, resolve the IP address.
    // WARNING: This uses a blocking getaddrinfo() call.
    // This function is NOT connected to the egress actor flow (Phase 6 task).
    // Do not rely on this for DNS rebinding protection in Phase 1.
    struct addrinfo hints, *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(TString(host).c_str(), nullptr, &hints, &result);
    if (ret != 0 || !result) {
        // Resolution failed — the proxy will also fail, but we can't pin.
        return {};
    }

    // Take the first resolved address and check if it's blocked.
    TString resolvedIP;
    for (auto* rp = result; rp; rp = rp->ai_next) {
        char addrStr[INET6_ADDRSTRLEN] = {0};
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, addrStr, sizeof(addrStr))) {
                resolvedIP = addrStr;
                break;
            }
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6* sa = reinterpret_cast<struct sockaddr_in6*>(rp->ai_addr);
            if (inet_ntop(AF_INET6, &sa->sin6_addr, addrStr, sizeof(addrStr))) {
                resolvedIP = addrStr;
                break;
            }
        }
    }
    freeaddrinfo(result);

    if (resolvedIP.empty()) {
        return {};
    }

    // Check if the resolved IP is blocked.
    if (IsBlockedIP(resolvedIP)) {
        return {};
    }

    return resolvedIP;
}

} // namespace NYql::NDq::NHttpEgress
