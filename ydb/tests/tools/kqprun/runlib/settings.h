#pragma once

#include <util/generic/string.h>

namespace NKikimrRun {

struct TAsyncQueriesSettings {
    enum class EVerbose {
        EachQuery,
        Final,
    };

    ui64 InFlightLimit = 0;
    EVerbose Verbose = EVerbose::EachQuery;
};

struct TServerSettings {
    TString DomainName = "Root";

    bool MonitoringEnabled = false;
    ui16 FirstMonitoringPort = 0;

    bool GrpcEnabled = false;
    ui16 FirstGrpcPort = 0;

    TString LogOutputFile;
};

enum class EResultOutputFormat {
    RowsJson,  // Rows in json format
    FullJson,  // Columns, rows and types in json format
    FullProto,  // Columns, rows and types in proto string format
};

}  // namespace NKikimrRun
