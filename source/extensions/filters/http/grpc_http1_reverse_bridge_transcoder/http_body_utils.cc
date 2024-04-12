#include "http_body_utils.h"
#include "absl/status/status.h"
#include "source/common/grpc/codec.h"
#include "source/common/buffer/zero_copy_input_stream_impl.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder::HttpBodyUtils {
namespace {
using Envoy::Protobuf::io::CodedInputStream;
using Envoy::Protobuf::io::CodedOutputStream;
using Envoy::Protobuf::io::StringOutputStream;
using Envoy::Protobuf::io::ZeroCopyInputStream;

constexpr uint32_t ProtobufLengthDelimitedField = 2;

bool parseMessageByFieldPath(CodedInputStream* input,
                             absl::Span<const ProtobufWkt::Field* const> field_path,
                             Protobuf::Message* message) {
  // Warning: This is recursive protobuf parsing black magic taken from
  // ../grpc_json_transcoder/http_body_utils.cc. It seems to work, but nobody really knows how.
  // If you change any of this, be aware that you are on your own, so think twice if changes are
  // necessary before doing....
  if (field_path.empty()) {
    return message->MergeFromCodedStream(input);
  }

  const uint32_t expected_tag = (field_path.front()->number() << 3) | ProtobufLengthDelimitedField;
  for (;;) {
    const uint32_t tag = input->ReadTag();
    if (tag == expected_tag) {
      uint32_t length = 0;
      if (!input->ReadVarint32(&length)) {
        return false;
      }
      auto limit = input->IncrementRecursionDepthAndPushLimit(length);
      if (!parseMessageByFieldPath(input, field_path.subspan(1), message)) {
        return false;
      }
      if (!input->DecrementRecursionDepthAndPopLimit(limit.first)) {
        return false;
      }
    } else if (tag == 0) {
      return true;
    } else {
      if (!Protobuf::internal::WireFormatLite::SkipField(input, tag)) {
        return false;
      }
    }
  }
}

std::string appendHttpBodyEnvelope(const std::vector<const ProtobufWkt::Field*>& field_path,
                                   std::string content_type, uint64_t content_length) {
  // Warning: This is serialization black magic taken from
  // ../grpc_json_transcoder/http_body_utils.cc with some slight modifications. It seems to work,
  // but nobody really knows how. If you change any of this, be aware that you are on your own, so
  // think twice if changes are necessary before doing....

  // Manually encode the protobuf envelope for the body.
  // See https://developers.google.com/protocol-buffers/docs/encoding#embedded for wire format.

  std::string proto_envelope;
  {
    // For memory safety, the StringOutputStream needs to be destroyed before
    // we read the string.

    const uint32_t http_body_field_number =
        (google::api::HttpBody::kDataFieldNumber << 3) | ProtobufLengthDelimitedField;

    ::google::api::HttpBody body;
    body.set_content_type(std::move(content_type));

    uint64_t envelope_size = body.ByteSizeLong() +
                             CodedOutputStream::VarintSize32(http_body_field_number) +
                             CodedOutputStream::VarintSize64(content_length);
    std::vector<uint32_t> message_sizes;
    message_sizes.reserve(field_path.size());
    for (auto it = field_path.rbegin(); it != field_path.rend(); ++it) {
      const ProtobufWkt::Field* field = *it;
      const uint64_t message_size = envelope_size + content_length;
      const uint32_t field_number = (field->number() << 3) | ProtobufLengthDelimitedField;
      const uint64_t field_size = CodedOutputStream::VarintSize32(field_number) +
                                  CodedOutputStream::VarintSize64(message_size);
      message_sizes.push_back(message_size);
      envelope_size += field_size;
    }
    std::reverse(message_sizes.begin(), message_sizes.end());

    proto_envelope.reserve(envelope_size);

    StringOutputStream string_stream(&proto_envelope);
    CodedOutputStream coded_stream(&string_stream);

    // Serialize body field definition manually to avoid the copy of the body.
    for (size_t i = 0; i < field_path.size(); ++i) {
      const ProtobufWkt::Field* field = field_path[i];
      const uint32_t field_number = (field->number() << 3) | ProtobufLengthDelimitedField;
      const uint64_t message_size = message_sizes[i];
      coded_stream.WriteTag(field_number);
      coded_stream.WriteVarint64(message_size);
    }
    body.SerializeToCodedStream(&coded_stream);
    coded_stream.WriteTag(http_body_field_number);
    coded_stream.WriteVarint64(content_length);
  }
  return proto_envelope;
}
} // namespace

absl::StatusOr<HttpBody> parseByMessageFields(Buffer::Instance& buffer,
                                              ProtoMessageFields const& field_path) {
  HttpBody body_message;
  Grpc::Decoder decoder;
  std::vector<Grpc::Frame> frames;

  if (!decoder.decode(buffer, frames)) {
    return absl::InvalidArgumentError("Failed to decode Buffer into gRPC Frames.");
  }

  if (frames.empty()) {
    return absl::InvalidArgumentError("Buffer does not contain gRPC Frames.");
  }

  for (auto& frame : frames) {
    Buffer::ZeroCopyInputStreamImpl stream(std::move(frame.data_));
    CodedInputStream input(&stream);
    input.SetRecursionLimit(field_path.size());

    if (!parseMessageByFieldPath(&input, absl::MakeConstSpan(field_path), &body_message)) {
      return absl::InvalidArgumentError("Unable to parse buffer content. Abort.");
    }
    return body_message;
  }
  return absl::InvalidArgumentError("Buffer did not contain HTTP Body message. Abort.");
}

// TODO: Improve ownership ownership?
absl::StatusOr<std::string> serializeByMessageFields(HttpBody const& http_body_data,
                                                     ProtoMessageFields const& field_path) {
  const auto& body_data = http_body_data.data();
  auto data = appendHttpBodyEnvelope(field_path, http_body_data.content_type(), body_data.size());
  if (data.empty()) {
    return absl::InvalidArgumentError("Failed to create gRPC Wire format for HttpBody Message.");
  }

  data.append(body_data.cbegin(), body_data.cend());
  return data;
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder::HttpBodyUtils
