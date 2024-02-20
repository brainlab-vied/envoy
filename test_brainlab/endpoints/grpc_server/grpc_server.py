#!/usr/bin/env python

"""Test server application using gRPC.

This server is supports the debugging/development of the "grpc_client" program.
By having a gRPC based server, it is easier to ensure that the client works as intended.

Usage:
    grpc_server.py [--address=<address>] [--port=<port>]
    grpc_server.py (-h | --help)

Options:
    --address=<address>   Set IP address to listen on. [default: [::]]
    --port=<port>         Set Port number to listen on. [default: 50051]
    -h --help             Show this help screen.
"""

__author__ = "Simon Brummer <simon.brummer@zuehlke.com>"

import concurrent
import docopt
import grpc
import service_pb2
import service_pb2_grpc
import sys


class TestService(service_pb2_grpc.TestServiceServicer):
    def __init__(self):
        self.request_counter = 0

    def ping(
        self, request: service_pb2.PingRequest, _context
    ) -> service_pb2.PingResponse:
        print(f"Ping: Received '{ping_request_to_str(request)}'")
        response = service_pb2.PingResponse(ping=request, counter=self.request_counter)

        print(f"Ping: Send '{ping_response_to_str(response)}'")
        self.request_counter += 1
        return response


class TestServer:
    def __init__(self, address: str, port: str):
        self.address = address
        self.port = port
        self.server = grpc.server(concurrent.futures.ThreadPoolExecutor(max_workers=1))

    def add_service(self, service):
        service_pb2_grpc.add_TestServiceServicer_to_server(service, self.server)

    def run(self):
        print(f"Server on '{self.address}:{self.port}'. Press CTRL-C to stop.")
        self.server.add_insecure_port(f"{self.address}:{self.port}")
        self.server.start()
        self.server.wait_for_termination()


def ping_request_to_str(request: service_pb2.PingRequest) -> str:
    return f"PingRequest(counter:{request.counter})"


def ping_response_to_str(response: service_pb2.PingResponse) -> str:
    return f"PingResponse(ping:{ping_request_to_str(response.ping)}, counter:{response.counter})"


def main():
    args = docopt.docopt(__doc__, version=None)
    server = TestServer(args["--address"], args["--port"])
    server.add_service(TestService())
    server.run()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print('Received CTRL-C. Shutting down server.')
        sys.exit()
    except Exception as ex:
        sys.exit(f'An error occurrred, returning an error code. Error was: {ex}')
