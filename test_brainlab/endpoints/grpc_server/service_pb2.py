# -*- coding: utf-8 -*-
# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: service.proto
# Protobuf Python Version: 4.25.2
"""Generated protocol buffer code."""
from google.protobuf import descriptor as _descriptor
from google.protobuf import descriptor_pool as _descriptor_pool
from google.protobuf import symbol_database as _symbol_database
from google.protobuf.internal import builder as _builder
# @@protoc_insertion_point(imports)

_sym_db = _symbol_database.Default()


from google.api import annotations_pb2 as google_dot_api_dot_annotations__pb2


DESCRIPTOR = _descriptor_pool.Default().AddSerializedFile(b'\n\rservice.proto\x12\x07testing\x1a\x1cgoogle/api/annotations.proto\"\x1e\n\x0bPingRequest\x12\x0f\n\x07\x63ounter\x18\x01 \x01(\r\"C\n\x0cPingResponse\x12\"\n\x04ping\x18\x01 \x01(\x0b\x32\x14.testing.PingRequest\x12\x0f\n\x07\x63ounter\x18\x02 \x01(\r2T\n\x0bTestService\x12\x45\n\x04ping\x12\x14.testing.PingRequest\x1a\x15.testing.PingResponse\"\x10\x82\xd3\xe4\x93\x02\n\"\x05/ping:\x01*b\x06proto3')

_globals = globals()
_builder.BuildMessageAndEnumDescriptors(DESCRIPTOR, _globals)
_builder.BuildTopDescriptorsAndMessages(DESCRIPTOR, 'service_pb2', _globals)
if _descriptor._USE_C_DESCRIPTORS == False:
  DESCRIPTOR._options = None
  _globals['_TESTSERVICE'].methods_by_name['ping']._options = None
  _globals['_TESTSERVICE'].methods_by_name['ping']._serialized_options = b'\202\323\344\223\002\n\"\005/ping:\001*'
  _globals['_PINGREQUEST']._serialized_start=56
  _globals['_PINGREQUEST']._serialized_end=86
  _globals['_PINGRESPONSE']._serialized_start=88
  _globals['_PINGRESPONSE']._serialized_end=155
  _globals['_TESTSERVICE']._serialized_start=157
  _globals['_TESTSERVICE']._serialized_end=241
# @@protoc_insertion_point(module_scope)
