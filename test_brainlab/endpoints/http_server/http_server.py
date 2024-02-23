#!/usr/bin/env python

"""Test server application using http/REST.

Usage:
    http_server.py [--address=<address>] [--port=<port>]
    http_server.py (-h | --help)

Options:
    --address=<address>   Set IP address to listen on. [default: [::]]
    --port=<port>         Set Port number to listen on. [default: 80]
    -h --help             Show this help screen.
"""

__author__ = "Simon Brummer <simon.brummer@zuehlke.com>"

import docopt
import flask
import sys
import http

server = flask.Flask(__name__)
request_counter = 0


@server.route("/testing.TestService/ping", methods=[http.HTTPMethod.POST])
def ping():
    # Note: For expected data types and formats see: ../../data/service.proto
    print("Ping. Received Request:")
    print(f"Headers:\n{flask.request.headers}")
    print(f"Data:\n{flask.request.data}")

    # Verify received request
    if flask.request.headers.get("content_type") != "application/json":
        msg = "Unsupported content type. Content type must be application/json."
        status = http.HTTPStatus.NOT_IMPLEMENTED
        print(f'An error occurred "{msg}" return HTTPStatus "{status}"')
        return (flask.jsonify({}), status)

    request_as_json = flask.request.get_json()
    if len(request_as_json) != 1:
        msg = "Malformed request. JSON must a single entry."
        status = http.HTTPStatus.BAD_REQUEST
        print(f'An error occurred "{msg}" return HTTPStatus "{status}"')
        return (flask.jsonify({}), status)

    try:
        int(request_as_json["counter"])
    except KeyError:
        msg = "Malformed request. JSON contains no key counter."
        status = http.HTTPStatus.BAD_REQUEST
        print(f'An error occurred "{msg}" return HTTPStatus "{status}"')
        return (flask.jsonify({}), status)

    except ValueError:
        msg = "Malformed request. Given counter is no number."
        status = http.HTTPStatus.BAD_REQUEST
        print(f'An error occurred "{msg}" return HTTPStatus "{status}"')
        return (flask.jsonify({}), status)

    # Everything is fine. Construct proper response.
    global request_counter
    response = {"ping": request_as_json, "counter": request_counter}
    request_counter += 1

    print(f"Ping. Send: {response}")
    return (flask.jsonify(response), http.HTTPStatus.OK)


def main():
    args = docopt.docopt(__doc__, version=None)
    host = args["--address"]
    port = args["--port"]
    print(f"Server on '{host}:{port}'. Press CTRL-C to stop.")

    # Note: Flask adds automatically brackets around IPv6 addresses. Strip them
    server.run(host=host.strip("[]"), port=port)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Received CTRL-C. Shutting down server.")
        sys.exit()
    except Exception as ex:
        sys.exit(f"An error occurrred, returning an error code. Error was: {ex}")
