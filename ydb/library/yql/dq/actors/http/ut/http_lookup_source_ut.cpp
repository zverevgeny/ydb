#include <ydb/library/yql/dq/actors/http/http_url_encode.h>
#include <ydb/library/yql/dq/actors/http/http_lookup_source_factory.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io_factory.h>
#include <ydb/library/yql/dq/proto/http_lookup.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/string.h>

namespace NYql::NDq::NHttpEgress {

namespace {

// Build a minimal settings proto for testing.
Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings MakeSettings(
    TString method = "GET",
    TString endpoint = "http://example.com",
    TString pathTemplate = "/lookup/{key}",
    TString bodyTemplate = "",
    ui64 timeoutMs = 5000,
    ui64 maxBatchSize = 100,
    ui64 maxResponseSize = 1024 * 1024,
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CachePolicy cachePolicy =
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_NONE)
{
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings;
    settings.set_method(std::move(method));
    settings.set_endpoint(std::move(endpoint));
    settings.set_path_template(std::move(pathTemplate));
    settings.set_body_template(std::move(bodyTemplate));
    settings.set_timeout_ms(timeoutMs);
    settings.set_max_batch_size(maxBatchSize);
    settings.set_max_response_size(maxResponseSize);
    settings.set_cache_policy(cachePolicy);
    return settings;
}

} // namespace

Y_UNIT_TEST_SUITE(HttpLookupSourceTests) {

// ===== UrlEncode Tests =====

Y_UNIT_TEST(TestUrlEncodeSafeChars) {
    // Safe characters should pass through unchanged.
    UNIT_ASSERT_EQUAL(UrlEncode("abc123"), "abc123");
    UNIT_ASSERT_EQUAL(UrlEncode("hello-world"), "hello-world");
    UNIT_ASSERT_EQUAL(UrlEncode("test_value"), "test_value");
    UNIT_ASSERT_EQUAL(UrlEncode("a.b~c"), "a.b~c");
}

Y_UNIT_TEST(TestUrlEncodeSpecialChars) {
    // Special characters should be percent-encoded.
    UNIT_ASSERT_EQUAL(UrlEncode("hello world"), "hello%20world");
    UNIT_ASSERT_EQUAL(UrlEncode("a/b"), "a%2Fb");
    UNIT_ASSERT_EQUAL(UrlEncode("a?b"), "a%3Fb");
    UNIT_ASSERT_EQUAL(UrlEncode("a&b"), "a%26b");
    UNIT_ASSERT_EQUAL(UrlEncode("a=b"), "a%3Db");
    UNIT_ASSERT_EQUAL(UrlEncode("a#b"), "a%23b");
    UNIT_ASSERT_EQUAL(UrlEncode("a+b"), "a%2Bb");
    UNIT_ASSERT_EQUAL(UrlEncode("a%b"), "a%25b");
}

Y_UNIT_TEST(TestUrlEncodeEmptyString) {
    UNIT_ASSERT_EQUAL(UrlEncode(""), "");
}

Y_UNIT_TEST(TestUrlEncodeUnicode) {
    // Multi-byte UTF-8 should be encoded byte-by-byte.
    UNIT_ASSERT_EQUAL(UrlEncode("\xC3\xA9"), "%C3%A9"); // é
}

// ===== Factory Registration Tests =====

Y_UNIT_TEST(TestFactoryRegistration) {
    NDq::TDqAsyncIoFactory factory;
    RegisterHttpLookupSourceFactory(factory);

    // Verify that the "HttpLookup" type is registered by attempting to create.
    // We can't fully test creation without MKQL infra, but we can verify
    // the registration didn't throw.
}

// ===== Proto Settings Tests =====

Y_UNIT_TEST(TestProtoSettingsDefaults) {
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings;

    UNIT_ASSERT_EQUAL(settings.method(), "");
    UNIT_ASSERT_EQUAL(settings.endpoint(), "");
    UNIT_ASSERT_EQUAL(settings.path_template(), "");
    UNIT_ASSERT_EQUAL(settings.body_template(), "");
    UNIT_ASSERT_EQUAL(settings.timeout_ms(), 0u);
    UNIT_ASSERT_EQUAL(settings.max_batch_size(), 0u);
    UNIT_ASSERT_EQUAL(settings.max_response_size(), 0u);
    UNIT_ASSERT_EQUAL(settings.cache_policy(),
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_NONE);
    UNIT_ASSERT_EQUAL(settings.cache_ttl_seconds(), 0u);
    UNIT_ASSERT_EQUAL(settings.auth_token_secret_name(), "");
    UNIT_ASSERT_FALSE(settings.has_egress_settings());
}

Y_UNIT_TEST(TestProtoSettingsWithValues) {
    auto settings = MakeSettings(
        "POST",
        "http://api.example.com/v1",
        "/lookup/{key}",
        "{\"key\": \"{key}\"}",
        30000,
        200,
        2 * 1024 * 1024,
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES);
    settings.set_cache_ttl_seconds(300);
    settings.set_auth_token_secret_name("my_token");

    UNIT_ASSERT_EQUAL(settings.method(), "POST");
    UNIT_ASSERT_EQUAL(settings.endpoint(), "http://api.example.com/v1");
    UNIT_ASSERT_EQUAL(settings.path_template(), "/lookup/{key}");
    UNIT_ASSERT_EQUAL(settings.body_template(), "{\"key\": \"{key}\"}");
    UNIT_ASSERT_EQUAL(settings.timeout_ms(), 30000u);
    UNIT_ASSERT_EQUAL(settings.max_batch_size(), 200u);
    UNIT_ASSERT_EQUAL(settings.max_response_size(), 2u * 1024 * 1024);
    UNIT_ASSERT_EQUAL(settings.cache_policy(),
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES);
    UNIT_ASSERT_EQUAL(settings.cache_ttl_seconds(), 300u);
    UNIT_ASSERT_EQUAL(settings.auth_token_secret_name(), "my_token");
}

Y_UNIT_TEST(TestProtoSettingsHeaders) {
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings;
    auto* header1 = settings.add_headers();
    header1->set_name("Content-Type");
    header1->set_value("application/json");
    auto* header2 = settings.add_headers();
    header2->set_name("X-Custom");
    header2->set_value("test");

    UNIT_ASSERT_EQUAL(settings.headers_size(), 2);
    UNIT_ASSERT_EQUAL(settings.headers(0).name(), "Content-Type");
    UNIT_ASSERT_EQUAL(settings.headers(0).value(), "application/json");
    UNIT_ASSERT_EQUAL(settings.headers(1).name(), "X-Custom");
    UNIT_ASSERT_EQUAL(settings.headers(1).value(), "test");
}

Y_UNIT_TEST(TestProtoSettingsEgressConfig) {
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings;
    auto* egress = settings.mutable_egress_settings();
    egress->add_allowed_hosts("api.example.com");
    egress->add_denied_hosts("internal.local");
    egress->set_max_request_body_size(1024 * 1024);
    egress->set_max_response_body_size(10 * 1024 * 1024);

    UNIT_ASSERT_TRUE(settings.has_egress_settings());
    UNIT_ASSERT_EQUAL(settings.egress_settings().allowed_hosts_size(), 1);
    UNIT_ASSERT_EQUAL(settings.egress_settings().allowed_hosts(0), "api.example.com");
    UNIT_ASSERT_EQUAL(settings.egress_settings().denied_hosts_size(), 1);
    UNIT_ASSERT_EQUAL(settings.egress_settings().denied_hosts(0), "internal.local");
    UNIT_ASSERT_EQUAL(settings.egress_settings().max_request_body_size(), 1024u * 1024);
    UNIT_ASSERT_EQUAL(settings.egress_settings().max_response_body_size(), 10u * 1024 * 1024);
}

} // Y_UNIT_TEST_SUITE(HttpLookupSourceTests)

} // namespace NYql::NDq::NHttpEgress
