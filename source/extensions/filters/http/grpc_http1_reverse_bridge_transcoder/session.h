#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

#include "absl/status/statusor.h"

#include "envoy/http/filter.h"
#include "source/common/buffer/buffer_impl.h"
#include "http_methods.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

/// Type representing a unique session Id
using SessionId = uint64_t;

/// Clock type used to determine session timestamps
using SessionClock = std::chrono::system_clock;

/// Type representing timestamp to determine if a session
/// timed out / never go a response.
using SessionTimestamp = std::chrono::time_point<SessionClock>;

/// Type representing a filter session to that shall be transcoded.
struct Session {
  SessionId id;
  SessionTimestamp last_access;
  HttpMethodAndPath method_and_path;
  Http::RequestHeaderMap* decoder_headers;
  Buffer::OwnedImpl decoder_data;
  Http::ResponseHeaderMap* encoder_headers;
  Buffer::OwnedImpl encoder_data;
};

/// Map containing sessions
using SessionMap = std::unordered_map<SessionId, Session>;

/// RAII Class managing the lifetime of sessions.
class SessionGuard {
  using SessionIds = std::unordered_set<SessionId>;

public:
  /**
   * CTOR
   * @params[in] sessions Reference to session map managing session storage.
   */
  explicit SessionGuard(SessionMap& sessions);

  /**
   * DTOR: Deletes all accessed session since guard constructions.
   * calling keepAccessedSessionAlive prevents the cleanup.
   */
  ~SessionGuard();

  /**
   * Create new session.
   * @params[in] sid session id of the new connection.
   * @returns on success a pointer to the session.
   *          on failure a status containing the error.
   */
  absl::StatusOr<Session* const> createSession(SessionId sid);


  /**
   * Lookup an existing session.
   * @params[in] sid session id of the connection to find.
   * @returns on success a pointer to the session.
   *          on failure a status containing the error.
   */
  absl::StatusOr<Session* const> lookupSession(SessionId sid);

  /**
   * Prevents deletion of accessed sessions (created or lookup) on
   * destructor execution. Call this if you want to keep the session 
   * alive beyond the scope of a method.
   */
  void keepAccessedSessionsAlive();

private:
  void onDestroy();

private:
  SessionMap& sessions_;
  SessionIds used_sessions_;
  bool keep_alive_;
};

} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
