/*

Copyright (c) 2010, Stephan Weiss, ASL, ETH Zurich, Switzerland
You can contact the author at <stephan dot weiss at ieee dot org>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
* Neither the name of ETHZ-ASL nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETHZ-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "pose_sensor.h"
#include <ssf_core/eigen_utils.h>

#define N_MEAS 7 // measurement size
PoseSensorHandler::PoseSensorHandler(ssf_core::Measurements* meas) :
  MeasurementHandler(meas)
{
  ros::NodeHandle pnh("~");
  pnh.param("measurement_world_sensor", measurement_world_sensor_, true);
  pnh.param("use_fixed_covariance", use_fixed_covariance_, false);

  ROS_INFO_COND(measurement_world_sensor_, "interpreting measurement as sensor w.r.t. world");
  ROS_INFO_COND(!measurement_world_sensor_, "interpreting measurement as world w.r.t. sensor (e.g. ethzasl_ptam)");

  ROS_INFO_COND(use_fixed_covariance_, "using fixed covariance");
  ROS_INFO_COND(!use_fixed_covariance_, "using covariance from sensor");

  subscribe();
}

void PoseSensorHandler::subscribe()
{
  ros::NodeHandle nh("ssf_core");
  subMeasurement_ = nh.subscribe("pose_measurement", 1, &PoseSensorHandler::measurementCallback, this);
  
  hector_groundtruth_subMeasurement_ = nh.subscribe("hector_groundtruth_pose_measurement",1,&PoseSensorHandler::hector_groundtruth_measurementCallback,this);
  

  measurements->ssf_core_.registerCallback(&PoseSensorHandler::noiseConfig, this);

  nh.param("meas_noise1", n_zp_, 9.9);	// default position noise is for ethzasl_ptam
  nh.param("meas_noise2", n_zq_, 0.02);	// default attitude noise is for ethzasl_ptam
}

void PoseSensorHandler::noiseConfig(ssf_core::SSF_CoreConfig& config, uint32_t level)
{
  //	if(level & ssf_core::SSF_Core_MISC)
  //	{
  this->n_zp_ = config.meas_noise1;
  this->n_zq_ = config.meas_noise2;
  //	}
}
void PoseSensorHandler::hector_groundtruth_measurementCallback(const geometry_msgs::PoseStampedConstPtr& msg)
{
  ssf_core::State state_old;
  ros::Time time_old = msg->header.stamp;
  Eigen::Matrix<double, N_MEAS, N_STATE> H_old; //测量矩阵
  Eigen::Matrix<double, N_MEAS, 1> r_old; //余差
  Eigen::Matrix<double, N_MEAS, N_MEAS> R; //测量噪音协方差阵

  H_old.setZero();
  R.setZero();

  // get measurements
  z_p_ = Eigen::Matrix<double, 3, 1>(msg->pose.position.x, msg->pose.position.y, msg->pose.position.z);
  z_q_ = Eigen::Quaternion<double>(msg->pose.orientation.w, msg->pose.orientation.x, msg->pose.orientation.y, msg->pose.orientation.z);

  // take covariance from sensor
  R.block<6, 6> (0, 0) = Eigen::Matrix<double, 6, 6>::Identity();//输入msg没有协方差，在未给定的情况下都设为1
  //clear cross-correlations between q and p 
  //注意这里的姿态协方差是用欧拉角表示的，下述是为了去除角度与位置的互相关
  R.block<3, 3> (0, 3) = Eigen::Matrix<double, 3, 3>::Zero();
  R.block<3, 3> (3, 0) = Eigen::Matrix<double, 3, 3>::Zero();
  R(6, 6) = 1e-6; // q_vw yaw-measurement noise
  
  if (use_fixed_covariance_)
  {
    const double s_zp = n_zp_ * n_zp_;
    const double s_zq = n_zq_ * n_zq_;
    R = (Eigen::Matrix<double, N_MEAS, 1>() << s_zp, s_zp, s_zp, s_zq, s_zq, s_zq, 1e-6).finished().asDiagonal();//表示对角阵
  }

  // feedback for init case
  measurements->p_vc_ = z_p_;
  measurements->q_cv_ = z_q_;
  //得到拿到测量数据这个时间点的状态量，idx为存储在缓存中与测量时间点最接近的状态时间点
  unsigned char idx = measurements->ssf_core_.getClosestState(&state_old, time_old);
  //-1表示没有足够的预测部分，导致测量与预测时间间隔相差太远
  if (state_old.time_ == -1)
    return; // // early abort // //

  // get rotation matrices
  Eigen::Matrix<double, 3, 3> C_wv = state_old.q_wv_.conjugate().toRotationMatrix(); //q_wv_ w相对于v, C_wv vision frame 相对于 world frame
  Eigen::Matrix<double, 3, 3> C_q = state_old.q_.conjugate().toRotationMatrix(); //q imu 相对于 world, C_q world相对于imu
  Eigen::Matrix<double, 3, 3> C_ci = state_old.q_ci_.conjugate().toRotationMatrix();//q_ci_ camera相对于imu; C_ci imu 相对于 camera

  // preprocess for elements in H matrix
  Eigen::Matrix<double, 3, 1> vecold;
  vecold = (state_old.p_ + C_q.transpose() * state_old.p_ci_) * state_old.L_;
  Eigen::Matrix<double, 3, 3> skewold = skew(vecold);

  Eigen::Matrix<double, 3, 3> pci_sk = skew(state_old.p_ci_);

  // construct H matrix using H-blockx :-)
  // position:
  H_old.block<3, 3> (0, 0) = C_wv.transpose() * state_old.L_; // p
  H_old.block<3, 3> (0, 6) = -C_wv.transpose() * C_q.transpose() * pci_sk * state_old.L_; // q
  H_old.block<3, 1> (0, 15) = C_wv.transpose() * C_q.transpose() * state_old.p_ci_ + C_wv.transpose() * state_old.p_; // L
  H_old.block<3, 3> (0, 16) = -C_wv.transpose() * skewold; // q_wv
  H_old.block<3, 3> (0, 22) = C_wv.transpose() * C_q.transpose() * state_old.L_; //p_ci

  // attitude
  H_old.block<3, 3> (3, 6) = C_ci; // q
  H_old.block<3, 3> (3, 16) = C_ci * C_q; // q_wv
  H_old.block<3, 3> (3, 19) = Eigen::Matrix<double, 3, 3>::Identity(); //q_ci
  H_old(6, 18) = 1.0; // fix vision world yaw drift because unobservable otherwise (see PhD Thesis)

  // construct residuals
  // position
  //求余差，delta(zp) = 测量值 - 预测值 
  r_old.block<3, 1> (0, 0) = z_p_ - C_wv.transpose() * (state_old.p_ + C_q.transpose() * state_old.p_ci_) * state_old.L_;
  // attitude
  Eigen::Quaternion<double> q_err;
  //求余差，注意论文与代码的不同，因为q^-1 = q*/|q|^2 (q*表示共轭)，当q为单位四元数的情况下，四元数的共轭与求逆相等
  q_err = (state_old.q_wv_ * state_old.q_ * state_old.q_ci_).conjugate() * z_q_;
  r_old.block<3, 1> (3, 0) = q_err.vec() / q_err.w() * 2;//微小四元数转欧拉角
  // vision world yaw drift
  q_err = state_old.q_wv_;
  //四元数转yaw角公式，为什么多了一个负号？
  r_old(6, 0) = -2 * (q_err.w() * q_err.z() + q_err.x() * q_err.y()) / (1 - 2 * (q_err.y() * q_err.y() + q_err.z() * q_err.z()));

  // call update step in core class
  measurements->ssf_core_.applyMeasurement(idx, H_old, r_old, R);
}


void PoseSensorHandler::measurementCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr & msg)
{
  	//ROS_INFO_STREAM("measurement received \n"
  	//				<< "type is: " << typeid(msg).name());

  // init variables，N_MEAS 7 表示测量量
  ssf_core::State state_old;
  ros::Time time_old = msg->header.stamp;
  Eigen::Matrix<double, N_MEAS, N_STATE> H_old; //测量矩阵
  Eigen::Matrix<double, N_MEAS, 1> r_old; //余差
  Eigen::Matrix<double, N_MEAS, N_MEAS> R; //测量噪音协方差阵

  H_old.setZero();
  R.setZero();

  // get measurements
  z_p_ = Eigen::Matrix<double, 3, 1>(msg->pose.pose.position.x, msg->pose.pose.position.y, msg->pose.pose.position.z);
  z_q_ = Eigen::Quaternion<double>(msg->pose.pose.orientation.w, msg->pose.pose.orientation.x, msg->pose.pose.orientation.y, msg->pose.pose.orientation.z);

  // take covariance from sensor
  R.block<6, 6> (0, 0) = Eigen::Matrix<double, 6, 6>(&msg->pose.covariance[0]);
  //clear cross-correlations between q and p 
  //注意这里的姿态协方差是用欧拉角表示的，下述是为了去除角度与位置的互相关
  R.block<3, 3> (0, 3) = Eigen::Matrix<double, 3, 3>::Zero();
  R.block<3, 3> (3, 0) = Eigen::Matrix<double, 3, 3>::Zero();
  R(6, 6) = 1e-6; // q_vw yaw-measurement noise

  /*************************************************************************************/
  // use this if your pose sensor is ethzasl_ptam (www.ros.org/wiki/ethzasl_ptam)
  // ethzasl_ptam publishes the camera pose as the world seen from the camera
  if (!measurement_world_sensor_)
  {
    Eigen::Matrix<double, 3, 3> C_zq = z_q_.toRotationMatrix();
    z_q_ = z_q_.conjugate();
    z_p_ = -C_zq.transpose() * z_p_;

    Eigen::Matrix<double, 6, 6> C_cov(Eigen::Matrix<double, 6, 6>::Zero());
    C_cov.block<3, 3> (0, 0) = C_zq;
    C_cov.block<3, 3> (3, 3) = C_zq;

    R.block<6, 6> (0, 0) = C_cov.transpose() * R.block<6, 6> (0, 0) * C_cov;
  }
  /*************************************************************************************/

  //  alternatively take fix covariance from reconfigure GUI
  if (use_fixed_covariance_)
  {
    const double s_zp = n_zp_ * n_zp_;
    const double s_zq = n_zq_ * n_zq_;
    R = (Eigen::Matrix<double, N_MEAS, 1>() << s_zp, s_zp, s_zp, s_zq, s_zq, s_zq, 1e-6).finished().asDiagonal();//表示对角阵
  }

  // feedback for init case
  measurements->p_vc_ = z_p_;
  measurements->q_cv_ = z_q_;
  //得到拿到测量数据这个时间点的状态量，idx为存储在缓存中与测量时间点最接近的状态时间点
  unsigned char idx = measurements->ssf_core_.getClosestState(&state_old, time_old);
  //-1表示没有足够的预测部分，导致测量与预测时间间隔相差太远
  if (state_old.time_ == -1)
    return; // // early abort // //

  // get rotation matrices
  Eigen::Matrix<double, 3, 3> C_wv = state_old.q_wv_.conjugate().toRotationMatrix(); //q_wv_ w相对于v, C_wv vision frame 相对于 world frame
  Eigen::Matrix<double, 3, 3> C_q = state_old.q_.conjugate().toRotationMatrix(); //q imu 相对于 world, C_q world相对于imu
  Eigen::Matrix<double, 3, 3> C_ci = state_old.q_ci_.conjugate().toRotationMatrix();//q_ci_ camera相对于imu; C_ci imu 相对于 camera

  // preprocess for elements in H matrix
  Eigen::Matrix<double, 3, 1> vecold;
  vecold = (state_old.p_ + C_q.transpose() * state_old.p_ci_) * state_old.L_;
  Eigen::Matrix<double, 3, 3> skewold = skew(vecold);

  Eigen::Matrix<double, 3, 3> pci_sk = skew(state_old.p_ci_);

  // construct H matrix using H-blockx :-)
  // position:
  H_old.block<3, 3> (0, 0) = C_wv.transpose() * state_old.L_; // p
  H_old.block<3, 3> (0, 6) = -C_wv.transpose() * C_q.transpose() * pci_sk * state_old.L_; // q
  H_old.block<3, 1> (0, 15) = C_wv.transpose() * C_q.transpose() * state_old.p_ci_ + C_wv.transpose() * state_old.p_; // L
  H_old.block<3, 3> (0, 16) = -C_wv.transpose() * skewold; // q_wv
  H_old.block<3, 3> (0, 22) = C_wv.transpose() * C_q.transpose() * state_old.L_; //p_ci

  // attitude
  H_old.block<3, 3> (3, 6) = C_ci; // q
  H_old.block<3, 3> (3, 16) = C_ci * C_q; // q_wv
  H_old.block<3, 3> (3, 19) = Eigen::Matrix<double, 3, 3>::Identity(); //q_ci
  H_old(6, 18) = 1.0; // fix vision world yaw drift because unobservable otherwise (see PhD Thesis)

  // construct residuals
  // position
  //求余差，delta(zp) = 测量值 - 预测值 
  r_old.block<3, 1> (0, 0) = z_p_ - C_wv.transpose() * (state_old.p_ + C_q.transpose() * state_old.p_ci_) * state_old.L_;
  // attitude
  Eigen::Quaternion<double> q_err;
  //求余差，注意论文与代码的不同，因为q^-1 = q*/|q|^2 (q*表示共轭)，当q为单位四元数的情况下，四元数的共轭与求逆相等
  q_err = (state_old.q_wv_ * state_old.q_ * state_old.q_ci_).conjugate() * z_q_;
  r_old.block<3, 1> (3, 0) = q_err.vec() / q_err.w() * 2;//微小四元数转欧拉角
  // vision world yaw drift
  q_err = state_old.q_wv_;
  //四元数转yaw角公式，为什么多了一个负号？
  r_old(6, 0) = -2 * (q_err.w() * q_err.z() + q_err.x() * q_err.y()) / (1 - 2 * (q_err.y() * q_err.y() + q_err.z() * q_err.z()));

  // call update step in core class
  measurements->ssf_core_.applyMeasurement(idx, H_old, r_old, R);
}