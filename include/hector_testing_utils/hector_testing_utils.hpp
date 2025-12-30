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
#include <map>
#include <sstream>

#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

namespace hector_testing_utils
{

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kDefaultSpinPeriod{5};
constexpr std::chrono::seconds kDefaultTimeout{5};

// =============================================================================
// Executor Helper
// =============================================================================

class TestExecutor
{
public:
  TestExecutor() = default;

  void add_node(const rclcpp::Node::SharedPtr &node) { executor_.add_node(node); }
  
  // Spin until predicate is true
  bool spin_until(
    const std::function<bool()> &predicate,
    std::chrono::nanoseconds timeout = kDefaultTimeout,
    std::chrono::nanoseconds spin_period = kDefaultSpinPeriod)
  {
    const auto start = std::chrono::steady_clock::now();
    while (rclcpp::ok()) {
      if (predicate()) return true;
      if (std::chrono::steady_clock::now() - start > timeout) return false;
      executor_.spin_some();
      std::this_thread::sleep_for(spin_period);
    }
    return false;
  }

  void spin_some() { executor_.spin_some(); }

private:
  rclcpp::executors::SingleThreadedExecutor executor_;
};

// =============================================================================
// Interfaces & Wrappers
// =============================================================================

// Abstract base for anything that needs to "connect"
class Connectable {
public:
    virtual ~Connectable() = default;
    virtual bool is_connected() const = 0;
    virtual std::string get_name() const = 0;
    virtual std::string get_type() const = 0; // e.g., "Publisher", "Client"
};

/// Wrapper for Publisher
template <typename MsgT>
class TestPublisher : public Connectable
{
public:
  TestPublisher(rclcpp::Node::SharedPtr node, const std::string & topic) 
    : topic_(topic)
  {
    pub_ = node->create_publisher<MsgT>(topic, 10);
  }

  void publish(const MsgT & msg) { pub_->publish(msg); }
  
  bool is_connected() const override { return pub_->get_subscription_count() > 0; }
  std::string get_name() const override { return topic_; }
  std::string get_type() const override { return "Publisher"; }

  // Specific wait helper
  bool wait_for_subscription(TestExecutor& exec, std::chrono::nanoseconds timeout = kDefaultTimeout) {
      return exec.spin_until([this](){ return is_connected(); }, timeout);
  }

private:
  typename rclcpp::Publisher<MsgT>::SharedPtr pub_;
  std::string topic_;
};

/// Wrapper for Subscription
template <class MsgT>
class TestSubscription : public Connectable
{
public:
  TestSubscription(
    const rclcpp::Node::SharedPtr &node,
    const std::string &topic,
    const rclcpp::QoS &qos = rclcpp::QoS(rclcpp::KeepLast(10)),
    bool latched = false) : topic_(topic)
  {
    rclcpp::QoS resolved_qos = qos;
    if (latched) resolved_qos.durability(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
    
    sub_ = node->create_subscription<MsgT>(
      topic, resolved_qos,
      [this](const std::shared_ptr<MsgT> msg) { 
        std::lock_guard<std::mutex> lock(mutex_);
        last_message_ = *msg;
        message_history_.push_back(*msg);
        ++message_count_;
      });
  }

  bool is_connected() const override { return sub_->get_publisher_count() > 0; }
  std::string get_name() const override { return topic_; }
  std::string get_type() const override { return "Subscription"; }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_message_.reset();
    message_count_ = 0;
  }

  bool has_new_message(size_t start_count) const {
      std::lock_guard<std::mutex> lock(mutex_);
      return message_count_ > start_count;
  }

  std::optional<MsgT> last_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_message_;
  }
  
  // Test Helpers
  bool wait_for_new_message(TestExecutor &executor, std::chrono::nanoseconds timeout = kDefaultTimeout) {
    size_t start_count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        start_count = message_count_;
    }
    return executor.spin_until([this, start_count]() { return has_new_message(start_count); }, timeout);
  }

private:
  std::shared_ptr<rclcpp::Subscription<MsgT>> sub_;
  std::string topic_;
  mutable std::mutex mutex_;
  std::optional<MsgT> last_message_;
  std::vector<MsgT> message_history_;
  size_t message_count_{0};
};

/// Wrapper for Service Client
template <typename ServiceT>
class TestClient : public Connectable
{
public:
  TestClient(rclcpp::Node::SharedPtr node, const std::string & service_name) 
    : service_name_(service_name)
  {
    client_ = node->create_client<ServiceT>(service_name);
  }

  bool is_connected() const override { return client_->service_is_ready(); }
  std::string get_name() const override { return service_name_; }
  std::string get_type() const override { return "Service Client"; }

  typename rclcpp::Client<ServiceT>::SharedPtr get() { return client_; }

  bool wait_for_service(TestExecutor& exec, std::chrono::nanoseconds timeout = kDefaultTimeout) {
      // client->wait_for_service is blocking, so we use spin_until with non-blocking check
      return exec.spin_until([this](){ return client_->service_is_ready(); }, timeout);
  }

private:
  typename rclcpp::Client<ServiceT>::SharedPtr client_;
  std::string service_name_;
};

/// Wrapper for Action Client
template <typename ActionT>
class TestActionClient : public Connectable
{
public:
  TestActionClient(rclcpp::Node::SharedPtr node, const std::string & action_name) 
    : action_name_(action_name)
  {
    client_ = rclcpp_action::create_client<ActionT>(node, action_name);
  }

  bool is_connected() const override { return client_->action_server_is_ready(); }
  std::string get_name() const override { return action_name_; }
  std::string get_type() const override { return "Action Client"; }

  typename rclcpp_action::Client<ActionT>::SharedPtr get() { return client_; }

  bool wait_for_server(TestExecutor& exec, std::chrono::nanoseconds timeout = kDefaultTimeout) {
      return exec.spin_until([this](){ return client_->action_server_is_ready(); }, timeout);
  }

private:
  typename rclcpp_action::Client<ActionT>::SharedPtr client_;
  std::string action_name_;
};


// =============================================================================
// The Main Testing Node
// =============================================================================

class TestNode : public rclcpp::Node
{
public:
  explicit TestNode(const std::string & name_space, const std::string & node_name = "test_node")
  : Node(node_name, name_space) {}

  // Factory methods that register the objects
  template <typename MsgT>
  std::shared_ptr<TestPublisher<MsgT>> create_test_publisher(const std::string & topic) {
      auto ptr = std::make_shared<TestPublisher<MsgT>>(shared_from_this(), topic);
      register_connectable(ptr);
      return ptr;
  }

  template <typename MsgT>
  std::shared_ptr<TestSubscription<MsgT>> create_test_subscription(const std::string & topic, bool latched=false) {
      auto ptr = std::make_shared<TestSubscription<MsgT>>(shared_from_this(), topic, rclcpp::QoS(10), latched);
      register_connectable(ptr);
      return ptr;
  }

  template <typename ServiceT>
  std::shared_ptr<TestClient<ServiceT>> create_test_client(const std::string & service_name) {
      auto ptr = std::make_shared<TestClient<ServiceT>>(shared_from_this(), service_name);
      register_connectable(ptr);
      return ptr;
  }

  template <typename ActionT>
  std::shared_ptr<TestActionClient<ActionT>> create_test_action_client(const std::string & action_name) {
      auto ptr = std::make_shared<TestActionClient<ActionT>>(shared_from_this(), action_name);
      register_connectable(ptr);
      return ptr;
  }

  // The main wait function
  bool wait_for_all_connections(TestExecutor& executor, std::chrono::seconds timeout = std::chrono::seconds(10)) {
      auto start = std::chrono::steady_clock::now();
      auto last_print = start;

      return executor.spin_until([&]() {
          bool all_connected = true;
          std::vector<std::string> disconnected_names;

          for(const auto& item : registry_) {
              if(!item->is_connected()) {
                  all_connected = false;
                  disconnected_names.push_back(item->get_type() + ": " + item->get_name());
              }
          }

          // Print status every 2 seconds if waiting
          auto now = std::chrono::steady_clock::now();
          if (!all_connected && (now - last_print) > 2s) {
              std::stringstream ss;
              ss << "Waiting for connections (" << disconnected_names.size() << " pending): ";
              for(const auto& name : disconnected_names) ss << "[" << name << "] ";
              RCLCPP_WARN(this->get_logger(), "%s", ss.str().c_str());
              last_print = now;
          }

          return all_connected;
      }, timeout);
  }

private:
  void register_connectable(std::shared_ptr<Connectable> item) {
      registry_.push_back(item);
  }

  std::vector<std::shared_ptr<Connectable>> registry_;
};


// =============================================================================
// Assertions & Macros
// =============================================================================

// Internal helpers
inline ::testing::AssertionResult assert_service_exists(
  TestExecutor & executor,
  rclcpp::Node::SharedPtr node,
  const std::string & service_name,
  std::chrono::nanoseconds timeout)
{
  bool found = executor.spin_until([&]() {
    auto services = node->get_service_names_and_types();
    return services.find(service_name) != services.end();
  }, timeout);

  if (found) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << "Service '" << service_name << "' failed to appear.";
}

inline ::testing::AssertionResult assert_action_server_exists(
  TestExecutor & executor,
  rclcpp::Node::SharedPtr node,
  const std::string & action_name,
  std::chrono::nanoseconds timeout)
{
   // Note: checking graph for actions is tricky because actions expand to multiple topics/services.
   // Reliable way: create a temp action client and check readiness, or check for the 'action_name/_action/status' topic.
   // Here we check for the status topic which every action server publishes.
   std::string status_topic = action_name + "/_action/status";
   
   bool found = executor.spin_until([&]() {
      auto topics = node->get_topic_names_and_types();
      return topics.find(status_topic) != topics.end();
   }, timeout);

   if (found) return ::testing::AssertionSuccess();
   return ::testing::AssertionFailure() << "Action Server '" << action_name << "' failed to appear.";
}

// Macros

#define ASSERT_SERVICE_EXISTS(node, service, timeout) \
  ASSERT_TRUE(hector_testing_utils::assert_service_exists( \
    *executor_, node, service, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)))

#define EXPECT_SERVICE_EXISTS(node, service, timeout) \
  EXPECT_TRUE(hector_testing_utils::assert_service_exists( \
    *executor_, node, service, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)))

#define ASSERT_ACTION_EXISTS(node, action, timeout) \
  ASSERT_TRUE(hector_testing_utils::assert_action_server_exists( \
    *executor_, node, action, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)))

#define EXPECT_ACTION_EXISTS(node, action, timeout) \
  EXPECT_TRUE(hector_testing_utils::assert_action_server_exists( \
    *executor_, node, action, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)))

// =============================================================================
// Fixture Update
// =============================================================================

class HectorTestFixture : public ::testing::Test
{
protected:
  void SetUp() override {
    if (!rclcpp::ok()) { rclcpp::init(0, nullptr); }
    executor_ = std::make_shared<TestExecutor>();
    // Instantiate our new smart TestNode
    tester_node_ = std::make_shared<TestNode>("hector_tester_node");
    executor_->add_node(tester_node_);
  }

  void TearDown() override {
    if (rclcpp::ok()) { rclcpp::shutdown(); }
  }

  std::shared_ptr<TestExecutor> executor_;
  std::shared_ptr<TestNode> tester_node_;
};

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

} // namespace hector_testing_utils

#endif // HECTOR_TESTING_UTILS_HECTOR_TESTING_UTILS_HPP