/**
 * @file constrained_ik.cpp
 * @brief Basic low-level kinematics functions.
 *
 * Typically, just wrappers around the equivalent KDL calls.
 *
 * @author dsolomon
 * @date Sep 15, 2013
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2013, Southwest Research Institute
 *
 * @license Software License Agreement (Apache License)\n
 * \n
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at\n
 * \n
 * http://www.apache.org/licenses/LICENSE-2.0\n
 * \n
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <constrained_ik/constrained_ik.h>
#include "constrained_ik/constraint_group.h"
#include <boost/make_shared.hpp>
#include <constrained_ik/constraint_results.h>
#include <ros/ros.h>

const std::string DEFAULT_COLLISION_DETECTOR = "IndustrialFCL";

namespace constrained_ik
{
using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::Affine3d;

Constrained_IK::Constrained_IK():nh_("~")
{
  initialized_ = false;
}

void Constrained_IK::dynamicReconfigureCallback(ConstrainedIKDynamicReconfigureConfig &config, uint32_t level)
{
  if (config.limit_auxiliary_motion)
  {
    if (config.auxiliary_norm > config.auxiliary_max_motion)
    {
      config.auxiliary_norm = config.auxiliary_max_motion;
    }
    else if (config.auxiliary_norm < config.auxiliary_max_motion)
    {
      unsigned int divisor = floor(config.auxiliary_max_motion/config.auxiliary_norm) + 1;
      config.auxiliary_norm = config.auxiliary_max_motion/divisor;
    }
  }
  config_ = config;
}

constrained_ik::ConstraintResults Constrained_IK::evalConstraint(constraint_types::ConstraintTypes constraint_type, const constrained_ik::SolverState &state) const
{
  switch(constraint_type)
  {
    case constraint_types::Primary:
      return primary_constraints_.evalConstraint(state);
    case constraint_types::Auxiliary:
      return auxiliary_constraints_.evalConstraint(state);
  }
}

Eigen::MatrixXd Constrained_IK::calcNullspaceProjection(const Eigen::MatrixXd &J) const
{
  MatrixXd J_pinv = calcDampedPseudoinverse(J);
  MatrixXd JplusJ = J_pinv * J;
  int mn = JplusJ.rows();
  MatrixXd P = MatrixXd::Identity(mn,mn)-JplusJ;
  return (P);
}

Eigen::MatrixXd Constrained_IK::calcNullspaceProjectionTheRightWay(const Eigen::MatrixXd &A) const
{
  Eigen::JacobiSVD<MatrixXd> svd(A, Eigen::ComputeFullV);
  MatrixXd V(svd.matrixV());

  // determine rank using same algorithym as latest Eigen
  // TODO replace next 10 lines of code with rnk = svd.rank(); once eigen3 is updated
  int rnk = 0;
  if(svd.singularValues().size()==0)
  {
    rnk = 0;
  }
  else
  {
    double threshold = std::min(A.rows(),A.cols())*Eigen::NumTraits<double>::epsilon();
    double premultiplied_threshold = svd.singularValues().coeff(0) * threshold;
    rnk = svd.nonzeroSingularValues()-1;
    while(rnk>=0 && svd.singularValues().coeff(rnk) < premultiplied_threshold) --rnk;
    rnk++;
  }

  // zero singular vectors in the range of A
  for(int i=0; i<rnk; ++i)
  {
    for(int j=0; j<A.cols(); j++) V(j,i) = 0;
  }

  MatrixXd P = V *  V.transpose();
  return(P);
}

Eigen::MatrixXd Constrained_IK::calcDampedPseudoinverse(const Eigen::MatrixXd &J) const
{
  MatrixXd J_pinv;

  if (basic_kin::BasicKin::dampedPInv(J,J_pinv))
  {
    return J_pinv;
  }
  else
  {
    ROS_ERROR_STREAM("Not able to calculate damped pseudoinverse!");
    throw std::runtime_error("Not able to calculate damped pseudoinverse!  IK solution may be invalid.");
  }
}

bool Constrained_IK::calcInvKin(const Eigen::Affine3d &goal,
                                const Eigen::VectorXd &joint_seed,
                                Eigen::VectorXd &joint_angles) const
{
  return calcInvKin(goal,joint_seed, planning_scene::PlanningSceneConstPtr(), joint_angles);
}

bool Constrained_IK::calcInvKin(const Eigen::Affine3d &goal,
                                const Eigen::VectorXd &joint_seed,
                                const planning_scene::PlanningSceneConstPtr planning_scene,
                                Eigen::VectorXd &joint_angles) const
{
  double dJoint_norm;
  SolverStatus status;

    // initialize state
  joint_angles = joint_seed;  // initialize result to seed value
  constrained_ik::SolverState state = getState(goal, joint_seed); // create state vars for this IK solve
  state.condition = checkInitialized();
  state.planning_scene = planning_scene;

  //TODO: Does this still belong here?
  if(planning_scene)
  {
    state.robot_state = robot_state::RobotStatePtr(new moveit::core::RobotState(planning_scene->getCurrentState()));

    //Check and make sure the correct collision detector is loaded.
    if (planning_scene->getActiveCollisionDetectorName() != DEFAULT_COLLISION_DETECTOR)
    {
      throw std::runtime_error("Constrained IK requires the use of collision detector \"" + DEFAULT_COLLISION_DETECTOR + "\"\n"
                               "To resolve the issue add the ros parameter collision_detector = " + DEFAULT_COLLISION_DETECTOR +
                               ".\nIt is recommend to added it where the move_group node is launched, usually in the in the "
                               "(robot_name)_moveit_config/launch/move_group.launch");
    }

    state.collision_robot = boost::dynamic_pointer_cast<const collision_detection::CollisionRobotIndustrial>(planning_scene->getCollisionRobot());
    state.collision_world = boost::dynamic_pointer_cast<const collision_detection::CollisionWorldIndustrial>(planning_scene->getCollisionWorld());
  }

  if (state.condition == initialization_state::NothingInitialized || state.condition == initialization_state::AuxiliaryOnly)
    throw std::runtime_error("Must call init() before using Constrained_IK and have a primary constraint.");

  //Cache the joint angles to return if max iteration is reached.
  Eigen::VectorXd cached_joint_angles = joint_seed;

  // iterate until solution converges (or aborted)
  while (true)
  {
    // re-update internal state variables
    updateState(state, joint_angles);

    // calculate a Jacobian (relating joint-space updates/deltas to cartesian-space errors/deltas)
    // and the associated cartesian-space error/delta vector
    // Primary Constraints
    constrained_ik::ConstraintResults primary = evalConstraint(constraint_types::Primary, state);
    // TODO since we already have J_p = USV, use that to get null-projection too.
    // otherwise, we are repeating the expensive calculation of the SVD
    MatrixXd Ji_p = calcDampedPseudoinverse(primary.jacobian);
    VectorXd dJoint_p = config_.primary_gain*(Ji_p*primary.error);
    dJoint_norm = dJoint_p.norm();
    if(config_.allow_primary_normalization && dJoint_norm > config_.primary_norm)// limit maximum update radian/meter
    {
      dJoint_p = config_.primary_norm * (dJoint_p/dJoint_norm);
      dJoint_norm = dJoint_p.norm();
    }
    state.primary_sum += dJoint_norm;

    // Auxiliary Constraints
    VectorXd dJoint_a;
    constrained_ik::ConstraintResults auxiliary;
    dJoint_a.setZero(dJoint_p.size());
    if (state.condition == initialization_state::PrimaryAndAuxiliary)
    {
      if (!((config_.limit_auxiliary_motion && state.auxiliary_sum >= config_.auxiliary_max_motion) ||
          (config_.limit_auxiliary_interations && state.iter > config_.auxiliary_max_iterations)))
      {
        auxiliary = evalConstraint(constraint_types::Auxiliary, state);
        if (!auxiliary.isEmpty()) // This is required because not all constraints always return data.
        {
          MatrixXd N_p = calcNullspaceProjectionTheRightWay(primary.jacobian);
          MatrixXd Jnull_a = calcDampedPseudoinverse(auxiliary.jacobian*N_p);
          dJoint_a = config_.auxiliary_gain*Jnull_a*(auxiliary.error-auxiliary.jacobian*dJoint_p);
          dJoint_norm = dJoint_a.norm();
          if(config_.allow_auxiliary_nomalization && dJoint_norm > config_.auxiliary_norm)// limit maximum update radian/meter
          {
            dJoint_a = config_.auxiliary_norm * (dJoint_a/dJoint_norm);
            dJoint_norm = dJoint_a.norm();
          }
          state.auxiliary_sum += dJoint_norm;
        }
      }
      else
      {
        state.auxiliary_at_limit = true;
      }
    }

    status = checkStatus(state, primary, auxiliary);
    
    
    if (status == Converged)
    {
      ROS_DEBUG_STREAM("IK solution: " << joint_angles.transpose());
      return true;
    }
    else if (status == NotConverged)
    {
      // update joint solution by the calculated update (or a partial fraction)
      joint_angles += (dJoint_p + dJoint_a);
      clipToJointLimits(joint_angles);
    }
    else if (status == Failed)
    {
      joint_angles = cached_joint_angles;
      return false;
    }
  }
}

SolverStatus Constrained_IK::checkStatus(const constrained_ik::SolverState &state, const constrained_ik::ConstraintResults &primary, const constrained_ik::ConstraintResults &auxiliary) const
{
  // Check the status of convergence
  if(state.condition == initialization_state::PrimaryAndAuxiliary)
  {
    bool status = (primary.status && auxiliary.status);

    if (!status && primary.status && state.auxiliary_at_limit && state.iter >= config_.solver_min_iterations)
    {
      ROS_DEBUG("Auxiliary motion or iteration limit reached!");
      return Converged;
    }
    else if(status && state.iter >= config_.solver_min_iterations)
    {
      return Converged;
    }
  }
  
  if(state.condition == initialization_state::PrimaryOnly)
  {   
    if (primary.status && state.iter >= config_.solver_min_iterations)
    {
      return Converged;
    }
  }

  // check for joint convergence
  //   - this is an error: joints stabilize, but goal pose not reached
  if (config_.allow_joint_convergence && state.joints_delta.cwiseAbs().maxCoeff() < config_.joint_convergence_tol && state.iter >= config_.solver_min_iterations)
  {
    ROS_DEBUG_STREAM("Joint convergence reached " << state.iter << " / " << config_.solver_max_iterations << " iterations before convergence.");
    return Converged;
  }
  
  if (state.iter > config_.solver_max_iterations || (config_.limit_primary_motion && state.primary_sum >= config_.primary_max_motion))
  {
    if (!primary.status)
    {
      if (config_.limit_primary_motion && state.primary_sum >= config_.primary_max_motion)
      {
        ROS_WARN_STREAM("Primary reached max allowed motion, no solution returned.");
      }
      else
      {
        ROS_WARN_STREAM("Solver reached max allowed iteration, no solution returned.");
      }
      return Failed;
    }
    else if (state.condition == initialization_state::PrimaryAndAuxiliary)
    {
      ROS_WARN_STREAM("Maximum iterations reached but primary converged so returning solution.");
      return Converged;
    }
  }

  return NotConverged;
}

void Constrained_IK::clearConstraintList()
{
  primary_constraints_.clear();
  auxiliary_constraints_.clear();
}

void Constrained_IK::clipToJointLimits(Eigen::VectorXd &joints) const
{
  const MatrixXd limits = kin_.getLimits();
  const VectorXd orig_joints(joints);

  if (joints.size() != limits.rows())
    throw std::invalid_argument("clipToJointLimits: Unexpected number of joints");

  for (size_t i=0; i<limits.rows(); ++i)
  {
    joints[i] = std::max(limits(i,0), std::min(limits(i,1), joints[i]));
  }
  if (config_.debug_mode && !joints.isApprox(orig_joints))
      ROS_WARN("Joints have been clipped");
}

void Constrained_IK::init(const basic_kin::BasicKin &kin)
{
  if (!kin.checkInitialized())
    throw std::invalid_argument("Input argument 'BasicKin' must be initialized");

  kin_ = kin;
  dynamic_reconfigure_server_.reset(new dynamic_reconfigure::Server<ConstrainedIKDynamicReconfigureConfig>(mutex_, ros::NodeHandle(nh_, "constrained_ik_solver/" + kin_.getJointModelGroup()->getName())));
  dynamic_reconfigure_server_->setCallback(boost::bind(&Constrained_IK::dynamicReconfigureCallback, this, _1, _2));
  initialized_ = true;
  primary_constraints_.init(this);
  auxiliary_constraints_.init(this);

}

double Constrained_IK::rangedAngle(double angle)
{
    angle = copysign(fmod(fabs(angle),2.0*M_PI), angle);
    if (angle < -M_PI) return angle+2.*M_PI;
    if (angle > M_PI)  return angle-2.*M_PI;
    return angle;
}

constrained_ik::SolverState Constrained_IK::getState(const Eigen::Affine3d &goal, const Eigen::VectorXd &joint_seed) const
{
  if (!kin_.checkJoints(joint_seed))
    throw std::invalid_argument("Seed doesn't match kinematic model");

  if (!goal.matrix().block(0,0,3,3).isUnitary(1e-6))
        throw std::invalid_argument("Goal pose not proper affine");

  return constrained_ik::SolverState(goal, joint_seed);
}

void Constrained_IK::updateState(constrained_ik::SolverState &state, const Eigen::VectorXd &joints) const
{
  // update maximum iterations
  state.iter++;

  // update joint convergence
  state.joints_delta = joints - state.joints;
  state.joints = joints;
  kin_.calcFwdKin(joints, state.pose_estimate);

  if(state.planning_scene && state.robot_state)
  {
    state.robot_state->setJointGroupPositions(kin_.getJointModelGroup()->getName(), joints);
    state.robot_state->update();
  }

  if (config_.debug_mode)
    state.iteration_path.push_back(joints);

}

} // namespace constrained_ik

