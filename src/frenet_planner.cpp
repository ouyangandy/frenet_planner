#include <autoware_msgs/LaneArray.h>
#include <autoware_msgs/DetectedObjectArray.h>

#include <tf2/utils.h>
#include <tf/transform_datatypes.h>

//headers in Eigen
#include <Eigen/Dense>

#include "frenet_planner.h"
#include "vectormap_struct.h"


//TODO: make namespace/file for utility method
//TODO: better naming 
double calculate2DDistace(const geometry_msgs::Point& point1,
                          const geometry_msgs::Point& point2)
{
  double dx = point1.x - point2.x;
  double dy = point1.y - point2.y;
  double distance = std::sqrt(std::pow(dx, 2)+std::pow(dy,2));
  return distance;
}

// ref: http://www.mech.tohoku-gakuin.ac.jp/rde/contents/course/robotics/coordtrans.html
// (pu, pv): retative, (px, py): absolute, (ox, oy): origin
// (pu, pv) = rot^-1 * {(px, py) - (ox, oy)}
geometry_msgs::Point transformToRelativeCoordinate2D(const geometry_msgs::Point &point,
                                                     const geometry_msgs::Pose &origin)
{
  geometry_msgs::Point res;

  // translation
  geometry_msgs::Point trans_p;
  trans_p.x = point.x - origin.position.x;
  trans_p.y = point.y - origin.position.y;
  

  // rotation (use inverse matrix of rotation)
  double yaw = tf2::getYaw(origin.orientation);
  res.x = (cos(yaw) * trans_p.x) + (sin(yaw) * trans_p.y);
  res.y = ((-1) * sin(yaw) * trans_p.x) + (cos(yaw) * trans_p.y);
  res.z = origin.position.z;

  return res;
}


//does not consider z axis information
//does not consider time axis motion of ego vehicle
FrenetPlanner::FrenetPlanner():
dt_for_sampling_points_(0.5),
initial_velocity_m_s_(0.6),
velcity_before_obstalcle_m_s_(0.3),
distance_before_obstalcle_(6.0)
{
}

FrenetPlanner::~FrenetPlanner()
{
}


void FrenetPlanner::doPlan(const geometry_msgs::PoseStamped& in_current_pose,
              const geometry_msgs::TwistStamped& in_current_twist,
              const std::vector<Point>& in_nearest_lane_points,
              const std::vector<autoware_msgs::Waypoint>& in_reference_waypoints,
              const std::unique_ptr<autoware_msgs::DetectedObjectArray>& in_objects_ptr,
              const geometry_msgs::TransformStamped& in_wp2map_tf,
              autoware_msgs::Lane& out_trajectory,
              std::vector<autoware_msgs::Lane>& out_debug_trajectories,
              std::vector<geometry_msgs::Point>& out_reference_points)
{   
  //TODO: seek more readable code
  //TODO: think the interface between the components
  std::cerr << "start process of doPlan" << std::endl;
  FrenetPoint origin_frenet_point;
  if(getOriginPointAndReferencePoint(
     in_current_pose.pose,
     in_current_twist.twist.linear.x,
     in_reference_waypoints,
     in_nearest_lane_points,
     in_objects_ptr,
     origin_frenet_point,
     kept_current_reference_point_,
     kept_current_trajectory_))
  {
    std::cerr << "log: update [current] reference point " << std::endl;
    std::vector<Trajectory> trajectories;
    drawTrajectories(origin_frenet_point,
                    *kept_current_reference_point_,
                    in_nearest_lane_points,
                    in_reference_waypoints,
                    trajectories,
                    out_debug_trajectories);
    
    //TODO: is_no_potential_accident comes from getBestTrajectory's output?
    selectBestTrajectory(trajectories,
                      in_objects_ptr,
                      in_reference_waypoints, 
                      kept_current_reference_point_,
                      kept_current_trajectory_);
    // std::cerr << "after pick up best trajectory" << std::endl;
  }
  else
  {
    std::cerr << "log: not update [current] refenrence point"  << std::endl;
  }
  
  
  if(kept_current_reference_point_)
  {
    out_reference_points.push_back(kept_current_reference_point_->cartesian_point);
  }
  
  FrenetPoint next_origin_point;  
  if(getNextOriginPointAndReferencePoint(in_current_pose.pose,
                        kept_current_trajectory_->trajectory_points.waypoints.back().twist.twist.linear.x,
                        in_reference_waypoints,
                        in_nearest_lane_points,
                        in_objects_ptr,
                        kept_current_trajectory_,
                        kept_next_trajectory_,
                        kept_current_reference_point_,
                        kept_next_reference_point_,
                        next_origin_point))
  {
    
    std::cerr << "log: drawTrajectories" << std::endl;
    //validity
    //draw trajectories
    std::vector<Trajectory> trajectories;
    drawTrajectories(next_origin_point,
                    *kept_next_reference_point_,
                    in_nearest_lane_points,
                    in_reference_waypoints,
                    trajectories,
                    out_debug_trajectories);
                    
    //get best trajectory
    bool succsessfully_get_best_trajectory =
    selectBestTrajectory(trajectories,
                      in_objects_ptr,
                      in_reference_waypoints, 
                      kept_next_reference_point_,
                      kept_next_trajectory_);
    //for debug
    if(kept_next_reference_point_)
    {
      out_reference_points.push_back(kept_next_reference_point_->cartesian_point);
    }
    //debug
    if(!succsessfully_get_best_trajectory)
    {
      std::cerr << "ATTENTION: calling null update!!!! getBestTrajectory"  << std::endl;
      kept_next_reference_point_ = nullptr;
      kept_next_trajectory_ = nullptr;
    }
  }
  else
  {
    std::cerr << "log: not update [next] reference point"  << std::endl;
    //for debug
    if(kept_next_reference_point_)
    {
      out_reference_points.push_back(kept_next_reference_point_->cartesian_point);
    }
  }
  
   
  //concate trajectory and make output
  out_trajectory.waypoints = kept_current_trajectory_->trajectory_points.waypoints;
  std::cerr << "wp size for current trajectory " << kept_current_trajectory_->trajectory_points.waypoints.size()<< std::endl;
  if(!kept_next_trajectory_)
  {
    std::cerr << "next trajectory is nullptr "  << std::endl;
  }
  else
  {
    std::cerr << "next trajectory is not nullptr" << std::endl;
    std::cerr << "wp size for next trajectory " << kept_next_trajectory_->trajectory_points.waypoints.size()<< std::endl;
    // make sure concat befor call kept_next_trajectory_.reset()
    out_trajectory.waypoints.insert(out_trajectory.waypoints.end(),
                                    kept_next_trajectory_->trajectory_points.waypoints.begin(),
                                    kept_next_trajectory_->trajectory_points.waypoints.end());
  }
 
}

bool FrenetPlanner::getTrajectory(
    const std::vector<Point>& lane_points,
    const std::vector<autoware_msgs::Waypoint>& reference_waypoints,
    const FrenetPoint& origin_frenet_point,
    const FrenetPoint& reference_freent_point,
    const double time_horizon,
    const double dt_for_sampling_points,
    Trajectory& trajectory)
{
  
  // TODO: seperate trajectory generation method and calculating cost method
  // https://www.researchgate.net/publication/254098780_Optimal_trajectories_for_time-critical_street_scenarios_using_discretized_terminal_manifolds
  
  // currently, only considering high speed
  
  
  // calculating coefficiency c012 for d
  Eigen::Matrix3d m1_0;
  m1_0 << 1.0, 0, 0,
          0, 1, 0,
          0, 0, 2; 
  Eigen::Vector3d c012 = m1_0.inverse()*origin_frenet_point.d_state;
  
  // calculatiing coefficiency c345 for d 
  Eigen::Matrix3d m1_t;
  m1_t << 1, time_horizon, std::pow(time_horizon,2),
          0, 1, 2*time_horizon,
          0, 0, 2;
  Eigen::Matrix3d m2_t;
  m2_t << std::pow(time_horizon, 3),std::pow(time_horizon, 4),std::pow(time_horizon, 5),
          3*std::pow(time_horizon,2), 4*std::pow(time_horizon, 3), 5*std::pow(time_horizon, 4),
          6*time_horizon, 12*std::pow(time_horizon, 2), 20*std::pow(time_horizon, 3);
  Eigen::Vector3d c345 = m2_t.inverse()*(reference_freent_point.d_state - m1_t*c012);
  
  // // TODO:, write method for less code
  // // calculating coefficiency c012 for s
  // Eigen::Matrix3d s_m1_0;
  // s_m1_0 << 1.0, 0, 0,
  //         0, 1, 0,
  //         0, 0, 2; 
  // Eigen::Vector3d s_c012 = s_m1_0.inverse()*current_s;
  
  // // calculating coefficiency c345 for s
  // Eigen::Matrix3d s_m1_t;
  // s_m1_t << 1, time_horizon, std::pow(time_horizon,2),
  //         0, 1, 2*time_horizon,
  //         0, 0, 2;
  // Eigen::Matrix3d s_m2_t;
  // s_m2_t << std::pow(time_horizon, 3),std::pow(time_horizon, 4),std::pow(time_horizon, 5),
  //         3*std::pow(time_horizon,2), 4*std::pow(time_horizon, 3), 5*std::pow(time_horizon, 4),
  //         6*time_horizon, 12*std::pow(time_horizon, 2), 20*std::pow(time_horizon, 3);
  // Eigen::Vector3d target_s_state;
  // Eigen::Vector3d s_c345 = s_m2_t.inverse()*(target_s - s_m1_t*s_c012);
  
  
  // TODO, write method for less code
  // calculating coefficiency c012 for s velocity
  Eigen::Vector2d current_s_va_state;
  current_s_va_state = origin_frenet_point.s_state.tail(2);
  
  Eigen::Matrix2d s_v_m1_0;
  s_v_m1_0 << 1, 0.0,
              0.0, 2; 
  Eigen::Vector2d s_v_c12 = s_v_m1_0.inverse()*current_s_va_state;
  
  // calculating coefficiency c345 for s velocity
  Eigen::Matrix2d s_v_m1_t;
  s_v_m1_t << 1, 2*time_horizon,
              0.0, 2;
  Eigen::Matrix2d s_v_m2_t;
  s_v_m2_t <<  3*std::pow(time_horizon, 2),4*std::pow(time_horizon, 3),
               6*time_horizon, 12*std::pow(time_horizon, 2);
  Eigen::Vector2d target_s_va_state;
  target_s_va_state = reference_freent_point.s_state.tail(2);
  Eigen::Vector2d s_v_c34 = s_v_m2_t.inverse()*(target_s_va_state - s_v_m1_t*s_v_c12);
  
  
  // sampling points from calculated path
  std::vector<double> d_vec;
  double calculated_d, calculated_d_v, calculated_d_a, calculated_s, calculated_s_v, calculated_s_a;
  //TODO: fix here 
  calculated_s = origin_frenet_point.s_state(0);
  
  //variables for cost calculation
  double s_at_time_horizon,  s_v_at_time_horizon, d_at_time_horizion, d_v_at_time_horizon;
  autoware_msgs::Waypoint waypoint_at_time_horizon;
  
  // sampling waypoint
  // for(double i = dt_for_sampling_points; i <= time_horizon; i+=dt_for_sampling_points)
  for(double i = dt_for_sampling_points; i <= time_horizon; i+=dt_for_sampling_points)
  {
    calculated_d = c012(0) + c012(1)*i + c012(2)*i*i + c345(0)*i*i*i + c345(1)*i*i*i*i + c345(2)*i*i*i*i*i;
    calculated_d_v = c012(1) + 2*c012(2)*i + 3*c345(0)*i*i + 4*c345(1)*i*i*i + 5*c345(2)*i*i*i*i;
    calculated_d_a = 2*c012(2) + 6*c345(0)*i + 12*c345(1)*i*i + 20*c345(2)*i*i*i;
    
    // calculated_s = s_c012(0) + s_c012(1)*i + s_c012(2)*i*i + s_c345(0)*i*i*i + s_c345(1)*i*i*i*i + s_c345(2)*i*i*i*i*i;
    // calculated_s_v = s_c012(1) + 2*s_c012(2)*i + 3*s_c345(0)*i*i + 4*s_c345(1)*i*i*i + 5*s_c345(2)*i*i*i*i;
    // calculated_s_a = 2*s_c012(2) + 6*s_c345(0)*i + 12*s_c345(1)*i*i + 20*s_c345(2)*i*i*i;
    calculated_s_v = s_v_c12(0) + 2*s_v_c12(1)*i + 3*s_v_c34(0)*i*i + 4*s_v_c34(1)*i*i*i;
    calculated_s_a = 2*s_v_c12(1) + 6*s_v_c34(0)*i + 12*s_v_c34(1)*i*i;
    calculated_s += calculated_s_v * dt_for_sampling_points;
      
    
    
    FrenetPoint calculated_frenet_point;
    calculated_frenet_point.s_state(0) = calculated_s;
    calculated_frenet_point.s_state(1) = calculated_s_v;
    calculated_frenet_point.s_state(2) = calculated_s_a;
    calculated_frenet_point.d_state(0) = calculated_d;
    calculated_frenet_point.d_state(1) = calculated_d_v;
    calculated_frenet_point.d_state(2) = calculated_d_a;
    autoware_msgs::Waypoint waypoint;
    calculateWaypoint(lane_points, 
                      calculated_frenet_point,
                      waypoint);
    // std::cerr << "waypoint linear velocity " << waypoint.twist.twist.linear.x << std::endl;
    waypoint.pose.pose.position.z = reference_waypoints.front().pose.pose.position.z;
    trajectory.trajectory_points.waypoints.push_back(waypoint);
    
    Eigen::Vector3d s_state, d_state;
    s_state << calculated_s,
               calculated_s_v,
               calculated_s_a;
    d_state << calculated_d,
               calculated_d_v,
               calculated_d_a;
    FrenetPoint frenet_point;
    frenet_point.s_state = s_state;
    frenet_point.d_state = d_state;
    trajectory.frenet_trajectory_points.push_back(frenet_point);
  }
  // std::cerr << "discrete sum " << calculated_s<< std::endl;
  // std::cerr << "continus sum " << current_s(0)+time_horizon*s_v_c12(0)+
  //                                 time_horizon*time_horizon*s_v_c12(1)+
  //                                 std::pow(time_horizon,3)*s_v_c34(0)+
  //                                 std::pow(time_horizon,4)*s_v_c34(1)
  //                                 << std::endl;
  
  //TODO: there meight be a better way
  trajectory.target_d = reference_freent_point.d_state;  
  return false;
}


bool FrenetPlanner::calculateWaypoint(
                           const std::vector<Point>& lane_points, 
                           const FrenetPoint& frenet_point,
                           autoware_msgs::Waypoint& waypoint)
{
  // add conversion script for low velocity
  // std::cerr << "-------" << std::endl;
  double s_position = frenet_point.s_state(0);
  double s_velocity = frenet_point.s_state(1);
  double s_acceleration = frenet_point.s_state(2);
  double d_position = frenet_point.d_state(0);
  double d_velocity = frenet_point.d_state(1);
  double d_acceleration = frenet_point.d_state(2);
  double min_abs_delta = 100000;
  double nearest_lane_point_delta_s;
  double nearest_lane_point_yaw;
  double nearest_lane_point_curvature;
  double nearest_lane_point_curvature_dot;
  geometry_msgs::Point nearest_lane_point;
  for (const auto& point: lane_points)
  {
    double delta_s = s_position - point.cumulated_s;
    if(std::abs(delta_s) < min_abs_delta)
    {
      min_abs_delta = std::abs(delta_s);
      nearest_lane_point_delta_s = delta_s;
      nearest_lane_point_yaw = point.rz;
      nearest_lane_point_curvature = point.curvature;
      nearest_lane_point_curvature_dot = point.curvature_dot;
      
      nearest_lane_point.x = point.tx;
      nearest_lane_point.y = point.ty;
      
    }
  }
  
  double velocity = std::sqrt(std::pow(1 - nearest_lane_point_curvature*d_position, 2)*
                              std::pow(s_velocity, 2) + 
                              std::pow(d_velocity,2));
  // double delta_yaw = nearest_lane_point_yaw - current_yaw;
  double d_dash = d_velocity/s_velocity;
  double d_double_dash = (1/(s_velocity*s_velocity))*(d_acceleration - s_acceleration*d_dash);
  double delta_yaw = std::atan(d_dash/(1 - nearest_lane_point_curvature * d_position));
  double waypoint_yaw = nearest_lane_point_yaw - delta_yaw;
  double waypoint_curvature = (std::pow(std::cos(delta_yaw),3)/
                               std::pow((1-nearest_lane_point_curvature*d_position),2))*
                              (d_double_dash +
                               (nearest_lane_point_curvature_dot*d_position +
                                nearest_lane_point_curvature*d_velocity)*
                                std::tan(delta_yaw)+
                               ((1 - nearest_lane_point_curvature*d_position)/
                               (std::pow(std::cos(delta_yaw),2)))*nearest_lane_point_curvature);
  double acceleration = s_acceleration*((1 - nearest_lane_point_curvature*d_position)/std::cos(delta_yaw))+
                        s_velocity*s_velocity/std::cos(delta_yaw)*
                        ((1 - nearest_lane_point_curvature*d_position)*std::tan(delta_yaw)*
                        (waypoint_curvature*((1 - nearest_lane_point_curvature*d_position)/std::cos(delta_yaw))-
                        nearest_lane_point_curvature) -
                        -1*(nearest_lane_point_curvature_dot*d_position+
                             nearest_lane_point_curvature*d_velocity));
  
  // std::cerr << "wp velocity " << velocity << std::endl;
  // std::cerr << "wp acceleration " << acceleration << std::endl;
  // std::cerr << "wp yaw " << waypoint_yaw << std::endl;
  // std::cerr << "wp curvature " << waypoint_curvature << std::endl;
  waypoint.pose.pose.orientation = tf::createQuaternionMsgFromYaw(waypoint_yaw);
  waypoint.twist.twist.linear.x = velocity;
  
  // double d_double_dash = -1*((nearest_lane_point_curvature_dot*d_position+
  //                            nearest_lane_point_curvature*d_velocity)*std::tan(delta_yaw))+
  //                            ((1 - nearest_lane_point_curvature*d_position)/std::cos(delta_yaw)*std::cos(delta_yaw))*
  //                            (nearest_lane_point_curvature*((1 - nearest_lane_point_curvature*d_position)/std::cos(delta_yaw))-
  //                            nearest_lane_point_curvature);
                  
  // double d_velocit
                              
                              
  // std::cerr << "nearest lane yaw " << nearest_lane_point_yaw << std::endl;
  // std::cerr << "current yaw " << current_yaw << std::endl;
  geometry_msgs::Point waypoint_position;
  convertFrenetPosition2CartesianPosition(s_position,
                                          d_position,
                                          nearest_lane_point,
                                          nearest_lane_point_delta_s,
                                          nearest_lane_point_yaw,
                                          waypoint_position);
  waypoint.pose.pose.position = waypoint_position;

  return true;
}


bool FrenetPlanner::convertFrenetPosition2CartesianPosition(
                           const double frenet_s,
                           const double frenet_d,
                           const geometry_msgs::Point& nearest_lane_cartesian_point,
                           const double delta_s,
                           const double delta_yaw,
                           geometry_msgs::Point& waypoint_position)
{
  waypoint_position.x = nearest_lane_cartesian_point.x + delta_s*std::cos(delta_yaw);
  waypoint_position.y = nearest_lane_cartesian_point.y + delta_s*std::sin(delta_yaw);
  waypoint_position.x += frenet_d*std::cos(delta_yaw-M_PI/2);
  waypoint_position.y += frenet_d*std::sin(delta_yaw-M_PI/2);

  return true;
  // std::cerr <<"cumulated s "<< point.cumulated_s << std::endl;
}

bool FrenetPlanner::convertCartesianPosition2FrenetPosition(
        const geometry_msgs::Point& cartesian_point,
        const std::vector<Point> lane_points,        
        double& frenet_s_position,
        double& frenet_d_position)
{
  //TODO: redundant
  Point nearest_point, second_nearest_point;
  getNearestPoints(cartesian_point,
                    lane_points,
                    nearest_point,
                    second_nearest_point);
  double x1 = nearest_point.tx;
  double y1 = nearest_point.ty;
  double x2 = second_nearest_point.tx;
  double y2 = second_nearest_point.ty;
  
  // ax + by + c = 0 which pass point1 and point2
  double line_a = (y2 - y1)/(x2 - x1);
  double line_b = -1;
  double line_c = y1 - (x1*(y2 - y1))/(x2 - x1);
  
  // distance between point3 and line
  double x3 = cartesian_point.x;
  double y3 = cartesian_point.y;
  double current_d_position = std::abs(line_a*x3 + line_b*y3 + line_c)/std::sqrt(line_a*line_a+line_b*line_b);
  
  // check current_d is positive or negative by using cross product
  double tmp_det_z;
  tmp_det_z = (x2 - x1)*(y3 - y1) - (y2 - y1)*(x3 - x1);
  if (tmp_det_z > 0)
  {
    current_d_position*= -1;
  }
  
  
  
  
  // line equation perpendicular to ax + by + c = 0 && pass point3
  double p_line_a = line_b;
  double p_line_b = -1* line_a;
  double p_line_c = line_a*y3 - line_b*x3; 
  
  // again distance between point1 and p_line
  double current_s_position = std::abs(p_line_a*x1 + p_line_b*y1+ p_line_c)/std::sqrt(p_line_a*p_line_a+p_line_b*p_line_b);
  
  // check if current_s is positive or negative
  // calculate another point in p_line
  double x_a = x3 + 1;
  double y_a = -1* p_line_a*x_a/p_line_b -1*p_line_c/p_line_b;
  tmp_det_z = (x3 - x_a)*(y1 - y_a) - (y3 - y_a)*(x1 - x_a);
  if (tmp_det_z < 0)
  {
    current_s_position*= -1;
  }
  
  
  current_s_position += nearest_point.cumulated_s;

  // Eigen::Vecto
  frenet_d_position = current_d_position;
  frenet_s_position = current_s_position;
}
    
    
// TODO: redundant
void FrenetPlanner::getNearestPoints(const geometry_msgs::Point& point,
                                    const std::vector<Point>& nearest_lane_points,
                                    Point& nearest_point,
                                    Point& second_nearest_point)
{
  double min_dist = 99999;
  for(const auto& sub_point: nearest_lane_points)
  {
    double dx = point.x - sub_point.tx;
    double dy = point.y - sub_point.ty;
    double dist = std::sqrt(std::pow(dx, 2)+std::pow(dy, 2));
    if(dist < min_dist)
    {
      min_dist = dist;
      nearest_point = sub_point;
    }
  }
  double second_min_dist = 99999;
  bool is_initialized = false;
  for(size_t i = 0; i < nearest_lane_points.size(); i++)
  {
    double dx = point.x - nearest_lane_points[i].tx;
    double dy = point.y - nearest_lane_points[i].ty;
    double dist = std::sqrt(std::pow(dx, 2)+std::pow(dy, 2));
    if(dist <= min_dist && (i+1 < nearest_lane_points.size()))
    {
      second_nearest_point = nearest_lane_points[i+1];
      is_initialized = true;
    }
  }
  if (!is_initialized)
  {
    std::cerr << "second is not initialized"  << std::endl;
  }
  
  
  double threshold = 3;
  if(min_dist > threshold)
  {
    std::cerr << "error: target is too far; might not be valid goal" << std::endl;
  }
}

// TODO: redundant 
// TODO: make it faster
void FrenetPlanner::getNearestWaypoints(const geometry_msgs::Pose& point,
                                    const autoware_msgs::Lane& waypoints,
                                    autoware_msgs::Waypoint& nearest_waypoint,
                                    autoware_msgs::Waypoint& second_nearest_waypoint)
{
  double min_dist = 99999;
  size_t min_waypoint_index = 0;
  for(size_t i = 0 ; i < waypoints.waypoints.size(); i++)
  {
    double dx = point.position.x - waypoints.waypoints[i].pose.pose.position.x;
    double dy = point.position.y - waypoints.waypoints[i].pose.pose.position.y;
    double dist = std::sqrt(std::pow(dx, 2)+std::pow(dy, 2));
    if(dist < min_dist)
    {
      min_dist = dist;
      min_waypoint_index = i;
      nearest_waypoint = waypoints.waypoints[i];
    }
  }
  
  size_t second_min_waypoint_index = 0;
  if(min_waypoint_index == (waypoints.waypoints.size()-1))
  {
    second_min_waypoint_index = min_waypoint_index - 1; 
  }
  else
  {
    second_min_waypoint_index = min_waypoint_index + 1; 
  }
  second_nearest_waypoint = waypoints.waypoints[second_min_waypoint_index]; 
}

// TODO: redundant 
// TODO: make it faster
void FrenetPlanner::getNearestWaypoint(const geometry_msgs::Point& point,
                                    const std::vector<autoware_msgs::Waypoint>& waypoints,
                                    autoware_msgs::Waypoint& nearest_waypoint)
{
  double min_dist = 99999;
  for(size_t i = 0 ; i < waypoints.size(); i++)
  {
    double dx = point.x - waypoints[i].pose.pose.position.x;
    double dy = point.y - waypoints[i].pose.pose.position.y;
    double dist = std::sqrt(std::pow(dx, 2)+std::pow(dy, 2));
    if(dist < min_dist)
    {
      min_dist = dist;
      nearest_waypoint = waypoints[i];
    }
  } 
}

//TODO: make method for redundant part
void FrenetPlanner::getNearestWaypointIndex(const geometry_msgs::Point& point,
                                    const std::vector<autoware_msgs::Waypoint>& waypoints,
                                    size_t& nearest_waypoint_index)
{
  double min_dist = 99999;
  for(size_t i = 0 ; i < waypoints.size(); i++)
  {
    double dx = point.x - waypoints[i].pose.pose.position.x;
    double dy = point.y - waypoints[i].pose.pose.position.y;
    double dist = std::sqrt(std::pow(dx, 2)+std::pow(dy, 2));
    if(dist < min_dist)
    {
      min_dist = dist;
      nearest_waypoint_index = i;
    }
  } 
}


bool FrenetPlanner::selectBestTrajectory(
      const std::vector<Trajectory>& trajectories,
      const std::unique_ptr<autoware_msgs::DetectedObjectArray>& objects_ptr,
      const std::vector<autoware_msgs::Waypoint>& reference_waypoints, 
      std::unique_ptr<ReferencePoint>& kept_reference_point,
      std::unique_ptr<Trajectory>& kept_best_trajectory)
{
  //TODO: make it faster
  //make subset reference wps for evaluation
  // std::cerr << "num ref wps " << reference_waypoints.size() << std::endl;
  geometry_msgs::Point origin_cartesian_point = trajectories.front().trajectory_points.waypoints.front().pose.pose.position;
  size_t nearest_origin_waypoint_id;
  getNearestWaypointIndex(origin_cartesian_point,
                     reference_waypoints,
                     nearest_origin_waypoint_id);
  size_t nearest_last_waypoint_id;
  getNearestWaypointIndex(kept_reference_point->cartesian_point,
                     reference_waypoints,
                     nearest_last_waypoint_id);
  std::vector<autoware_msgs::Waypoint> subset_reference_waypoints;
  if(nearest_origin_waypoint_id>= nearest_last_waypoint_id)
  {
    //TODO: extremely bad practice; fix in v0.4
    // generate emergency stop waypoint?
    std::cerr << "nearest origin wp id " << nearest_origin_waypoint_id << std::endl;
    std::cerr << "nearest last wp id " << nearest_last_waypoint_id << std::endl;  
    std::cerr << "ERROR: Could not handle this error in v0.3 getBestTrajectory"  << std::endl;
    std::cerr << "Recommend to lower distance_before_obstalce_"  << std::endl;
    kept_reference_point = nullptr;
    kept_best_trajectory = nullptr;
  }
  for(size_t i = nearest_origin_waypoint_id; i < nearest_last_waypoint_id; i++)
  {
    subset_reference_waypoints.push_back(reference_waypoints[i]);
  }
  // std::cerr << "num subset ref wps " << subset_reference_waypoints.size() << std::endl;
  
  std::vector<double> ref_waypoints_costs;
  std::vector<double> ref_last_waypoints_costs;
  std::vector<double> debug_last_waypoints_costs_s;
  std::vector<double> debug_last_waypoints_costs_d;
  std::vector<double> debug_last_waypoints_actual_s;
  std::vector<double> debug_last_waypoints_actual_d;
  double sum_ref_waypoints_costs = 0;
  double sum_last_waypoints_costs = 0;
  for(const auto& trajectory: trajectories)
  {
    //calculate cost with reference waypoints
    double ref_waypoints_cost = 0;
    for(size_t i = 0; i < subset_reference_waypoints.size(); i++)
    {
      geometry_msgs::Point reference_point = subset_reference_waypoints[i].pose.pose.position;
      
      autoware_msgs::Waypoint nearest_trajectory_point;
      getNearestWaypoint(reference_point, 
                         trajectory.trajectory_points.waypoints,
                         nearest_trajectory_point);
      double dist = calculate2DDistace(reference_point, 
                              nearest_trajectory_point.pose.pose.position);
      
      // std::cerr << "ref "<< reference_point.x << " " << reference_point.y << std::endl;
      // std::cerr << "nearest traj "<< nearest_trajectory_point.pose.pose.position.x << " " << nearest_trajectory_point.pose.pose.position.y << std::endl;
      // std::cerr << "dist " << dist << std::endl;
      ref_waypoints_cost += dist;
    }
    ref_waypoints_costs.push_back(ref_waypoints_cost);
    sum_ref_waypoints_costs += ref_waypoints_cost;
    
    //calculate terminal cost
    FrenetPoint frenet_point_at_time_horizon = trajectory.frenet_trajectory_points.back();
    double ref_last_waypoint_cost = 
    std::pow(frenet_point_at_time_horizon.d_state(0) - kept_reference_point->frenet_point.d_state(0), 2) + 
    std::pow(frenet_point_at_time_horizon.s_state(0) - kept_reference_point->frenet_point.s_state(0), 2) +
    std::pow(frenet_point_at_time_horizon.s_state(1) - kept_reference_point->frenet_point.s_state(1), 2);
    // double ref_last_waypoint_cost = 
    // std::pow(frenet_point_at_time_horizon.d_state(0) - kept_reference_point->frenet_point.d_state(0), 2); 
    ref_last_waypoints_costs.push_back(ref_last_waypoint_cost);
    sum_last_waypoints_costs+= ref_last_waypoint_cost;
    debug_last_waypoints_costs_d.push_back(
      frenet_point_at_time_horizon.d_state(0) - kept_reference_point->frenet_point.d_state(0));
    debug_last_waypoints_costs_s.push_back(
      frenet_point_at_time_horizon.s_state(0) - kept_reference_point->frenet_point.s_state(0));
    debug_last_waypoints_actual_d.push_back(frenet_point_at_time_horizon.d_state(0));
    debug_last_waypoints_actual_s.push_back(frenet_point_at_time_horizon.s_state(0));
    // std::cerr << "diff s " <<  frenet_point_at_time_horizon.s_state(0) - kept_reference_point->frenet_point.s_state(0)<< std::endl;
    // std::cerr << "diff sv " <<  frenet_point_at_time_horizon.s_state(1) - kept_reference_point->frenet_point.s_state(1)<< std::endl;
    // std::cerr << "diff d " <<  frenet_point_at_time_horizon.d_state(0) - kept_reference_point->frenet_point.d_state(0)<< std::endl;
    // std::cerr << "total last wp diff " <<  ref_last_waypoint_cost<< std::endl;
    // std::cerr << "total wps diff " <<  ref_waypoints_cost<< std::endl;
  }
  
  std::vector<double> costs;
  std::vector<double> debug_norm_ref_wps_costs;
  std::vector<double> debug_norm_ref_last_wp_costs;
  for(size_t i = 0; i < ref_last_waypoints_costs.size(); i++)
  {
    double normalized_ref_waypoints_cost = ref_waypoints_costs[i]/sum_ref_waypoints_costs;
    double normalized_ref_last_waypoints_cost = ref_last_waypoints_costs[i]/sum_last_waypoints_costs;
    double sum_cost = normalized_ref_waypoints_cost + normalized_ref_last_waypoints_cost;
    costs.push_back(sum_cost);
    debug_norm_ref_wps_costs.push_back(normalized_ref_waypoints_cost);
    debug_norm_ref_last_wp_costs.push_back(normalized_ref_last_waypoints_cost);
    // std::cerr << "norm_ref_wps " << normalized_ref_waypoints_cost
    //           << "norm ref last "<< normalized_ref_last_waypoints_cost
    //           << "sum "<< sum_cost<< std::endl;
  }
  //arg sort 
  // https://stackoverflow.com/questions/1577475/c-sorting-and-keeping-track-of-indexes/12399290#12399290
  std::vector<size_t> indexes(trajectories.size());
  std::iota(indexes.begin(), indexes.end(), 0);
  std::sort(indexes.begin(), indexes.end(), [&costs](const size_t &a, const size_t &b)
                                               { return costs[a] < costs[b];});
  
  // std::cerr << "target s " << kept_reference_point->frenet_point.s_state(0) << std::endl;
  // std::cerr << "target d " << kept_reference_point->frenet_point.d_state(0) << std::endl;
  // std::cerr << "last point s " << ->frenet_point.d_state(0) << std::endl;
  // std::cerr << "last point d " << ->frenet_point.s_state(0) << std::endl;
  bool has_got_best_trajectory = false;
  for(const auto& index: indexes)
  {
    // std::cerr << "traj index from min cost " << index << std::endl;
    std::cerr << "n-wps " << debug_norm_ref_wps_costs[index]
              << " n-las "<< debug_norm_ref_last_wp_costs[index]
              << " las s "<< debug_last_waypoints_costs_s[index]
              << " act s "<< debug_last_waypoints_actual_s[index]
              << " las d "<< debug_last_waypoints_costs_d[index]
              << " act d "<< debug_last_waypoints_actual_d[index]
              << " sum "<< costs[index]<< std::endl;
    bool is_collision_free = true;
    if(objects_ptr)
    {
      is_collision_free = isTrajectoryCollisionFree(
                            trajectories[index].trajectory_points.waypoints,
                            *objects_ptr);
    }
    
    if(is_collision_free)
    {
      has_got_best_trajectory = true;
      kept_best_trajectory.reset(new Trajectory(trajectories[index]));
      break;
    }
  } 
  
  //TODO: this might be bad effect
  if(!has_got_best_trajectory)
  {
    return false;
    // std::cerr << "ATTENTION: calling null update!!!! getBestTrajectory"  << std::endl;
    // kept_reference_point = nullptr;
    // kept_best_trajectory = nullptr;
  }
  else
  {
    return true;
  }
}


bool FrenetPlanner::containFlaggedWaypoint(
  const autoware_msgs::Lane& reference_waypoints,
        autoware_msgs::Waypoint& flagged_waypoint)
{
  for(const auto& waypoint: reference_waypoints.waypoints)
  {
    //check if velocity is 0 or not
    if(std::abs(waypoint.twist.twist.linear.x) < 0.01 )
    {
      flagged_waypoint = waypoint;
      return true;
    }
  }
  return false;
}

bool FrenetPlanner::isFlaggedWaypointCloseWithTrajectory(
        const autoware_msgs::Waypoint& flagged_waypoint,
         const std::vector<autoware_msgs::Waypoint>& waypoints)
{
  for(const auto& waypoint: waypoints)
  {
    double dx = flagged_waypoint.pose.pose.position.x - 
                waypoint.pose.pose.position.x;
    double dy = flagged_waypoint.pose.pose.position.y - 
                waypoint.pose.pose.position.y;
    double distance = std::sqrt(std::pow(dx, 2) + std::pow(dy,2));
    //TODO: ellipse collisio check
    double radius =4;
    if(distance < radius)
    {
      return true;
    }
  }
  return false;
}


bool FrenetPlanner::isFlaggedWaypointCloseWithPoint(
        const autoware_msgs::Waypoint& flagged_waypoint,
        const geometry_msgs::Point& point)
{
  //TODO: redundant 
  double dx = flagged_waypoint.pose.pose.position.x - 
              point.x;
  double dy = flagged_waypoint.pose.pose.position.y - 
              point.y;
  double distance = std::sqrt(std::pow(dx, 2) + std::pow(dy,2));
  //TODO: ellipse collisio check variable/param
  double radius =4;
  if(distance < radius)
  {
    return true;
  }
  return false;
}


//TODO: not considering the size of waypoints
bool FrenetPlanner::generateNewReferencePoint(
  const ReferencePoint& current_reference_point,
  const double origin_linear_velocity,  
  const std::vector<autoware_msgs::Waypoint>& reference_waypoints,
  const std::vector<Point>& lane_points,
  const std::unique_ptr<autoware_msgs::DetectedObjectArray>& objects_ptr,
  ReferencePoint& reference_point)
{
  //TODO: assuming current_reference_point is terminal point in global waypoint
  if(current_reference_point.reference_type_info.type == ReferenceType::StopLine)
  {
    return false;
  }
  std::cerr << "calling getNewReferencePoint for next_reference_point" << std::endl;
  std::cerr << "----------------------------------------------current reference type " 
  << static_cast<int>(current_reference_point.reference_type_info.type) << std::endl;
  //change look ahead distance based on current velocity
  const double lookahead_distance_ratio = 4.0;
  double lookahead_distance = fabs(origin_linear_velocity) * lookahead_distance_ratio;
  const double minimum_lookahed_distance = 12.0;
  if(lookahead_distance < minimum_lookahed_distance)
  {
    lookahead_distance = minimum_lookahed_distance;
  }
  
  double max_distance = -9999;
  autoware_msgs::Waypoint default_target_waypoint;
  for(const auto& waypoint: reference_waypoints)
  {
    //TODO: make method 
    geometry_msgs::Point relative_cartesian_point =  
    transformToRelativeCoordinate2D(current_reference_point.cartesian_point, waypoint.pose.pose);
    double angle = std::atan2(relative_cartesian_point.y, relative_cartesian_point.x);
    
    //       |
    //       | abs(angle)<PI/2
    //-------wp-------
    //       | 
    //       | abs(angle)>PI/2
    //
    if(std::abs(angle) < M_PI/2)
    {
      continue;
    }
    
    double dx = waypoint.pose.pose.position.x - current_reference_point.cartesian_point.x;
    double dy = waypoint.pose.pose.position.y - current_reference_point.cartesian_point.y;
    double distance = std::sqrt(std::pow(dx, 2)+std::pow(dy,2));
    // if(distance < search_radius_for_target_point_ && distance > max_distance)
    if(distance < lookahead_distance && distance > max_distance)
    {
      max_distance = distance;
      default_target_waypoint = waypoint;
    }
  }
  
  FrenetPoint default_reference_frenet_point;
  convertWaypoint2FrenetPoint(
    default_target_waypoint.pose.pose.position,
    default_target_waypoint.twist.twist.linear.x,
    lane_points,
    default_reference_frenet_point);
  
  
  //TODO: change the behavior here based on current_reference_point's reference type
  //TODO: need refactor 
  // only assuming avoid obstacle by moving right
  ReferenceTypeInfo reference_type_info;
  reference_type_info.type = ReferenceType::Unknown;
  size_t collision_object_id = 0;
  size_t collision_object_index = 0;
  reference_type_info.referencing_object_id = collision_object_id;
  reference_type_info.referencing_object_index = collision_object_index;
  FrenetPoint reference_frenet_point;
  geometry_msgs::Point reference_cartesian_point;
  if(current_reference_point.reference_type_info.type != ReferenceType::Obstacle)
  {
    
    double time_horizon = 8.0;
    Trajectory trajectory;
    getTrajectory(lane_points,
                  reference_waypoints,
                  current_reference_point.frenet_point,
                  default_reference_frenet_point,
                  time_horizon,
                  dt_for_sampling_points_,
                  trajectory);
    size_t collision_waypoint_index = 0;
    bool is_collision_free = true;
    if(objects_ptr)
    {
      is_collision_free = isTrajectoryCollisionFree(
                                trajectory.trajectory_points.waypoints,
                                *objects_ptr,
                                collision_waypoint_index,
                                collision_object_id,
                                collision_object_index);
    }
    
    if(is_collision_free)
    {
      if(default_target_waypoint.twist.twist.linear.x < 0.01)
      {
        reference_type_info.type = ReferenceType::StopLine;
      }
      else
      {
        reference_type_info.type = ReferenceType::Waypoint;
      }
      
      reference_frenet_point = default_reference_frenet_point;
      reference_cartesian_point = default_target_waypoint.pose.pose.position;
    }
    else
    {
      // collision is detected
      std::cerr << "Put stop at collision point inside getNewReferencePoint " << std::endl;
      
      double min_dist = 99999;
      geometry_msgs::Point collision_point = trajectory.trajectory_points.waypoints[collision_waypoint_index].pose.pose.position;
      std::vector<autoware_msgs::Waypoint> waypoints = trajectory.trajectory_points.waypoints;
      geometry_msgs::Point tmp_reference_cartesian_point;
      bool has_got_reference_cartesian_point = false;
      for(size_t i = 0; i < collision_waypoint_index; i++)
      {
        double distance = calculate2DDistace(collision_point, waypoints[i].pose.pose.position);
        if(distance < min_dist && distance > distance_before_obstalcle_)
        {
          min_dist = distance;
          tmp_reference_cartesian_point = waypoints[i].pose.pose.position;
          has_got_reference_cartesian_point = true;
        }
      }
      if(!has_got_reference_cartesian_point)
      {
        std::cerr << "could not find distance_before_obstacle--------------------------------------"  << std::endl;
        tmp_reference_cartesian_point = waypoints.front().pose.pose.position;
      }
      else
      {
        std::cerr << "min dist " << min_dist << std::endl;
        std::cerr << "Did find distance_before_obst!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!1"  << std::endl;
      }
      
      convertWaypoint2FrenetPoint(
        tmp_reference_cartesian_point,
        velcity_before_obstalcle_m_s_,
        lane_points,
        reference_frenet_point);
      reference_type_info.type = ReferenceType::Obstacle;
      reference_type_info.referencing_object_id = collision_object_id;
      reference_type_info.referencing_object_index = collision_object_index;
      reference_cartesian_point = tmp_reference_cartesian_point;
      reference_frenet_point = reference_frenet_point;
    }
  }
  else
  {//search for avoiding point
    const double lateral_max_offset = 7.0;
    const double lateral_sampling_resolution = 1.0;
    bool has_got_collision_free_trajectory = false;
    for(double lateral_offset = 0.0;
        lateral_offset <= lateral_max_offset;
        lateral_offset+=lateral_sampling_resolution)
    {
      double time_horizon = 12.0;
      FrenetPoint offset_reference_frenet_point;
      offset_reference_frenet_point = default_reference_frenet_point;
      offset_reference_frenet_point.d_state(0) += lateral_offset;
      Trajectory trajectory;
      getTrajectory(lane_points,
                    reference_waypoints,
                    current_reference_point.frenet_point,
                    offset_reference_frenet_point,
                    time_horizon,
                    dt_for_sampling_points_,
                    trajectory);
      size_t collision_waypoint_index;
      size_t collision_object_id;
      size_t collision_object_index;
      bool is_collision_free = true;
      if(objects_ptr)
      {
        is_collision_free = isTrajectoryCollisionFree(
                                  trajectory.trajectory_points.waypoints,
                                  *objects_ptr,
                                  collision_waypoint_index,
                                  collision_object_id,
                                  collision_object_index);
      }
      
      if(is_collision_free)
      {
        std::cerr << "lateral offset " << lateral_offset << std::endl;
        reference_type_info.type = ReferenceType::AvoidingPoint;
        reference_frenet_point = offset_reference_frenet_point;
        reference_cartesian_point = trajectory.trajectory_points.waypoints.back().pose.pose.position;
        has_got_collision_free_trajectory = true;
        break;
      }
    }
    if(!has_got_collision_free_trajectory)
    {
      std::cerr << "error: could not find safe reference point; will stop at current reference point "  << std::endl;
      reference_type_info.type = ReferenceType::Unknown;
      reference_frenet_point = current_reference_point.frenet_point;
      reference_frenet_point.s_state(1) = 0.0;
      reference_cartesian_point = current_reference_point.cartesian_point;
    }
  }
  
  
  
  
  if(reference_type_info.type == ReferenceType::Waypoint)
  {
    reference_point.frenet_point = reference_frenet_point;
    reference_point.cartesian_point = reference_cartesian_point;
    reference_point.lateral_max_offset = 0.0;
    reference_point.lateral_sampling_resolution =0.01;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 8.0;
    reference_point.time_horizon_sampling_resolution = 1.0;
    reference_point.reference_type_info = reference_type_info;
  }
  else if(reference_type_info.type == ReferenceType::Obstacle)
  {
    reference_point.frenet_point = reference_frenet_point;
    reference_point.cartesian_point = reference_cartesian_point;
    reference_point.lateral_max_offset = 0.0;
    reference_point.lateral_sampling_resolution = 0.01;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 4.0;
    reference_point.time_horizon_sampling_resolution = 2.0;
    reference_point.reference_type_info = reference_type_info;
  }
  else if(reference_type_info.type == ReferenceType::AvoidingPoint)
  {
    reference_point.frenet_point = reference_frenet_point;
    reference_point.frenet_point.s_state(1)*= 0.75;
    reference_point.cartesian_point = reference_cartesian_point;
    reference_point.lateral_max_offset = 3.0;
    reference_point.lateral_sampling_resolution = 0.5;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 10.0;
    reference_point.time_horizon_max_offset = 4.0;
    reference_point.time_horizon_sampling_resolution = 1.0;
    reference_point.reference_type_info = reference_type_info;
  }
  else if(reference_type_info.type == ReferenceType::StopLine)
  {
    reference_point.frenet_point = reference_frenet_point;
    reference_point.cartesian_point = reference_cartesian_point;
    reference_point.lateral_max_offset = 0.0;
    reference_point.lateral_sampling_resolution =0.01;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 8.0;
    reference_point.time_horizon_sampling_resolution = 1.0;
    reference_point.reference_type_info = reference_type_info;
  }
  else
  {
    std::cerr << "stop at current reference point"  << std::endl;
    reference_point.frenet_point = reference_frenet_point;
    reference_point.cartesian_point = reference_cartesian_point;
    reference_point.lateral_max_offset = 0.0;
    reference_point.lateral_sampling_resolution = 0.01;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 4.0;
    reference_point.time_horizon_sampling_resolution = 2.0;
    reference_point.reference_type_info = reference_type_info;
  }
  return true;
}

//TODO: not considering the size of waypoints
bool FrenetPlanner::generateInitialReferencePoint(
  const geometry_msgs::Point& origin_cartesian_point,
  const double origin_linear_velocity,  
  const std::vector<autoware_msgs::Waypoint>& reference_waypoints,
  const std::vector<Point>& lane_points,
  ReferencePoint& reference_point)
{
  std::cerr << "calling getInitialReferencePoint" << std::endl;
  //change look ahead distance based on current velocity
  const double lookahead_distance_ratio = 4.0;
  double lookahead_distance = fabs(origin_linear_velocity) * lookahead_distance_ratio;
  const double minimum_lookahed_distance = 12.0;
  if(lookahead_distance < minimum_lookahed_distance)
  {
    lookahead_distance = minimum_lookahed_distance;
  }
  
  double max_distance = -9999;
  autoware_msgs::Waypoint default_reference_waypoint;
  for(const auto& waypoint: reference_waypoints)
  {
    double dx = waypoint.pose.pose.position.x - origin_cartesian_point.x;
    double dy = waypoint.pose.pose.position.y - origin_cartesian_point.y;
    double distance = std::sqrt(std::pow(dx, 2)+std::pow(dy,2));
    // if(distance < search_radius_for_target_point_ && distance > max_distance)
    if(distance < lookahead_distance && distance > max_distance)
    {
      max_distance = distance;
      default_reference_waypoint = waypoint;
    }
  }
  
  FrenetPoint default_reference_frenet_point;
  convertWaypoint2FrenetPoint(
    default_reference_waypoint.pose.pose.position,
    default_reference_waypoint.twist.twist.linear.x,
    lane_points,
    default_reference_frenet_point);
  
  reference_point.frenet_point = default_reference_frenet_point;
  reference_point.lateral_max_offset = 0.0;
  reference_point.lateral_sampling_resolution =0.01;
  reference_point.longutudinal_max_offset = 0.0;
  reference_point.longutudinal_sampling_resolution = 0.01;
  reference_point.time_horizon = 12.0;
  reference_point.time_horizon_max_offset = 10.0;
  reference_point.time_horizon_sampling_resolution = 2.0;
  reference_point.reference_type_info.type = ReferenceType::Waypoint;
  reference_point.cartesian_point = default_reference_waypoint.pose.pose.position;
  return true;
}

bool FrenetPlanner::updateReferencePoint(
    const std::unique_ptr<Trajectory>& kept_trajectory,
    const std::vector<autoware_msgs::Waypoint>& local_reference_waypoints,
    const std::vector<Point>& lane_points,    
    const std::unique_ptr<autoware_msgs::DetectedObjectArray>& objects_ptr,
    const std::unique_ptr<ReferencePoint>& kept_reference_point,
    ReferencePoint& reference_point)
{
  std::cerr << "calling updateReferencePoint" << std::endl;
  if(!kept_trajectory)
  {
    std::cerr << "error: kept trajectory coulf not be nullptr in updateReferencePoint" << std::endl;
    return false;
  }
  
  //not update reference point when kept_reference point is equal to last waypoint
  double distance = calculate2DDistace(kept_reference_point->cartesian_point, 
                                       local_reference_waypoints.back().pose.pose.position);
  if(distance < 0.1)
  {
    return false;
  }
  
  
  // if containing flagged waypoint, update kept trajectory
  autoware_msgs::Waypoint flagged_waypoint;
  bool found_flagged_waypoint = includeFlaggedWaypoint(local_reference_waypoints, flagged_waypoint);
  
  //TODO: find better way; thie loop has 2 differenet meaning
  //TODO: ideally reference_point need to be considered with keep distance from the objects
  //       plus, has semantic meanings such as lateral_max_offset, time_horizon, etc                                                                                       
  //incident check
  //TODO: seek better way to deal with flag
  bool found_new_reference_point = false;
  double min_dist = 9999;
  autoware_msgs::Waypoint reference_waypoint;
  std::vector<autoware_msgs::Waypoint> current_trajectory_points = 
  kept_trajectory->trajectory_points.waypoints;
  for(size_t i = 0; i < current_trajectory_points.size(); i++)
  {
    bool is_collision = false;
    if(objects_ptr)
    {
      is_collision = isCollision(current_trajectory_points[i], *objects_ptr);
    }
    
    if(is_collision)
    {
      reference_waypoint = current_trajectory_points[i];
      found_new_reference_point = true;
      reference_point.reference_type_info.type = ReferenceType::Obstacle;
      std::cerr << "Detect collision while updating reference point!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
      break;
    } 
    // find closest waypoint with flagged waypoint
    if(found_flagged_waypoint)
    {
      double distance = calculate2DDistace(flagged_waypoint.pose.pose.position,
                                           current_trajectory_points[i].pose.pose.position);
      if(distance < 3 && distance < min_dist)
      {
        min_dist = distance;
        reference_waypoint = flagged_waypoint;
        found_new_reference_point = true;
        reference_point.reference_type_info.type = ReferenceType::StopLine;
        std::cerr << "found flagged point!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"  << std::endl;
        break;
      }
    }
  }
  if(!found_new_reference_point)
  {
    return found_new_reference_point;
  }
  
  FrenetPoint reference_frenet_point;
  convertWaypoint2FrenetPoint(
    reference_waypoint.pose.pose.position,
    reference_waypoint.twist.twist.linear.x,
    lane_points,
    reference_frenet_point);
  
  reference_point.frenet_point = reference_frenet_point;
  
  if(reference_waypoint.twist.twist.linear.x < 0.1)
  {
    reference_point.lateral_max_offset = 0.0;
    reference_point.lateral_sampling_resolution = 0.01;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 4.0;
    reference_point.time_horizon_sampling_resolution = 2.0;
  }
  else
  {
    reference_point.lateral_max_offset = 4.0;
    reference_point.lateral_sampling_resolution = 2.0;
    reference_point.longutudinal_max_offset = 0.0;
    reference_point.longutudinal_sampling_resolution = 0.01;
    reference_point.time_horizon = 12.0;
    reference_point.time_horizon_max_offset = 10.0;
    reference_point.time_horizon_sampling_resolution = 2.0;
    
  }
  reference_point.cartesian_point = reference_waypoint.pose.pose.position;
  return found_new_reference_point;
  
}

bool FrenetPlanner::includeFlaggedWaypoint(
  const std::vector<autoware_msgs::Waypoint>& waypoints,
  autoware_msgs::Waypoint& flagged_waypoint)
{
  for(const auto& waypoint: waypoints)
  {
    if(waypoint.twist.twist.linear.x < 0.1)
    {
      flagged_waypoint = waypoint;
      return true; 
    }
  }
  return false;
}

bool FrenetPlanner::isCollision(const autoware_msgs::Waypoint& waypoint,
                                const autoware_msgs::DetectedObjectArray& objects)
{
  //TODO: more sophisticated collision check
  for(const auto& object: objects.objects)
  {
    double distance = calculate2DDistace(waypoint.pose.pose.position,
                                          object.pose.position);
    //TODO: paremater
    //assuming obstacle is not car but corn
    if(distance < 2.5)
    {
      return true;
    }
  }
  return false;
}

bool FrenetPlanner::isCollision(const autoware_msgs::Waypoint& waypoint,
                                const autoware_msgs::DetectedObjectArray& objects,
                                size_t& collision_object_id,
                                size_t& collision_object_index)
{
  //TODO: more sophisticated collision check
  for(size_t i = 0; i < objects.objects.size(); i++)
  {
    double distance = calculate2DDistace(waypoint.pose.pose.position,
                                          objects.objects[i].pose.position);
    //TODO: paremater
    //assuming obstacle is not car but corn
    if(distance < 2.5)
    {
      collision_object_id = objects.objects[i].id;
      collision_object_index = i;
      return true;
    }
  }
  return false;
}

//TODO: might make this rich
bool FrenetPlanner::isReferencePointValid(
  const geometry_msgs::Pose& ego_pose,
  const geometry_msgs::Point& cartesian_reference_point,
  const geometry_msgs::Point& last_reference_waypoint)
{
  //check if reference point is at the end of global waypoints
  double distance = calculate2DDistace(cartesian_reference_point,
                                       last_reference_waypoint);
  // std::cerr << "dist current target and last wp " << distance << std::endl;
  if(distance<0.1)
  {
    return true;
  }
  
  // // check if kept_current_trahectory is too short
  // // if too short, current reference point is no longer valid
  // //TODO: use of parameter/variable
  // if(num_kept_current_trajectory_points <= 2)
  // {
  //   return false;
  // }
  
  // check if current_reference_point is behingd ego 
  geometry_msgs::Point relative_cartesian_point =  
  transformToRelativeCoordinate2D(cartesian_reference_point, ego_pose);
  double angle = std::atan2(relative_cartesian_point.y, relative_cartesian_point.x);
  
  //       |
  //       | abs(angle)<PI/2
  //------ego-------
  //       | 
  //       | abs(angle)>PI/2
  //
  if(std::abs(angle) < M_PI/2)
  {
    return true;
  }
  else
  {
    return false;
  }
}


//TODO: draw trajectories based on reference_point parameters
bool FrenetPlanner::drawTrajectories(
              const FrenetPoint& frenet_current_point,
              const ReferencePoint& reference_point,
              const std::vector<Point>& lane_points,
              const std::vector<autoware_msgs::Waypoint>& reference_waypoints,
              std::vector<Trajectory>& trajectories,
              std::vector<autoware_msgs::Lane>& out_debug_trajectories)
{
  std::cerr << "lateral offset " << reference_point.lateral_max_offset << std::endl;
  std::cerr << "lateral samp " << reference_point.lateral_sampling_resolution << std::endl;
  std::cerr << "long offset " << reference_point.longutudinal_max_offset << std::endl;
  std::cerr << "long samp "   << reference_point.longutudinal_sampling_resolution<< std::endl;
  std::cerr << "th default " << reference_point.time_horizon << std::endl;
  std::cerr << "th offset " << reference_point.time_horizon_max_offset << std::endl;
  std::cerr << "th samp "   << reference_point.time_horizon_sampling_resolution<< std::endl;
  for(double lateral_offset = -1*reference_point.lateral_max_offset; 
      lateral_offset<= reference_point.lateral_max_offset; 
      lateral_offset+=reference_point.lateral_sampling_resolution)
  {
    for(double longitudinal_offset = -1*reference_point.longutudinal_max_offset;
        longitudinal_offset<= reference_point.longutudinal_max_offset;
        longitudinal_offset+= reference_point.longutudinal_sampling_resolution)
    {
      for(double time_horizon_offset = -1*reference_point.time_horizon_max_offset;
          time_horizon_offset<=reference_point.time_horizon_max_offset;
          time_horizon_offset+=reference_point.time_horizon_sampling_resolution)
      {
        Eigen::Vector3d target_d = reference_point.frenet_point.d_state;
        Eigen::Vector3d target_s = reference_point.frenet_point.s_state;
        target_d(0) += lateral_offset;
        target_s(0) += longitudinal_offset;
        FrenetPoint frenet_target_point;
        frenet_target_point.d_state = target_d;
        frenet_target_point.s_state = target_s;
        double target_time_horizon = reference_point.time_horizon + time_horizon_offset;
        Trajectory trajectory;
        getTrajectory(
            lane_points,
            reference_waypoints,
            frenet_current_point,
            frenet_target_point,
            target_time_horizon,
            dt_for_sampling_points_,
            trajectory);
          trajectories.push_back(trajectory);
          out_debug_trajectories.push_back(trajectory.trajectory_points);
      }
    }
  }
}


//TODO: gross interface; currently pass reference point if true, pass tajectory if false
// you gotta think better way
bool FrenetPlanner::getOriginPointAndReferencePoint(
    const geometry_msgs::Pose& ego_pose,
    const double ego_linear_velocity,
    const std::vector<autoware_msgs::Waypoint>& reference_waypoints,    
    const std::vector<Point>& lane_points,
    const std::unique_ptr<autoware_msgs::DetectedObjectArray>& objects_ptr,
    FrenetPoint& origin_frenet_point,
    std::unique_ptr<ReferencePoint>& current_reference_point,
    std::unique_ptr<Trajectory>& kept_current_trajectory)
{
  //TODO: seek more readable code
  //TODO: think the interface between the components
  bool is_new_reference_point = false;
  if(current_reference_point)
  {
    //calculate origin point
    if(!kept_current_trajectory)
    {
      std::cerr << "kept trajectory is empty; something is wrong" << std::endl;
    }
    else
    {
      origin_frenet_point = kept_current_trajectory->frenet_trajectory_points.front(); 
    }
    
    ReferencePoint target_point;
    if(updateReferencePoint(kept_current_trajectory_,
                      reference_waypoints,
                      lane_points,
                      objects_ptr,
                      current_reference_point,
                      target_point))
    {
      current_reference_point.reset(new ReferencePoint(target_point));
      is_new_reference_point = true;
    }
  }
  else
  {//initialize when current_reference_point is nullptr
    FrenetPoint frenet_point;
    autoware_msgs::Waypoint nearest_waypoint;
    getNearestWaypoint(ego_pose.position, 
                       reference_waypoints,
                       nearest_waypoint);
    convertWaypoint2FrenetPoint(
      nearest_waypoint.pose.pose.position,
      initial_velocity_m_s_,
      lane_points,
      frenet_point);
    origin_frenet_point = frenet_point;
    
    ReferencePoint frenet_target_point;
    generateInitialReferencePoint(ego_pose.position,
                      ego_linear_velocity,
                      reference_waypoints,
                      lane_points,
                      frenet_target_point);
    current_reference_point.reset(new ReferencePoint(frenet_target_point));
    is_new_reference_point = true;
  }
  return is_new_reference_point;
}

bool FrenetPlanner::getNextOriginPointAndReferencePoint(
    const geometry_msgs::Pose& ego_pose,
    const double origin_linear_velocity,
    const std::vector<autoware_msgs::Waypoint>& reference_waypoints,
    const std::vector<Point>& lane_points,
    const std::unique_ptr<autoware_msgs::DetectedObjectArray>& objects_ptr,
    std::unique_ptr<Trajectory>& kept_current_trajectory,
    std::unique_ptr<Trajectory>& kept_next_trajectory,
    std::unique_ptr<ReferencePoint>& kept_current_reference_point,
    std::unique_ptr<ReferencePoint>& kept_next_reference_point,
    FrenetPoint& next_origin_point)
{
  //validity check for current referencer point
  if(!isReferencePointValid(ego_pose,
                           kept_current_reference_point->cartesian_point,
                           reference_waypoints.back().pose.pose.position))
  {
    std::cerr << "reference point is not valid" << std::endl;
    if(!kept_next_reference_point)
    {
      std::cerr << "kept_next_reference_point is nullptr" << std::endl;
    }
    else
    {
      kept_current_reference_point.reset(new ReferencePoint(*kept_next_reference_point));
      kept_next_reference_point = nullptr;
      
      if(kept_next_trajectory)
      {
        std::cerr << "kept next trajector is not nullptr " << std::endl;
        std::cerr << "kept next trajectory; num wps " << kept_next_trajectory->frenet_trajectory_points.size()<< std::endl;
        kept_current_trajectory->trajectory_points.waypoints.insert
                (kept_current_trajectory->trajectory_points.waypoints.end(),
                  kept_next_trajectory->trajectory_points.waypoints.begin(),
                  kept_next_trajectory->trajectory_points.waypoints.end());
        kept_current_trajectory->frenet_trajectory_points.insert
                (kept_current_trajectory->frenet_trajectory_points.end(),
                  kept_next_trajectory->frenet_trajectory_points.begin(),
                  kept_next_trajectory->frenet_trajectory_points.end());
        std::cerr << "after concat; num wps " << kept_current_trajectory->frenet_trajectory_points.size()<< std::endl;
      }
      else
      {
        std::cerr << "kept next trajectory is nullptr"  << std::endl;
      }
      
      kept_next_trajectory = nullptr;
      std::cerr << "replace curernt target with next target" << std::endl;
      //false = no need to draw trajectory and get best trajectory        
    }
  }
  else
  {
    std::cerr << "reference point is valid" << std::endl;
  }
  
  //validity check for current trajectory
  // update kept_trajectory based on current pose
  if(kept_current_trajectory)
  {
    std::cerr << "before current crop " << kept_current_trajectory->trajectory_points.waypoints.size() << std::endl;
    Trajectory dc_kept_trajectory = *kept_current_trajectory;
    for(const auto& waypoint: dc_kept_trajectory.trajectory_points.waypoints)
    {
      //TODO: make method for this
      // check if current_point is behingd ego 
      geometry_msgs::Point relative_cartesian_point =  
      transformToRelativeCoordinate2D(waypoint.pose.pose.position, ego_pose);
      double angle = std::atan2(relative_cartesian_point.y, relative_cartesian_point.x);
      //       |
      //       | abs(angle)<PI/2
      //------ego-------
      //       | 
      //       | abs(angle)>PI/2
      //
      if(std::abs(angle) > M_PI/2)
      {
        //TODO: not good implementation. this is because pure pursuit would die if 0 trajectory is passed
        if(kept_current_trajectory->frenet_trajectory_points.size() > 1)
        {
          kept_current_trajectory->trajectory_points.waypoints.erase(
              kept_current_trajectory->trajectory_points.waypoints.begin());
          kept_current_trajectory->frenet_trajectory_points.erase(
            kept_current_trajectory->frenet_trajectory_points.begin());
          
        }
      }
    }
    std::cerr << "after current crop "<< kept_current_trajectory->trajectory_points.waypoints.size()  << std::endl;
    if(kept_current_trajectory->frenet_trajectory_points.size() == 0)
    {
      std::cerr << "kept_current_trajectory is no longer valid"  << std::endl;
      if(kept_next_trajectory)
      {
        kept_current_trajectory.reset(new Trajectory(*kept_next_trajectory));
        kept_next_trajectory = nullptr;
        
      }
      if(kept_next_reference_point)
      {
        kept_current_reference_point.reset(new ReferencePoint(*kept_next_reference_point));
        kept_next_reference_point = nullptr;
      }
    }
  }
  
  //validity check for next trajectory
  // update kept_trajectory based on current pose
  if(kept_next_trajectory)
  {
    std::cerr << "before next crop " << kept_next_trajectory->trajectory_points.waypoints.size() << std::endl;
    Trajectory dc_kept_trajectory = *kept_next_trajectory;
    for(const auto& waypoint: dc_kept_trajectory.trajectory_points.waypoints)
    {
      //TODO: make method for this
      // check if current_point is behingd ego 
      geometry_msgs::Point relative_cartesian_point =  
      transformToRelativeCoordinate2D(waypoint.pose.pose.position, ego_pose);
      double angle = std::atan2(relative_cartesian_point.y, relative_cartesian_point.x);
      //       |
      //       | abs(angle)<PI/2
      //------ego-------
      //       | 
      //       | abs(angle)>PI/2
      //
      if(std::abs(angle) > M_PI/2)
      {
        kept_next_trajectory->trajectory_points.waypoints.erase(
            kept_next_trajectory->trajectory_points.waypoints.begin());
        kept_next_trajectory->frenet_trajectory_points.erase(
          kept_next_trajectory->frenet_trajectory_points.begin());
      }
    }
    std::cerr << "after next crop "<< kept_next_trajectory->trajectory_points.waypoints.size()  << std::endl;
    if(kept_next_trajectory->frenet_trajectory_points.size() == 0)
    {
      std::cerr << "make both next nullptr"  << std::endl;
      kept_next_trajectory = nullptr;
      kept_next_reference_point = nullptr;
    }
  }
  
  if(kept_current_trajectory)
  {
    std::cerr << "kept_current_taj extits"  << std::endl;
    std::cerr << "current traj size" << kept_current_trajectory->frenet_trajectory_points.size() << std::endl;
  }
  else
  {
    std::cerr << "kept_current_taj nullptr"  << std::endl;
  }
  if(kept_current_reference_point)
  {
    std::cerr << "current reference extits"  << std::endl;
  }
  else
  {
    std::cerr << "current reference nullptr"  << std::endl;
  }
  
  if(kept_next_trajectory)
  {
    std::cerr << "kept_next_taj extits"  << std::endl;
    std::cerr << "current traj size" << kept_next_trajectory->frenet_trajectory_points.size() << std::endl;
  }
  else
  {
    std::cerr << "kept_next_taj nullptr"  << std::endl;
  }
  
  if(kept_next_reference_point)
  {
    std::cerr << "next reference extits"  << std::endl;
  }
  else
  {
    std::cerr << "next reference nullptr"  << std::endl;
  }

  //update or generate or nothing especially for next reference point
  double distance = calculate2DDistace(ego_pose.position,
                                       kept_current_reference_point->cartesian_point);
  bool is_new_reference_point = false;
  if(kept_next_reference_point)
  {
    //update target point if something is on trajectory
    ReferencePoint next_point;
    if(updateReferencePoint(kept_next_trajectory,
                      reference_waypoints,
                      lane_points,
                      objects_ptr,
                      kept_next_reference_point,
                      next_point))
    {
      std::cerr << "updating next reference point" << std::endl;
      kept_next_reference_point.reset(new ReferencePoint(next_point));
      is_new_reference_point =  true;
    }
  }
  //TODO: use of param/variable
  else if(distance < 10 && !kept_next_reference_point)
  {
    std::cerr << "start generating new reference point" << std::endl;
    //make initinal target point
    ReferencePoint next_point;
    is_new_reference_point = 
       generateNewReferencePoint(
                          *kept_current_reference_point,
                          origin_linear_velocity,
                          reference_waypoints,
                          lane_points,
                          objects_ptr,
                          next_point);
    if(is_new_reference_point)
    {
      kept_next_reference_point.reset(new ReferencePoint(next_point));
    }
    // is_new_reference_point = ;
  }
  else
  {
    std::cerr << "next target is nullptr and distance > 10"  << std::endl;
    std::cerr << "current traj size" << kept_current_trajectory->frenet_trajectory_points.size() << std::endl;
  }
  
  
  if(!is_new_reference_point)
  {
    return is_new_reference_point;
  }
  
  std::cerr << "next reference type " << static_cast<int>(kept_next_reference_point->reference_type_info.type) << std::endl;
  std::cerr << "next reference v " << kept_next_reference_point->frenet_point.s_state(1) << std::endl;
  // //validity check for next reference point
  if(kept_next_reference_point->reference_type_info.type == 
     ReferenceType::Obstacle)
  {
    size_t collision_object_index = kept_next_reference_point->reference_type_info.referencing_object_index;
    std::cerr << "collision object index " << collision_object_index << std::endl;
    geometry_msgs::Point collsion_object_point = 
       objects_ptr->objects[collision_object_index].pose.position;
    double distance = calculate2DDistace(collsion_object_point, 
                                         kept_next_reference_point->cartesian_point);
    
    if(distance < distance_before_obstalcle_)
    {
      double min_distance = 99999;
      size_t reference_point_index = 0;
      bool found_reference_point = false;
      std::vector<autoware_msgs::Waypoint> waypoints =
        kept_current_trajectory->trajectory_points.waypoints;
      for(size_t i = 0; i < waypoints.size(); i++)
      {
        double tmp_distance = calculate2DDistace(waypoints[i].pose.pose.position,
                                                 collsion_object_point); 
        if(tmp_distance < min_distance && tmp_distance > distance_before_obstalcle_)
        {
          min_distance = tmp_distance;
          reference_point_index = i;
          found_reference_point = true;
        }
      }
      if(!found_reference_point)
      {
        std::cerr << "WARNING: could not find better next reference point; in getNextOriginPointAndReferencePoint"<< std::endl;
      }
      else
      {
        std::cerr << "dif find appropriate next reference point from current trajecotory"  << std::endl;
        std::cerr << "min_dist " << min_distance << std::endl;
      }
      FrenetPoint tmp_frenet_point = kept_current_trajectory->frenet_trajectory_points[reference_point_index];
      tmp_frenet_point.s_state(1) = velcity_before_obstalcle_m_s_;
      kept_next_reference_point->frenet_point = tmp_frenet_point;
      kept_next_reference_point->cartesian_point = waypoints[reference_point_index].pose.pose.position;
      size_t current_trajectory_size = kept_current_trajectory->frenet_trajectory_points.size();
      for(size_t i = reference_point_index; i < current_trajectory_size; i++)
      {
        kept_current_trajectory->trajectory_points.waypoints.erase(
            kept_current_trajectory->trajectory_points.waypoints.end());
        kept_current_trajectory->frenet_trajectory_points.erase(
          kept_current_trajectory->frenet_trajectory_points.end());
      }
    }
  }
  
  
  //TODO: non-holnomic at low velocity; v_0.4
  // v0.3 something like below
  //if(next_origin_point.s_state(1) < 0.1 && ReferenceType == ReferenceType::Obstacle) 
  next_origin_point = kept_current_trajectory->frenet_trajectory_points.back();
  if(next_origin_point.s_state(1) < 0.1)
  {
    next_origin_point.s_state(1) += 0.3;
  } 
  
  //TODO: validation that make sure next_reference_point.s_p > current_reference_point.s_p
  if(kept_next_reference_point->frenet_point.s_state(0) <
     kept_current_reference_point->frenet_point.s_state(0))
  {
    std::cerr << "WARNING: next_reference_point is behind current_reference_point; getNextReferencePoint" << std::endl;
  }
  
  
  //offset only for stopping waypoint
  //validity check for current_reference_point 
  //if new_reference_point is too close to converge to 0
  if(kept_next_reference_point->reference_type_info.type 
     == ReferenceType::Obstacle ||
     kept_next_reference_point->reference_type_info.type
     == ReferenceType::StopLine)
  {
    Trajectory trajectory;
    getTrajectory(lane_points,
                  reference_waypoints,
                  next_origin_point,
                  kept_next_reference_point->frenet_point,
                  kept_next_reference_point->time_horizon,
                  dt_for_sampling_points_,
                  trajectory);
                  
    //triggered only if last point of actual trajectory is 
    //overshot compared with reference point
    double reference_s_position = kept_next_reference_point->frenet_point.s_state(0);
    double acutual_s_positon = trajectory.frenet_trajectory_points.back().s_state(0);
    if(reference_s_position < acutual_s_positon)
    {
      std::cerr << "offseeeeeeeeeeeeeeeeeeeeeeet"  << std::endl;
      double min_dist = 99999;
      size_t offset_index = 0;
      bool is_offset = false;
      const std::vector<autoware_msgs::Waypoint> waypoints = 
      kept_current_trajectory->trajectory_points.waypoints;
      std::cerr << "num wps " << waypoints.size() << std::endl;
      for(size_t i = 0; i < waypoints.size(); i++)
      {
        double dist = calculate2DDistace(waypoints[i].pose.pose.position,
                                        kept_next_reference_point->cartesian_point);
        //TODO: calculate this value by constrained optimization
        const double minimum_distance_to_converge = 10;
        if(dist < min_dist && dist > minimum_distance_to_converge)
        {
          min_dist = dist;
          offset_index = i;
          is_offset = true;
        }
      }
      
      if(!is_offset)
      {
        offset_index = 0;
      }
      
      kept_current_reference_point->cartesian_point = 
        kept_current_trajectory->trajectory_points.waypoints[offset_index].pose.pose.position;
      kept_current_reference_point->frenet_point = 
        kept_current_trajectory->frenet_trajectory_points[offset_index];
      size_t num_kept_current_waypoints = kept_current_trajectory->trajectory_points.waypoints.size();
      std::cerr << "num kept current traj  " << num_kept_current_waypoints << std::endl;
      std::cerr << "offset index " << offset_index << std::endl;
      for(size_t i = (num_kept_current_waypoints - 1); i > offset_index; i--)
      {
        kept_current_trajectory->trajectory_points.waypoints.erase(
              kept_current_trajectory->trajectory_points.waypoints.end());
        kept_current_trajectory->frenet_trajectory_points.erase(
          kept_current_trajectory->frenet_trajectory_points.end());
      }
      std::cerr << "after; num kept current traj points " << kept_current_trajectory->trajectory_points.waypoints.size() << std::endl;
      std::cerr << "next origin point " << kept_current_trajectory->frenet_trajectory_points.back().s_state(0) << std::endl;
      next_origin_point = kept_current_trajectory->frenet_trajectory_points.back();
      std::cerr << "next origin s_v " << next_origin_point.s_state(1) << std::endl;
    }
  }
  return is_new_reference_point;
}

//TODO: not good interface; has 2 meanings check if safe, get collision waypoint
bool FrenetPlanner::isTrajectoryCollisionFree(
    const std::vector<autoware_msgs::Waypoint>& trajectory_points,
    const autoware_msgs::DetectedObjectArray& objects,
    size_t& collision_waypoint_index,
    size_t& collision_object_id,
    size_t& collision_object_index)
{
  for(size_t i= 0; i< trajectory_points.size(); i++)
  {
    size_t tmp_collision_object_id;
    size_t tmp_collision_object_index;
    bool is_collision = isCollision(trajectory_points[i], 
                                    objects,
                                    tmp_collision_object_id,
                                    tmp_collision_object_index);
    if(is_collision)
    {
      collision_waypoint_index = i;
      collision_object_id = tmp_collision_object_id;
      collision_object_index = tmp_collision_object_index;
      return false;
    }
  }
  return true;
}

//not sure this overload is good or bad
bool FrenetPlanner::isTrajectoryCollisionFree(
    const std::vector<autoware_msgs::Waypoint>& trajectory_points,
    const autoware_msgs::DetectedObjectArray& objects)
{
  for(const auto& point: trajectory_points)
  {
    bool is_collision = isCollision(point, objects);
    if(is_collision)
    {
      return false;
    }
  }
  return true;
}

bool FrenetPlanner::convertWaypoint2FrenetPoint(
        const geometry_msgs::Point& cartesian_point,
        const double linear_velocity,
        const std::vector<Point> lane_points,        
        FrenetPoint& frenet_point)
{
  //calculate origin_point
  double frenet_s_position, frenet_d_position;
  convertCartesianPosition2FrenetPosition(
    cartesian_point,
    lane_points,        
    frenet_s_position,
    frenet_d_position);
  Eigen::Vector3d frenet_s, frenet_d;
  frenet_s << frenet_s_position,
              linear_velocity,
              0;
  frenet_d << frenet_d_position,
              0,
              0;
             
  frenet_point.s_state = frenet_s;
  frenet_point.d_state = frenet_d;
}