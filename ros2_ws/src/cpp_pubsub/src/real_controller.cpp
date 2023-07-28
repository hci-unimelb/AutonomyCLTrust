#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_msgs/msg/float64.hpp"
#include "trajectory_msgs/msg/joint_trajectory.hpp"
#include "trajectory_msgs/msg/joint_trajectory_point.hpp"
#include "sensor_msgs/msg/joint_state.hpp"

#include "tutorial_interfaces/msg/falconpos.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <stdio.h>

#include <kdl_parser/kdl_parser.hpp>
#include <kdl/chain.hpp>
#include <kdl/chainjnttojacsolver.hpp>
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainiksolverpos_nr.hpp>
#include <kdl/chainiksolvervel_pinv.hpp>
#include <kdl/frames.hpp>
#include <kdl/jacobian.hpp>
#include <kdl/jntarray.hpp>


using namespace std::chrono_literals;


/////////////////// global variables ///////////////////
const std::string urdf_path = "/home/michael/FOR_TESTING/panda.urdf";
const unsigned int n_joints = 7;

const std::vector<double> lower_joint_limits {-2.8973, -1.7628, -2.8973, -3.0718, -2.8973, -0.0175, -2.8973};
const std::vector<double> upper_joint_limits {2.8973, 1.7628, 2.8973, -0.0698, 2.8973, 3.7525, 2.8973};

const bool display_time = true;

KDL::Tree panda_tree;
KDL::Chain panda_chain;

KDL::Rotation orientation;
bool got_orientation = false;

std::vector<double> tcp_pos {0.3069, 0.0, 0.4853};   // initialized the same as the "home" position


/////////////////// function declarations ///////////////////
void compute_ik(std::vector<double>& desired_tcp_pos, std::vector<double>& curr_vals, std::vector<double>& res_vals);

void get_robot_control(double t, std::vector<double>& vals);

bool within_limits(std::vector<double>& vals);
bool create_tree();
void get_chain();

void print_joint_vals(std::vector<double>& joint_vals);



/////////////// DEFINITION OF NODE CLASS //////////////

class RealController : public rclcpp::Node
{
public:
  
  // std::vector<double> origin {0.3059, 0.0, 0.4846}; //////// can change the task-space origin point! ////////
  std::vector<double> origin {0.4559, 0.0, 0.3346}; //////// can change the task-space origin point! ////////

  std::vector<double> human_offset {0.0, 0.0, 0.0};
  std::vector<double> robot_offset {0.0, 0.0, 0.0};

  std::vector<double> curr_joint_vals {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  std::vector<double> ik_joint_vals {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  std::vector<double> message_joint_vals {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  bool control = false;
  
  const double mapping_ratio = 1.5;    /////// this ratio is {end-effector movement} / {Falcon movement}
  const int control_freq = 20;   // the rate at which the "controller_publisher" function is called in [Hz]
  const double latency = 2.0;  // this is the artificial latency introduced into the joint points published

  // used to initially smoothly incorporate the Falcon offset
  // meaning that {max_count = control_frequency * 10} -> corresponds to 10 seconds of "smoothing time"
  const int max_count = control_freq * 10;
  int count = 0;

  double w = 0.0;  // this is the weight used for initial smoothing, which goes from 0 -> 1 as count goes from 0 -> max_count

  // alpha values in {x, y, z} dimensions, used for convex combination of human and robot input
  // alpha values correspond to the share of HUMAN INPUT
  // they have to be in the range of [0, 1]
  double ax = 1.0;
  double ay = 1.0;
  double az = 1.0;


  ////////////////////////////////////////////////////////////////////////
  RealController()
  : Node("real_controller")
  { 
    //Create publisher and subscriber and timer
    controller_pub_ = this->create_publisher<trajectory_msgs::msg::JointTrajectory>("joint_trajectory_controller/joint_trajectory", 10);
    controller_timer_ = this->create_wall_timer(50ms, std::bind(&RealController::controller_publisher, this));    // controls at 20 Hz 
    ////////////////// NOTE: the controller frequency should be kept quite low (20 Hz seems to be perfect) //////////////////

    //Create publisher and subscriber and timer
    tcp_pos_pub_ = this->create_publisher<tutorial_interfaces::msg::Falconpos>("tcp_position", 10);
    tcp_pos_timer_ = this->create_wall_timer(5ms, std::bind(&RealController::tcp_pos_publisher, this));    // publishes at 200 Hz

    joint_vals_sub_ = this->create_subscription<sensor_msgs::msg::JointState>(
      "joint_states", 10, std::bind(&RealController::joint_states_callback, this, std::placeholders::_1));

    falcon_pos_sub_ = this->create_subscription<tutorial_interfaces::msg::Falconpos>(
      "falcon_position", 10, std::bind(&RealController::falcon_pos_callback, this, std::placeholders::_1));

    //Create Panda tree and get its kinematic chain
    if (!create_tree()) rclcpp::shutdown();
    get_chain();

  }

private:
  
  ///////////////////////////////////// JOINT CONTROLLER /////////////////////////////////////
  void controller_publisher()
  { 
    if (control == true) {

      auto message = trajectory_msgs::msg::JointTrajectory();
      message.joint_names = {"panda_joint1", "panda_joint2", "panda_joint3", "panda_joint4", "panda_joint5", "panda_joint6", "panda_joint7"};


      // get the robot control offset in Cartesian space
      double time = (double) count / control_freq;
      get_robot_control(time, robot_offset);

      // perform the convex combination of robot and human offsets
      // also adding the origin and thus representing it as tcp_pos in the robot's base frame
      tcp_pos.at(0) = origin.at(0) + ax * human_offset.at(0) + (1-ax) * robot_offset.at(0);
      tcp_pos.at(1) = origin.at(1) + ay * human_offset.at(1) + (1-ay) * robot_offset.at(1);
      tcp_pos.at(2) = origin.at(2) + az * human_offset.at(2) + (1-az) * robot_offset.at(2);

      ///////// compute IK /////////
      compute_ik(tcp_pos, curr_joint_vals, ik_joint_vals);

      ///////// initial smooth transitioning from current position to Falcon-mapped position /////////
      if (count < max_count) count++;  // increase count up to the max_count value
      w = (double)count / max_count;
      std::cout << "The current count is " << count << std::endl;
      std::cout << "The current weight is " << w << std::endl;
      for (unsigned int i=0; i<n_joints; i++) message_joint_vals.at(i) = w * ik_joint_vals.at(i) + (1-w) * curr_joint_vals.at(i);

      ///////// check limits /////////
      if (!within_limits(message_joint_vals)) {
        std::cout << "--------\nThese violate the joint limits of the Panda arm, shutting down now !!!\n---------" << std::endl;
        rclcpp::shutdown();
      }
      
      ///////// prepare the trajectory message, introducing artificial latency /////////
      auto point = trajectory_msgs::msg::JointTrajectoryPoint();
      point.positions = message_joint_vals;
      point.time_from_start.nanosec = (int)(1000 / control_freq) * latency * 1000000;     //// => {milliseconds} * 1e6

      message.points = {point};

      std::cout << "The joint values [MESSAGE] are ";
      print_joint_vals(message_joint_vals);

      // RCLCPP_INFO(this->get_logger(), "Publishing controller joint values");
      controller_pub_->publish(message);

      
    }
  }

  ///////////////////////////////////// TCP POSITION PUBLISHER /////////////////////////////////////
  void tcp_pos_publisher()
  { 
    auto message = tutorial_interfaces::msg::Falconpos();
    message.x = tcp_pos.at(0);
    message.y = tcp_pos.at(1);
    message.z = tcp_pos.at(2);

    // RCLCPP_INFO(this->get_logger(), "Publishing controller joint values");
    tcp_pos_pub_->publish(message);
  }

  ///////////////////////////////////// JOINT STATES SUBSCRIBER /////////////////////////////////////
  void joint_states_callback(const sensor_msgs::msg::JointState & msg)
  { 
    auto data = msg.position;

    // write into our class variable for storing joint values
    for (unsigned int i=0; i<n_joints; i++) {
      curr_joint_vals.at(i) = data.at(i);
    }

    /// if this the first iteration, change the control flag and start partying!
    if (control == false) control = true;
  }

  ///////////////////////////////////// FALCON SUBSCRIBER /////////////////////////////////////
  void falcon_pos_callback(const tutorial_interfaces::msg::Falconpos & msg)
  { 
    human_offset.at(0) = msg.x / 100 * mapping_ratio;
    human_offset.at(1) = msg.y / 100 * mapping_ratio;
    human_offset.at(2) = msg.z / 100 * mapping_ratio;
    // std::cout << "x = " << human_offset.at(0) << ", " << "y = " << human_offset.at(1) << ", " << "z = " << human_offset.at(2) << std::endl;
  }

  rclcpp::Publisher<trajectory_msgs::msg::JointTrajectory>::SharedPtr controller_pub_;
  rclcpp::TimerBase::SharedPtr controller_timer_;

  rclcpp::Publisher<tutorial_interfaces::msg::Falconpos>::SharedPtr tcp_pos_pub_;
  rclcpp::TimerBase::SharedPtr tcp_pos_timer_;

  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_vals_sub_;

  rclcpp::Subscription<tutorial_interfaces::msg::Falconpos>::SharedPtr falcon_pos_sub_;
  
};



/////////////////////////////// robot control (trajectory following) function ///////////////////////////////

void get_robot_control(double t, std::vector<double>& vals) {

  // compute the coordinates of the time-parametrized trajectory (in Cartesian space)

  for (unsigned int i=0; i<n_joints; i++) {
    vals.at(i) = t;
  }

}


/////////////////////////////// my own ik function ///////////////////////////////

void compute_ik(std::vector<double>& desired_tcp_pos, std::vector<double>& curr_vals, std::vector<double>& res_vals) {

  auto start = std::chrono::high_resolution_clock::now();

	//Create solvers
	KDL::ChainFkSolverPos_recursive fk_solver(panda_chain);
	KDL::ChainIkSolverVel_pinv vel_ik_solver(panda_chain, 0.0001, 1000);
	KDL::ChainIkSolverPos_NR ik_solver(panda_chain, fk_solver, vel_ik_solver, 1000);

  //Create the KDL array of current joint values
  KDL::JntArray jnt_pos_start(n_joints);
  for (unsigned int i=0; i<n_joints; i++) {
    jnt_pos_start(i) = curr_vals.at(i);
  }

  //Write in the initial orientation if not already done so
  if (!got_orientation) {
    //Compute current tcp position
    KDL::Frame tcp_pos_start;
    fk_solver.JntToCart(jnt_pos_start, tcp_pos_start);
    orientation = tcp_pos_start.M;
    got_orientation = true;
  }

  //Create the task-space goal object
  // KDL::Vector vec_tcp_pos_goal(origin.at(0), origin.at(1), origin.at(2));
  KDL::Vector vec_tcp_pos_goal(desired_tcp_pos.at(0), desired_tcp_pos.at(1), desired_tcp_pos.at(2));
  KDL::Frame tcp_pos_goal(orientation, vec_tcp_pos_goal);

  //Compute inverse kinematics
  KDL::JntArray jnt_pos_goal(n_joints);
  ik_solver.CartToJnt(jnt_pos_start, tcp_pos_goal, jnt_pos_goal);

  //Change the control joint values and finish the function
  for (unsigned int i=0; i<n_joints; i++) {
    res_vals.at(i) = jnt_pos_goal.data(i);
  }

  if (display_time) {
    auto finish = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(finish - start);
    std::cout << "Execution of my IK solver function took " << duration.count() << " [microseconds]" << std::endl;
  }
  
}


///////////////// other helper functions /////////////////

bool within_limits(std::vector<double>& vals) {
  for (unsigned int i=0; i<n_joints; i++) {
    if (vals.at(i) > upper_joint_limits.at(i) || vals.at(i) < lower_joint_limits.at(i)) return false;
  }
  return true;
}

bool create_tree() {
  if (!kdl_parser::treeFromFile(urdf_path, panda_tree)){
		std::cout << "Failed to construct kdl tree" << std::endl;
   	return false;
  }
  return true;
}

void get_chain() {
  panda_tree.getChain("panda_link0", "panda_grasptarget", panda_chain);
}

void print_joint_vals(std::vector<double>& joint_vals) {
  
  std::cout << "[ ";
  for (unsigned int i=0; i<joint_vals.size(); i++) {
    std::cout << joint_vals.at(i) << ' ';
  }
  std::cout << "]" << std::endl;
}




//////////////////// MAIN FUNCTION ///////////////////

int main(int argc, char * argv[])
{   
  rclcpp::init(argc, argv);

  std::shared_ptr<RealController> michael = std::make_shared<RealController>();

  rclcpp::spin(michael);

  rclcpp::shutdown();
  return 0;
}
