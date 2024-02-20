# Brainlab specific tests

TODO: Add description

## Dependencies

- python-docopt
- python-flask
- python-grpcio-tools

## Building and test excecution

### Regenerate gRPC/Protobuf definitions

```
git clone https://github.com/googleapis/googleapis
GOOGLEAPIS_DIR=<clone location>

protoc -I${GOOGLEAPIS_DIR} -Idata --include_imports --include_source_info --descriptor_set_out=data/service.pb data/service.proto
python -m grpc_tools.protoc -I=data --python_out=endpoints/grpc_client --grpc_python_out=endpoints/grpc_client data/service.proto
python -m grpc_tools.protoc -I=data --python_out=endpoints/grpc_server --grpc_python_out=endpoints/grpc_server data/service.proto
```

### Why is there no application for a http_client?

For now, sending an http request through envoy is out of scope since we testing gRPC <-> http1 conversion.
If there is a need to http requests (e.g. developing/testing the http server test application) just use curl.

```
curl -i -X POST -H 'Content-Type: application/json' -d '{"counter": 0}' 'http://<server_address>:<server_port>/testing.TestService/ping'
```
