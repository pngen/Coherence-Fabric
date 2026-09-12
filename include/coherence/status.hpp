// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Stable machine-readable status and error semantics.
//
// Failure causes that carry different operational meaning are never flattened
// into a generic false/exception/string. Every status code below maps to a
// distinct cause that a caller is expected to act on differently.
#ifndef COHERENCE_STATUS_HPP
#define COHERENCE_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "coherence/export.hpp"

namespace coherence {

enum class StatusCode : std::uint16_t {
  Ok = 0,

  // --- caller / input problems ---------------------------------------------
  InvalidArgument = 1,
  InvalidState = 2,
  InvalidTransition = 3,
  Unsupported = 4,
  CapacityExceeded = 5,
  ResourceExhausted = 6,
  DuplicateIdentity = 7,

  // --- unknown referents ----------------------------------------------------
  UnknownDomain = 20,
  UnknownObject = 21,
  UnknownRegion = 22,
  UnknownParticipant = 23,
  UnknownPolicy = 24,
  UnknownPublication = 25,
  UnknownInvalidation = 26,
  UnknownSyncOperation = 27,
  UnknownRequest = 28,

  // --- stale identity rejection --------------------------------------------
  StaleEpoch = 40,
  StaleBoot = 41,
  StaleObjectGeneration = 42,
  StaleRegionGeneration = 43,
  StaleReplicaGeneration = 44,
  StaleOwnership = 45,
  StalePolicy = 46,
  StalePublication = 47,
  StaleInvalidation = 48,
  StaleSyncOperation = 49,
  StaleSession = 50,
  ReplayedRequest = 51,

  // --- authority ------------------------------------------------------------
  NotAuthoritative = 60,
  ReadNotCurrent = 61,
  WriteConflict = 62,
  ExclusiveWriterConflict = 63,
  NotWriteAuthorized = 64,
  Fenced = 65,
  Retired = 66,

  // --- required work not yet performed --------------------------------------
  SyncRequired = 80,
  RevalidationRequired = 81,
  InvalidationOutstanding = 82,
  EvidenceMissing = 83,
  EvidenceStale = 84,
  DirtyUnpublished = 85,
  DirtyLost = 86,
  RecoveryRequired = 87,
  Quiescing = 88,
  ShuttingDown = 89,

  // --- truthful ambiguity ---------------------------------------------------
  OutcomeUnknown = 100,
  ContentMismatch = 101,

  // --- integrity / transport / persistence ---------------------------------
  IntegrityFailure = 120,
  CorruptionDetected = 121,
  TruncatedInput = 122,
  TrailingGarbage = 123,
  OversizedInput = 124,
  ProtocolViolation = 125,
  UnsupportedSchema = 126,
  PersistenceFailure = 127,
  TransportFailure = 128,
  ConnectionClosed = 129,
  Timeout = 130,

  // --- internal -------------------------------------------------------------
  InternalInvariantViolation = 140,
  InternalError = 141,
};

/// Stable lowercase-with-underscores token. Suitable for machine consumption
/// and for test expectations; these strings are part of the public contract.
COHERENCE_API std::string_view status_code_name(StatusCode code) noexcept;

/// True when the code denotes a stale-identity rejection. Useful for tests and
/// for callers that must retry with refreshed generations.
COHERENCE_API bool is_stale_code(StatusCode code) noexcept;

/// Classification used by callers that must fail closed. Any code other than
/// Ok is a failure; this helper only groups failures.
enum class StatusSeverity : std::uint8_t {
  Success = 0,
  Transient = 1,   ///< retry may succeed after refresh (e.g. StaleEpoch)
  Conflict = 2,    ///< policy/authority conflict, retry needs a different actor
  Permanent = 3,   ///< the request can never succeed as issued
  Integrity = 4,   ///< data or protocol integrity was violated
  Internal = 5,    ///< an invariant of the runtime itself was violated
};

COHERENCE_API StatusSeverity status_severity(StatusCode code) noexcept;

class COHERENCE_API Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message);
  Status(StatusCode code, std::string message, std::string detail);

  [[nodiscard]] static Status success() noexcept { return Status(); }
  [[nodiscard]] static Status make(StatusCode code, std::string message) {
    return Status(code, std::move(message));
  }
  [[nodiscard]] static Status make(StatusCode code, std::string message, std::string detail) {
    return Status(code, std::move(message), std::move(detail));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] StatusSeverity severity() const noexcept { return status_severity(code_); }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// Deterministic single-line rendering: "code: message (detail)".
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_ && a.detail_ == b.detail_;
  }

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
  std::string detail_;
};

/// Convenience constructor helpers that keep call sites terse and uniform.
#define COHERENCE_STATUS(code, msg) ::coherence::Status(::coherence::StatusCode::code, (msg))
#define COHERENCE_STATUS_D(code, msg, detail) \
  ::coherence::Status(::coherence::StatusCode::code, (msg), (detail))

/// Result carrier. Exactly one of a value or a failure Status is present.
///
/// The type deliberately has no implicit conversion to the value type so that
/// a failure can never be consumed as if it were a successful result.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                    // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}            // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Result success(T value) { return Result(std::move(value)); }
  [[nodiscard]] static Result failure(Status status) { return Result(std::move(status)); }

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const noexcept { return *value_; }
  [[nodiscard]] T& value() noexcept { return *value_; }
  [[nodiscard]] T&& take() noexcept { return std::move(*value_); }

  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

  [[nodiscard]] T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

  [[nodiscard]] const T& operator*() const noexcept { return *value_; }
  [[nodiscard]] T& operator*() noexcept { return *value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &*value_; }
  [[nodiscard]] T* operator->() noexcept { return &*value_; }

 private:
  std::optional<T> value_;
  Status status_{};
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Result success() { return Result(); }
  [[nodiscard]] static Result failure(Status status) { return Result(std::move(status)); }

  [[nodiscard]] bool has_value() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

 private:
  Status status_{};
};

using VoidResult = Result<void>;

/// Propagate a failure from an inner Result and return it from the enclosing
/// function. Keeps validation chains readable while ensuring nothing is
/// silently ignored.
#define COHERENCE_TRY(expr)                                        \
  do {                                                             \
    auto coherence_try_result = (expr);                            \
    if (!coherence_try_result.has_value()) {                       \
      return ::coherence::Result<void>::failure(                   \
          coherence_try_result.status());                          \
    }                                                              \
  } while (false)

} // namespace coherence

#endif // COHERENCE_STATUS_HPP
