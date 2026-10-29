#pragma once
#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/session_window.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace openxmb::integrations {
// A pinned, read-only identity. It cannot be converted into OwnedProcess and
// has no termination operation. A close request addresses one validated window.
class ObservedProcess {
public:
  struct State;
  [[nodiscard]] std::int64_t id() const noexcept;
  [[nodiscard]] std::uint64_t creation_time() const noexcept;
  [[nodiscard]] ProcessObservation observe() const;
  [[nodiscard]] std::optional<SessionWindowTarget> window() const;
  [[nodiscard]] Result request_close() const;
private:
  explicit ObservedProcess(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
  friend class ExecutableObservation;
};
enum class ExecutableMatchState { pending, matched, rejected, unavailable };
struct ExecutableMatch {
  ExecutableMatchState state{ExecutableMatchState::pending};
  std::shared_ptr<ObservedProcess> process;
  std::string message;
};
// Construct before forwarding the launch. Exact mode requires this canonical
// image. Directory mode requires a safe installation root, a strictly contained
// canonical image and one validated game window. Both require a new, same-user
// and same-session identity. Ambiguous or preexisting games fail closed.
// Unrelated inaccessible processes cannot become directory-mode candidates.
// Poll on a worker, never the UI thread.
class ExecutableObservation {
public:
  explicit ExecutableObservation(const std::filesystem::path& target,bool installation_directory=false);
  ~ExecutableObservation();
  ExecutableObservation(const ExecutableObservation&)=delete;
  ExecutableObservation& operator=(const ExecutableObservation&)=delete;
  void exclude_process(std::int64_t pid);
  // Cancellation applies only to discovery. Once matched, every poll returns
  // the same pinned capability, including after its process exits.
  [[nodiscard]] ExecutableMatch poll(std::stop_token stop = {});
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::integrations
