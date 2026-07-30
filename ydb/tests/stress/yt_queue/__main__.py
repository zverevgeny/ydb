# -*- coding: utf-8 -*-
import argparse
from ydb.tests.stress.yt_queue.workload import Workload
import logging

logger = logging.getLogger("logger")

if __name__ == '__main__':

    logging.basicConfig(
        format='%(asctime)s,%(msecs)d %(name)s %(levelname)s %(message)s',
        datefmt='%H:%M:%S',
        level=logging.INFO)

    text = """\033[92mYT queue external data source workload\x1b[0m"""
    parser = argparse.ArgumentParser(description=text, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--endpoint', default='localhost:2135', type=str, help="An endpoint to be used")
    parser.add_argument('--database', default=None, type=str, required=True, help='A database to connect')
    parser.add_argument('--duration', default=60, type=int, help='A duration of workload in seconds.')
    parser.add_argument('--prefix', default='yt_queue_stress', type=str, help='External data source name prefix')
    parser.add_argument('--yt-endpoint', default='', type=str, help='YTsaurus cluster endpoint (optional)')
    parser.add_argument('--yt-token', default='', type=str, help='YTsaurus token (optional)')
    parser.add_argument('--yt-queue', default='', type=str, help='YT queue path to read from (optional)')
    args = parser.parse_args()

    with Workload(
        args.endpoint,
        args.database,
        args.duration,
        args.prefix,
        args.yt_endpoint,
        args.yt_token,
        args.yt_queue,
    ) as workload:
        workload.loop()
