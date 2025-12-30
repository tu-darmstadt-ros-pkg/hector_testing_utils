#ifndef HECTOR_TESTING_UTILS_HECTOR_TESTING_UTILS_HPP
#define HECTOR_TESTING_UTILS_HECTOR_TESTING_UTILS_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

namespace hector_testing_utils
{

constexpr std::chrono::milliseconds kDefaultSpinPeriod{5};
constexpr std::chrono::seconds kDefaultTimeout{10};

/// Executor helper that keeps spinning while waiting on test conditions.
class TestExecutor
{
public:
  TestExecutor() = default;

  void add_node(const rclcpp::Node::SharedPtr &node) { executor_.add_node(node); }
  void remove_node(const rclcpp::Node::SharedPtr &node) { executor_.remove_node(node); }

  /// Spin until predicate returns true or timeout expires.
  bool spin_until(
    const std::function<bool()> &predicate,
    std::chrono::nanoseconds timeout,
    std::chrono::nanoseconds spin_period = kDefaultSpinPeriod)
  {
    if (timeout <= std::chrono::nanoseconds(0)) {
      executor_.spin_some();
      return predicate();
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (rclcpp::ok() && std::chrono::steady_clock::now() < deadline) {
      executor_.spin_some();
      if (predicate()) {
        return true;
      }
      if (spin_period > std::chrono::nanoseconds(0)) {
        std::this_thread::sleep_for(spin_period);
      }
    }
    executor_.spin_some();
    return predicate();
  }

  /// Spin until a future completes or timeout expires.
  template <typename FutureT>
  bool spin_until_future_complete(FutureT &future, std::chrono::nanoseconds timeout)
  {
    auto result = executor_.spin_until_future_complete(future, timeout);
    return result == rclcpp::FutureReturnCode::SUCCESS;
  }

  void spin_some() { executor_.spin_some(); }

private:
  rclcpp::executors::SingleThreadedExecutor executor_;
};

/// Subscription wrapper that caches the last message and message count.
template <class MsgT>
class CachedSubscriber
{
public:
  CachedSubscriber(
    const rclcpp::Node::SharedPtr &node,
    const std::string &topic,
    const rclcpp::QoS &qos = rclcpp::QoS(rclcpp::KeepLast(10)),
    bool latched = false)
  {
    rclcpp::QoS resolved_qos = qos;
    if (latched) {
      resolved_qos.durability(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
    }
    sub_ = node->create_subscription<MsgT>(
      topic,
      resolved_qos,
      [this](const std::shared_ptr<MsgT> msg) { on_message(msg); });
  }

  /// Clear cached data.
  void reset()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_message_.reset();
    message_count_ = 0;
  }

  bool has_message() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_message_.has_value();
  }

  /// Return a copy of the last message (if any).
  std::optional<MsgT> last_message() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_message_;
  }

  size_t message_count() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_count_;
  }

  /// Wait for a new message that optionally satisfies a predicate.
  bool wait_for_message(
    TestExecutor &executor,
    std::chrono::nanoseconds timeout,
    const std::function<bool(const MsgT &)> &predicate = nullptr)
  {
    size_t start_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      start_count = message_count_;
    }

    return executor.spin_until(
      [this, start_count, &predicate]() {
        std::optional<MsgT> msg;
        size_t count = 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          count = message_count_;
          if (count <= start_count || !last_message_) {
            return false;
          }
          msg = last_message_;
        }
        if (predicate) {
          return predicate(*msg);
        }
        return true;
      },
      timeout);
  }

  /// Wait until at least min_publishers are connected.
  bool wait_for_publishers(
    TestExecutor &executor,
    size_t min_publishers,
    std::chrono::nanoseconds timeout)
  {
    return executor.spin_until(
      [this, min_publishers]() {
        return sub_ && sub_->get_publisher_count() >= min_publishers;
      },
      timeout);
  }

  bool is_connected() const { return sub_ && sub_->get_publisher_count() > 0; }

private:
  void on_message(const std::shared_ptr<MsgT> msg)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_message_ = *msg;
    ++message_count_;
  }

  std::shared_ptr<rclcpp::Subscription<MsgT>> sub_;
  mutable std::mutex mutex_;
  std::optional<MsgT> last_message_;
  size_t message_count_{0};
};

/// Wait until the topic has at least min_publishers publishers.
inline bool wait_for_publishers(
  TestExecutor &executor,
  const rclcpp::Node::SharedPtr &node,
  const std::string &topic,
  size_t min_publishers,
  std::chrono::nanoseconds timeout)
{
  return executor.spin_until(
    [&node, &topic, min_publishers]() { return node->count_publishers(topic) >= min_publishers; },
    timeout);
}

/// Wait until the topic has at least min_subscribers subscribers.
inline bool wait_for_subscribers(
  TestExecutor &executor,
  const rclcpp::Node::SharedPtr &node,
  const std::string &topic,
  size_t min_subscribers,
  std::chrono::nanoseconds timeout)
{
  return executor.spin_until(
    [&node, &topic, min_subscribers]() { return node->count_subscribers(topic) >= min_subscribers; },
    timeout);
}

/// Wait for a service to appear on the ROS graph.
template <typename ServiceT>
bool wait_for_service(
  const typename rclcpp::Client<ServiceT>::SharedPtr &client,
  TestExecutor &executor,
  std::chrono::nanoseconds timeout)
{
  return executor.spin_until(
    [&client]() { return client->wait_for_service(std::chrono::nanoseconds(0)); },
    timeout);
}

/// Wait for an action server to appear on the ROS graph.
template <typename ActionT>
bool wait_for_action_server(
  const typename rclcpp_action::Client<ActionT>::SharedPtr &client,
  TestExecutor &executor,
  std::chrono::nanoseconds timeout)
{
  return executor.spin_until(
    [&client]() { return client->wait_for_action_server(std::chrono::nanoseconds(0)); },
    timeout);
}

struct ServiceCallOptions
{
  std::chrono::nanoseconds service_timeout{kDefaultTimeout};
  std::chrono::nanoseconds response_timeout{kDefaultTimeout};
};

/// Call a service and wait for the response.
template <typename ServiceT>
typename ServiceT::Response::SharedPtr call_service(
  const typename rclcpp::Client<ServiceT>::SharedPtr &client,
  const typename ServiceT::Request::SharedPtr &request,
  TestExecutor &executor,
  const ServiceCallOptions &options = ServiceCallOptions())
{
  if (!wait_for_service<ServiceT>(client, executor, options.service_timeout)) {
    return nullptr;
  }
  auto future = client->async_send_request(request);
  if (!executor.spin_until_future_complete(future, options.response_timeout)) {
    return nullptr;
  }
  return future.get();
}

struct ActionCallOptions
{
  std::chrono::nanoseconds server_timeout{kDefaultTimeout};
  std::chrono::nanoseconds goal_timeout{kDefaultTimeout};
  std::chrono::nanoseconds result_timeout{std::chrono::seconds(30)};
};

/// Send an action goal and wait for the result.
template <typename ActionT>
std::optional<typename rclcpp_action::ClientGoalHandle<ActionT>::WrappedResult> call_action(
  const typename rclcpp_action::Client<ActionT>::SharedPtr &client,
  const typename ActionT::Goal &goal,
  TestExecutor &executor,
  const ActionCallOptions &options = ActionCallOptions())
{
  if (!wait_for_action_server<ActionT>(client, executor, options.server_timeout)) {
    return std::nullopt;
  }

  auto goal_future = client->async_send_goal(goal);
  if (!executor.spin_until_future_complete(goal_future, options.goal_timeout)) {
    return std::nullopt;
  }
  auto goal_handle = goal_future.get();
  if (!goal_handle) {
    return std::nullopt;
  }

  auto result_future = client->async_get_result(goal_handle);
  if (!executor.spin_until_future_complete(result_future, options.result_timeout)) {
    return std::nullopt;
  }
  return result_future.get();
}

/// Create NodeOptions that load parameters from a YAML file.
inline rclcpp::NodeOptions node_options_from_yaml(
  const std::string &params_file,
  const std::vector<std::string> &extra_arguments = {})
{
  std::vector<std::string> args = {"--ros-args", "--params-file", params_file};
  args.insert(args.end(), extra_arguments.begin(), extra_arguments.end());
  rclcpp::NodeOptions options;
  options.arguments(args);
  options.automatically_declare_parameters_from_overrides(true);
  return options;
}

}  // namespace hector_testing_utils

#endif  // HECTOR_TESTING_UTILS_HECTOR_TESTING_UTILS_HPP
