#ifndef HECTOR_TESTING_UTILS_TEST_WRAPPERS_HPP
#define HECTOR_TESTING_UTILS_TEST_WRAPPERS_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include <hector_testing_utils/constants.hpp>
#include <hector_testing_utils/graph_introspection.hpp>
#include <hector_testing_utils/qos_helpers.hpp>
#include <hector_testing_utils/test_executor.hpp>

namespace hector_testing_utils
{

// =============================================================================
// Connectable Interface
// =============================================================================

/// @brief Interface for test objects that need to report connection status.
class Connectable
{
public:
  virtual ~Connectable() = default;

  /// @brief Check if the object is connected to its peer(s).
  virtual bool is_connected() const = 0;

  /// @brief Get the name (topic/service/action name).
  virtual std::string get_name() const = 0;

  /// @brief Get the type string (e.g. "Publisher", "Subscription", "Service Client").
  virtual std::string get_type() const = 0;
};

// =============================================================================
// QoS Enrichment Helpers
// =============================================================================

namespace detail
{

/// @brief Lock a wrapper's node. Wrappers hold it weakly since the TestNode owns them.
inline rclcpp::Node::SharedPtr lock_node( const std::weak_ptr<rclcpp::Node> &node,
                                          const std::string &name )
{
  auto locked = node.lock();
  if ( !locked ) {
    throw std::runtime_error( "hector_testing_utils: test wrapper for '" + name +
                              "' used after its node was destroyed" );
  }
  return locked;
}

/// @brief Enrich topic suggestions with QoS information.
///
/// This function adds QoS summary and compatibility hints to each suggestion,
/// helping users understand why their connection might be failing.
///
/// @param node The node to use for QoS introspection
/// @param suggestions The suggestions to enrich
/// @param my_qos The QoS settings of the local endpoint
/// @param is_publisher True if the local endpoint is a publisher, false for subscriber
inline void enrich_topic_suggestions_with_qos( const rclcpp::Node::SharedPtr &node,
                                               std::vector<Suggestion> &suggestions,
                                               const rclcpp::QoS &my_qos, bool is_publisher )
{
  for ( auto &suggestion : suggestions ) {
    // Add QoS summary
    suggestion.qos_info = get_topic_qos_summary( node, suggestion.name );

    // Add compatibility hint
    suggestion.qos_compatibility_hint =
        get_qos_compatibility_hint( node, suggestion.name, my_qos, is_publisher );
  }
}

} // namespace detail

// =============================================================================
// Test Wrappers
// =============================================================================

/// @brief Wrapper for a ROS publisher to facilitate testing.
template<typename MsgT>
class TestPublisher : public Connectable
{
public:
  /// @brief Construct a new Test Publisher object.
  TestPublisher( rclcpp::Node::SharedPtr node, const std::string &topic,
                 const rclcpp::QoS &qos = rclcpp::QoS( rclcpp::KeepLast( 10 ) ) )
      : node_( node ), topic_( topic ), qos_( qos )
  {
    pub_ = node->create_publisher<MsgT>( topic, qos );
  }

  /// @brief Publish a message.
  void publish( const MsgT &msg ) { pub_->publish( msg ); }

  bool is_connected() const override { return pub_->get_subscription_count() > 0; }
  std::string get_name() const override { return topic_; }
  std::string get_type() const override { return "Publisher"; }

  /// @brief Wait until at least one subscription is connected.
  ///
  /// On timeout, logs suggestions for similar topics that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @return true if a subscription connected, false on timeout.
  bool wait_for_subscription( TestExecutor &exec, std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_subscription( exec, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait until at least one subscription is connected, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar topics that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if a subscription connected, false on timeout.
  bool wait_for_subscription( TestExecutor &exec, std::chrono::nanoseconds timeout,
                              WaitFailureInfo &failure_info )
  {
    const bool success = exec.spin_until( [this]() { return is_connected(); }, timeout );

    if ( !success ) {
      failure_info.searched_name = topic_;
      failure_info.searched_type = detail::get_type_name<MsgT>();
      failure_info.entity_kind = "topic";
      const auto node = detail::lock_node( node_, get_name() );
      failure_info.suggestions = suggest_similar_topics( node, topic_, failure_info.searched_type );
      detail::enrich_topic_suggestions_with_qos( node, failure_info.suggestions, qos_, true );
    }

    return success;
  }

private:
  std::weak_ptr<rclcpp::Node> node_;
  typename rclcpp::Publisher<MsgT>::SharedPtr pub_;
  std::string topic_;
  rclcpp::QoS qos_;
};

/// @brief Wrapper for a ROS subscription to facilitate testing.
///
/// Handles message capture, counting, and provides wait utilities.
template<class MsgT>
class TestSubscription : public Connectable
{
public:
  /// @brief Construct a new Test Subscription object.
  TestSubscription( const rclcpp::Node::SharedPtr &node, const std::string &topic,
                    const rclcpp::QoS &qos = rclcpp::QoS( rclcpp::KeepLast( 10 ) ),
                    bool latched = false )
      : node_( node ), topic_( topic )
  {
    rclcpp::QoS resolved_qos = qos;
    if ( latched )
      resolved_qos.durability( RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL );

    qos_ = resolved_qos;

    // Capture state_, not this, as the callback may outlive the wrapper.
    sub_ = node->create_subscription<MsgT>( topic, resolved_qos,
                                            [state = state_]( const std::shared_ptr<MsgT> msg ) {
                                              std::lock_guard<std::mutex> lock( state->mutex );
                                              state->last_message = *msg;
                                              ++state->message_count;
                                            } );
  }

  bool is_connected() const override { return sub_->get_publisher_count() > 0; }
  std::string get_name() const override { return topic_; }
  std::string get_type() const override { return "Subscription"; }

  /// @brief Clear captured messages and reset count.
  void reset()
  {
    std::lock_guard<std::mutex> lock( state_->mutex );
    state_->last_message.reset();
    state_->message_count = 0;
  }

  /// @brief Check if new messages arrived since a starting count.
  bool has_new_message( size_t start_count ) const
  {
    std::lock_guard<std::mutex> lock( state_->mutex );
    return state_->message_count > start_count;
  }

  /// @brief Get the last received message.
  std::optional<MsgT> last_message() const
  {
    std::lock_guard<std::mutex> lock( state_->mutex );
    return state_->last_message;
  }

  /// @brief Get the total number of received messages.
  size_t message_count() const
  {
    std::lock_guard<std::mutex> lock( state_->mutex );
    return state_->message_count;
  }

  /// @brief Wait until at least 'count' publishers are connected.
  ///
  /// On timeout, logs suggestions for similar topics that exist in the ROS graph.
  ///
  /// @param executor The executor to spin.
  /// @param count Minimum number of publishers required.
  /// @param timeout Maximum wait time.
  /// @return true if enough publishers connected, false on timeout.
  bool wait_for_publishers( TestExecutor &executor, size_t count = 1,
                            std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_publishers( executor, count, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait until at least 'count' publishers are connected, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar topics that exist in the ROS graph.
  ///
  /// @param executor The executor to spin.
  /// @param count Minimum number of publishers required.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if enough publishers connected, false on timeout.
  bool wait_for_publishers( TestExecutor &executor, size_t count, std::chrono::nanoseconds timeout,
                            WaitFailureInfo &failure_info )
  {
    const bool success = executor.spin_until(
        [this, count]() { return sub_->get_publisher_count() >= count; }, timeout );

    if ( !success ) {
      failure_info.searched_name = topic_;
      failure_info.searched_type = detail::get_type_name<MsgT>();
      failure_info.entity_kind = "topic";
      const auto node = detail::lock_node( node_, get_name() );
      failure_info.suggestions = suggest_similar_topics( node, topic_, failure_info.searched_type );
      detail::enrich_topic_suggestions_with_qos( node, failure_info.suggestions, qos_, false );
    }

    return success;
  }

  /// @brief Wait for a message to arrive, optionally matching a predicate.
  /// @param executor The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param predicate Optional function to filter messages.
  /// @return true if a matching message arrived, false on timeout.
  bool wait_for_message( TestExecutor &executor, std::chrono::nanoseconds timeout = kDefaultTimeout,
                         const std::function<bool( const MsgT & )> &predicate = nullptr )
  {
    size_t start_count = 0;
    {
      std::lock_guard<std::mutex> lock( state_->mutex );
      start_count = state_->message_count;
    }

    return executor.spin_until(
        [this, start_count, &predicate]() {
          std::optional<MsgT> msg;
          size_t count = 0;
          {
            std::lock_guard<std::mutex> lock( state_->mutex );
            count = state_->message_count;
            if ( count <= start_count || !state_->last_message ) {
              return false;
            }
            msg = state_->last_message;
          }
          if ( predicate ) {
            return predicate( *msg );
          }
          return true;
        },
        timeout );
  }

  /// @brief Convenience wrapper for wait_for_message without predicate.
  bool wait_for_new_message( TestExecutor &executor,
                             std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    return wait_for_message( executor, timeout );
  }

private:
  std::weak_ptr<rclcpp::Node> node_;
  std::shared_ptr<rclcpp::Subscription<MsgT>> sub_;
  std::string topic_;
  rclcpp::QoS qos_{ rclcpp::KeepLast( 10 ) };

  struct State {
    std::mutex mutex;
    std::optional<MsgT> last_message;
    size_t message_count{ 0 };
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

/// @brief Wrapper for a ROS service client to facilitate testing.
template<typename ServiceT>
class TestClient : public Connectable
{
public:
  /// @brief Construct a new Test Client object.
  TestClient( rclcpp::Node::SharedPtr node, const std::string &service_name )
      : node_( node ), service_name_( service_name )
  {
    client_ = node->create_client<ServiceT>( service_name );
  }

  bool is_connected() const override { return client_->service_is_ready(); }
  std::string get_name() const override { return service_name_; }
  std::string get_type() const override { return "Service Client"; }

  /// @brief Get the underlying ROS client.
  typename rclcpp::Client<ServiceT>::SharedPtr get() { return client_; }

  /// @brief Wait for the service to be available.
  ///
  /// On timeout, logs suggestions for similar services that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @return true if the service became available, false on timeout.
  bool wait_for_service( TestExecutor &exec, std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_service( exec, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait for the service to be available, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar services that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if the service became available, false on timeout.
  bool wait_for_service( TestExecutor &exec, std::chrono::nanoseconds timeout,
                         WaitFailureInfo &failure_info )
  {
    const bool success = exec.spin_until( [this]() { return client_->service_is_ready(); }, timeout );

    if ( !success ) {
      failure_info.searched_name = service_name_;
      failure_info.searched_type = detail::get_type_name<ServiceT>();
      failure_info.entity_kind = "service";
      failure_info.suggestions = suggest_similar_services(
          detail::lock_node( node_, get_name() ), service_name_, failure_info.searched_type );
    }

    return success;
  }

private:
  std::weak_ptr<rclcpp::Node> node_;
  typename rclcpp::Client<ServiceT>::SharedPtr client_;
  std::string service_name_;
};

/// @brief Wrapper for a ROS action client to facilitate testing.
template<typename ActionT>
class TestActionClient : public Connectable
{
public:
  /// @brief Construct a new Test Action Client object.
  TestActionClient( rclcpp::Node::SharedPtr node, const std::string &action_name )
      : node_( node ), action_name_( action_name )
  {
    client_ = rclcpp_action::create_client<ActionT>( node, action_name );
  }

  bool is_connected() const override { return client_->action_server_is_ready(); }
  std::string get_name() const override { return action_name_; }
  std::string get_type() const override { return "Action Client"; }

  /// @brief Get the underlying ROS action client.
  typename rclcpp_action::Client<ActionT>::SharedPtr get() { return client_; }

  /// @brief Wait for the action server to be available.
  ///
  /// On timeout, logs suggestions for similar actions that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @return true if the action server became available, false on timeout.
  bool wait_for_server( TestExecutor &exec, std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_server( exec, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait for the action server to be available, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar actions that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if the action server became available, false on timeout.
  bool wait_for_server( TestExecutor &exec, std::chrono::nanoseconds timeout,
                        WaitFailureInfo &failure_info )
  {
    const bool success =
        exec.spin_until( [this]() { return client_->action_server_is_ready(); }, timeout );

    if ( !success ) {
      failure_info.searched_name = action_name_;
      failure_info.searched_type = detail::get_type_name<ActionT>();
      failure_info.entity_kind = "action";
      failure_info.suggestions = suggest_similar_actions( detail::lock_node( node_, get_name() ),
                                                          action_name_, failure_info.searched_type );
    }

    return success;
  }

private:
  std::weak_ptr<rclcpp::Node> node_;
  typename rclcpp_action::Client<ActionT>::SharedPtr client_;
  std::string action_name_;
};

/// @brief Wrapper for a ROS service server to facilitate testing.
template<typename ServiceT>
class TestServiceServer : public Connectable
{
public:
  using Callback = std::function<void( const std::shared_ptr<typename ServiceT::Request>,
                                       std::shared_ptr<typename ServiceT::Response> )>;

  /// @brief Construct a new Test Service Server object.
  TestServiceServer( const rclcpp::Node::SharedPtr &node, const std::string &service_name,
                     const Callback &callback, const rclcpp::QoS &qos = rclcpp::ServicesQoS() )
      : node_( node ), service_name_( service_name )
  {
    service_ = node->create_service<ServiceT>( service_name_, callback, qos );
  }

  bool is_connected() const override
  {
    return detail::lock_node( node_, get_name() )->count_clients( service_name_ ) > 0;
  }
  std::string get_name() const override { return service_name_; }
  std::string get_type() const override { return "Service Server"; }

  /// @brief Wait for at least one client to connect.
  ///
  /// On timeout, logs suggestions for similar services that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @return true if a client connected, false on timeout.
  bool wait_for_client( TestExecutor &exec, std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_client( exec, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait for at least one client to connect, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar services that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if a client connected, false on timeout.
  bool wait_for_client( TestExecutor &exec, std::chrono::nanoseconds timeout,
                        WaitFailureInfo &failure_info )
  {
    const bool success = exec.spin_until( [this]() { return is_connected(); }, timeout );

    if ( !success ) {
      failure_info.searched_name = service_name_;
      failure_info.searched_type = detail::get_type_name<ServiceT>();
      failure_info.entity_kind = "service";
      failure_info.suggestions = suggest_similar_services(
          detail::lock_node( node_, get_name() ), service_name_, failure_info.searched_type );
    }

    return success;
  }

  /// @brief Get the underlying ROS service.
  typename rclcpp::Service<ServiceT>::SharedPtr get() { return service_; }

private:
  std::weak_ptr<rclcpp::Node> node_;
  typename rclcpp::Service<ServiceT>::SharedPtr service_;
  std::string service_name_;
};

/// @brief Wrapper for a ROS action server to facilitate testing.
template<typename ActionT>
class TestActionServer : public Connectable
{
public:
  using GoalHandle = rclcpp_action::ServerGoalHandle<ActionT>;
  using GoalCallback = std::function<rclcpp_action::GoalResponse(
      const rclcpp_action::GoalUUID &, const std::shared_ptr<const typename ActionT::Goal> )>;
  using CancelCallback =
      std::function<rclcpp_action::CancelResponse( const std::shared_ptr<GoalHandle> )>;
  using AcceptedCallback = std::function<void( const std::shared_ptr<GoalHandle> )>;

  /// @brief Construct a new Test Action Server object.
  TestActionServer( const rclcpp::Node::SharedPtr &node, const std::string &action_name,
                    GoalCallback goal_cb, CancelCallback cancel_cb, AcceptedCallback accepted_cb )
      : node_( node ), action_name_( action_name )
  {
    server_ = rclcpp_action::create_server<ActionT>(
        node, action_name_, std::move( goal_cb ), std::move( cancel_cb ), std::move( accepted_cb ) );
  }

  bool is_connected() const override
  {
    // Clients subscribe to the status topic; use that to infer connectivity.
    return detail::lock_node( node_, get_name() )->count_subscribers( status_topic() ) > 0;
  }

  std::string get_name() const override { return action_name_; }
  std::string get_type() const override { return "Action Server"; }

  /// @brief Wait for at least one client to connect.
  ///
  /// On timeout, logs suggestions for similar actions that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @return true if a client connected, false on timeout.
  bool wait_for_client( TestExecutor &exec, std::chrono::nanoseconds timeout = kDefaultTimeout )
  {
    WaitFailureInfo failure_info;
    const bool success = wait_for_client( exec, timeout, failure_info );
    if ( !success ) {
      RCLCPP_ERROR( detail::lock_node( node_, get_name() )->get_logger(), "%s",
                    failure_info.format( kDefaultMaxSuggestions ).c_str() );
    }
    return success;
  }

  /// @brief Wait for at least one client to connect, with failure info on timeout.
  ///
  /// If the wait times out, this method populates the failure_info with suggestions
  /// for similar actions that exist in the ROS graph.
  ///
  /// @param exec The executor to spin.
  /// @param timeout Maximum wait time.
  /// @param failure_info Reference to receive failure information if wait times out.
  /// @return true if a client connected, false on timeout.
  bool wait_for_client( TestExecutor &exec, std::chrono::nanoseconds timeout,
                        WaitFailureInfo &failure_info )
  {
    const bool success = exec.spin_until( [this]() { return is_connected(); }, timeout );

    if ( !success ) {
      failure_info.searched_name = action_name_;
      failure_info.searched_type = detail::get_type_name<ActionT>();
      failure_info.entity_kind = "action";
      failure_info.suggestions = suggest_similar_actions( detail::lock_node( node_, get_name() ),
                                                          action_name_, failure_info.searched_type );
    }

    return success;
  }

  /// @brief Get the underlying ROS action server.
  typename rclcpp_action::Server<ActionT>::SharedPtr get() { return server_; }

private:
  std::string status_topic() const { return action_name_ + "/_action/status"; }

  std::weak_ptr<rclcpp::Node> node_;
  typename rclcpp_action::Server<ActionT>::SharedPtr server_;
  std::string action_name_;
};

} // namespace hector_testing_utils

#endif // HECTOR_TESTING_UTILS_TEST_WRAPPERS_HPP
