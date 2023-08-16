#include <franka_example_controllers/my_controller.hpp>

#include <cassert>
#include <cmath>
#include <exception>

#include <Eigen/Eigen>
#include <controller_interface/controller_interface.hpp>

#include <memory>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/joint_state.hpp"


namespace franka_example_controllers {

/////////////////////////////////////////////////////////////////////////////////////////////
controller_interface::InterfaceConfiguration
MyController::command_interface_configuration() const {

  std::cout << "\n\n command_interface_configuration function \n\n" << std::endl;

  controller_interface::InterfaceConfiguration config;
  config.type = controller_interface::interface_configuration_type::INDIVIDUAL;

  for (int i = 1; i <= num_joints; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/effort");
  }
  return config;
}


/////////////////////////////////////////////////////////////////////////////////////////////
controller_interface::InterfaceConfiguration
MyController::state_interface_configuration() const {

  std::cout << "\n\n state_interface_configuration function \n\n" << std::endl;

  controller_interface::InterfaceConfiguration config;
  config.type = controller_interface::interface_configuration_type::INDIVIDUAL;
  for (int i = 1; i <= num_joints; ++i) {
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/position");
    config.names.push_back(arm_id_ + "_joint" + std::to_string(i) + "/velocity");
  }
  return config;
}


/////////////////////////////////////////////////////////////////////////////////////////////
controller_interface::return_type MyController::update(
    const rclcpp::Time& /*time*/,
    const rclcpp::Duration& /*period*/) {

  // std::cout << "\n\n update function \n\n" << std::endl;


  /////////////////////////////// SUBSCRIBER SECTION ///////////////////////////////

  rclcpp::WaitSet wait_set({}, std::vector<rclcpp::GuardCondition::SharedPtr>{guard_condition1});
  wait_set.add_subscription(sub1);
  auto wait_result = wait_set.wait(std::chrono::milliseconds(2));    //// this should be < (1000 / controller's update frequency)

  if (wait_result.kind() == rclcpp::WaitResultKind::Ready) {
    size_t subscriptions_num = wait_set.get_rcl_wait_set().size_of_subscriptions;
    for (size_t i = 0; i < subscriptions_num; i++) {
      if (wait_result.get_wait_set().get_rcl_wait_set().subscriptions[i]) {
        sensor_msgs::msg::JointState msg;
        rclcpp::MessageInfo msg_info;
        if (sub1->take(msg, msg_info)) {


          ////// this is where I need to fill in //////
          auto poses = msg.position;
          for (size_t i=0; i<poses.size(); i++) std::cout << "    " << poses.at(i);
          std::cout << "\n" << std::endl;
            

        } else {
          RCLCPP_INFO(node->get_logger(), "subscription %zu: No message", i + 1);
        }
      }
    }
  } else if (wait_result.kind() == rclcpp::WaitResultKind::Timeout) {
    RCLCPP_INFO(node->get_logger(), "wait-set waiting failed with timeout");
  } else if (wait_result.kind() == rclcpp::WaitResultKind::Empty) {
    RCLCPP_INFO(node->get_logger(), "wait-set waiting failed because wait-set is empty");
  }

  wait_set.remove_guard_condition(guard_condition1);
  wait_set.remove_subscription(sub1);




  /////////////////////////////// FRANKA CONTROLLER SECTION ///////////////////////////////

  updateJointStates();
  auto trajectory_time = this->get_node()->now() - start_time_;
  auto motion_generator_output = motion_generator_->getDesiredJointPositions(trajectory_time);
  Vector7d q_desired = motion_generator_output.first;
  bool finished = motion_generator_output.second;
  if (not finished) {
    const double kAlpha = 0.99;
    dq_filtered_ = (1 - kAlpha) * dq_filtered_ + kAlpha * dq_;
    Vector7d tau_d_calculated =
        k_gains_.cwiseProduct(q_desired - q_) + d_gains_.cwiseProduct(-dq_filtered_);
    for (int i = 0; i < 7; ++i) {
      command_interfaces_[i].set_value(tau_d_calculated(i));
    }
  } else {
    for (auto& command_interface : command_interfaces_) {
      command_interface.set_value(0);
    }
  }
  return controller_interface::return_type::OK;
}


/////////////////////////////////////////////////////////////////////////////////////////////
CallbackReturn MyController::on_init() {

  std::cout << "\n\n on_init function \n\n" << std::endl;

  q_goal_ << 0, -M_PI_4, 0, -3 * M_PI_4, 0, M_PI_2, M_PI_4;



  node = this->get_node();
  auto do_nothing = [](sensor_msgs::msg::JointState::UniquePtr) {assert(false);};
  sub1 = node->create_subscription<sensor_msgs::msg::JointState>("desired_joint_vals", 10, do_nothing);
  guard_condition1 = std::make_shared<rclcpp::GuardCondition>();
  


  try {
    auto_declare<std::string>("arm_id", "panda");
    auto_declare<std::vector<double>>("k_gains", {});
    auto_declare<std::vector<double>>("d_gains", {});
  } catch (const std::exception& e) {
    fprintf(stderr, "Exception thrown during init stage with message: %s \n", e.what());
    return CallbackReturn::ERROR;
  }
  return CallbackReturn::SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////////
CallbackReturn MyController::on_configure(
    const rclcpp_lifecycle::State& /*previous_state*/) {

  std::cout << "\n\n on_configure function \n\n" << std::endl;

  arm_id_ = get_node()->get_parameter("arm_id").as_string();
  auto k_gains = get_node()->get_parameter("k_gains").as_double_array();
  auto d_gains = get_node()->get_parameter("d_gains").as_double_array();
  if (k_gains.empty()) {
    RCLCPP_FATAL(get_node()->get_logger(), "k_gains parameter not set");
    return CallbackReturn::FAILURE;
  }
  if (k_gains.size() != static_cast<uint>(num_joints)) {
    RCLCPP_FATAL(get_node()->get_logger(), "k_gains should be of size %d but is of size %ld",
                 num_joints, k_gains.size());
    return CallbackReturn::FAILURE;
  }
  if (d_gains.empty()) {
    RCLCPP_FATAL(get_node()->get_logger(), "d_gains parameter not set");
    return CallbackReturn::FAILURE;
  }
  if (d_gains.size() != static_cast<uint>(num_joints)) {
    RCLCPP_FATAL(get_node()->get_logger(), "d_gains should be of size %d but is of size %ld",
                 num_joints, d_gains.size());
    return CallbackReturn::FAILURE;
  }
  for (int i = 0; i < num_joints; ++i) {
    d_gains_(i) = d_gains.at(i);
    k_gains_(i) = k_gains.at(i);
  }
  dq_filtered_.setZero();
  return CallbackReturn::SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////////
CallbackReturn MyController::on_activate(
    const rclcpp_lifecycle::State& /*previous_state*/) {

  std::cout << "\n\n on_activate function \n\n" << std::endl;

  updateJointStates();
  motion_generator_ = std::make_unique<MotionGenerator>(0.2, q_, q_goal_);
  start_time_ = this->get_node()->now();
  return CallbackReturn::SUCCESS;
}


/////////////////////////////////////////////////////////////////////////////////////////////
void MyController::updateJointStates() {
  for (auto i = 0; i < num_joints; ++i) {
    const auto& position_interface = state_interfaces_.at(2 * i);
    const auto& velocity_interface = state_interfaces_.at(2 * i + 1);

    assert(position_interface.get_interface_name() == "position");
    assert(velocity_interface.get_interface_name() == "velocity");

    q_(i) = position_interface.get_value();
    dq_(i) = velocity_interface.get_value();
  }
}
}  // namespace franka_example_controllers
#include "pluginlib/class_list_macros.hpp"
// NOLINTNEXTLINE
PLUGINLIB_EXPORT_CLASS(franka_example_controllers::MyController,
                       controller_interface::ControllerInterface)