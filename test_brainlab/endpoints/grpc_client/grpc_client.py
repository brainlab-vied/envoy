#!/usr/bin/env python

"""Test client application using gRPC.

Usage:
    grpc_client.py ping [--times=<times>] [--hostname=<hostname>] [--port=<port>]
    grpc_client.py (-h | --help)

Options:
    --hostname=<hostname>   Set FQDN to connect to. [default: [::1]]
    --port=<port>           Set Port number to connect on. [default: 50051]
    --times=<times>         Number of times to execute given action. [default: 1]
    -h --help               Show this help screen.
"""

__author__ = "Simon Brummer <simon.brummer@zuehlke.com>"

import docopt
import grpc
import service_pb2
import service_pb2_grpc
import sys


class TestClient:
    def __init__(self, hostname: str, port: str):
        self.grpc_channel = grpc.insecure_channel(f"{hostname}:{port}")
        self.grpc_client = service_pb2_grpc.TestServiceStub(self.grpc_channel)

    def ping(self, times: int):
        for request_number in range(0, times):
            request = service_pb2.PingRequest(counter=request_number)
            print(f"Ping: Send '{ping_request_to_str(request)}'")

            response = self.grpc_client.ping(request)
            print(f"Ping: Received '{ping_response_to_str(response)}'")

            assert request.counter == response.ping.counter


def ping_request_to_str(request: service_pb2.PingRequest) -> str:
    return f"PingRequest(counter:{request.counter})"


def ping_response_to_str(response: service_pb2.PingResponse) -> str:
    return f"PingResponse(ping:{ping_request_to_str(response.ping)}, counter:{response.counter})"


def main():
    args = docopt.docopt(__doc__, version=None)
    client = TestClient(args["--hostname"], args["--port"])

    if args["ping"]:
        client.ping(int(args["--times"]))


if __name__ == "__main__":
    try:
        main()
        sys.exit()
    except Exception as ex:
        sys.exit(f'An error occurrred, returning an error code. Error was: {ex}')
