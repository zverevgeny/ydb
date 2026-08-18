#pragma once

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io_factory.h>

namespace NYql::NDq {

// Registers the HTTP lookup source factory in TDqAsyncIoFactory.
// This allows queries to use "HttpLookup" as the lookup provider name.
void RegisterHttpLookupSourceFactory(TDqAsyncIoFactory& factory);

} // namespace NYql::NDq
