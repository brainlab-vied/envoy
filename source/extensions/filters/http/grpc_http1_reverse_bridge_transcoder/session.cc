#include <vector>
#include "session.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {
namespace {

// Since it is hard to tell then a session will not respond anymore
// we define a rather large threshold just to be on the safe side.
auto const cleanup_threshold = std::chrono::minutes(5);
} // namespace

SessionGuard::SessionGuard(SessionMap& sessions)
    : sessions_{sessions}, used_sessions_{}, keep_alive_{false} {}

SessionGuard::~SessionGuard() { onDestroy(); }

absl::StatusOr<Session* const> SessionGuard::createSession(SessionId sid) {
  auto [pos, ok] = sessions_.insert({sid, Session{}});
  if (!ok) {
    return absl::FailedPreconditionError(
        absl::StrCat("Session with sid ", sid, " already exists."));
  }

  // Initialize session after successful insertion.
  auto* const session = &(pos->second);
  session->id = sid;
  session->last_access = SessionClock::now();
  session->decoder_headers = nullptr;
  session->decoder_data = Buffer::OwnedImpl();
  session->encoder_headers = nullptr;
  session->encoder_data = Buffer::OwnedImpl();

  // Memorize id: The previous lookup ensures uniqueness and existence.
  used_sessions_.insert(session->id);
  return session;
}

absl::StatusOr<Session* const> SessionGuard::lookupSession(SessionId sid) {
  auto pos = sessions_.find(sid);
  if (pos == sessions_.cend()) {
    return absl::NotFoundError(absl::StrCat("Failed to lookup session with id ", sid));
  }

  auto* const session = &(pos->second);
  used_sessions_.insert(session->id);

  // Update last access time
  session->last_access = SessionClock::now();
  return session;
}

void SessionGuard::keepAccessedSessionsAlive() { keep_alive_ = true; }

void SessionGuard::onDestroy() {
  // Cleanup all used sessions if not keep alive explicitly.
  if (!keep_alive_) {
    for (auto const id : used_sessions_) {
      sessions_.erase(id);
    }
  }

  // Cleanup all sessions that are not accessed anymore
  auto const now = SessionClock::now();
  std::vector<SessionId> sessions_to_cleanup;

  for (auto const& [id, session] : sessions_) {
    auto const duration = now - session.last_access;

    if (cleanup_threshold < std::chrono::duration_cast<decltype(cleanup_threshold)>(duration)) {
      sessions_to_cleanup.push_back(id);
    }
  }

  for (auto const& id : sessions_to_cleanup) {
    sessions_.erase(id);
  }
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
