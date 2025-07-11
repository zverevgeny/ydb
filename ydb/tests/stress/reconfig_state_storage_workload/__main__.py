# -*- coding: utf-8 -*-
import argparse
from ydb.tests.stress.common.common import YdbClient
from ydb.tests.stress.reconfig_state_storage_workload.workload import WorkloadRunner


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="state storage reconfiguration stability workload", formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--endpoint", default="localhost:2135", help="An endpoint to be used")
    parser.add_argument("--database", default="Root/test", help="A database to connect")
    parser.add_argument("--path", default="olap_workload", help="A path prefix for tables")
    parser.add_argument("--duration", default=10 ** 9, type=lambda x: int(x), help="A duration of workload in seconds.")
    args = parser.parse_args()
    client = YdbClient(args.endpoint, args.database, True)
    client.wait_connection()
    with WorkloadRunner(client, args.path, args.duration) as runner:
        runner.run()
