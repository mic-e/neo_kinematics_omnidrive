/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016, Neobotix GmbH
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Neobotix nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#ifndef INCLUDE_OMNI_KINEMATICS_H_
#define INCLUDE_OMNI_KINEMATICS_H_

#include "OmniWheel.h"

#include <angles/angles.h>

#include <vector>
#include <stdexcept>
#include <math.h>


/*
 * Computes desired wheel steering angles and velocities based on commanded
 * platform velocity and yawrate.
 */
class OmniKinematics {
public:
	double zero_vel_threshold = 0.01;		// [m/s]
	double steering_hysteresis = 0.1;		// [rad]
	bool home_on_stop = false;

	OmniKinematics(int num_wheels_)
		:	num_wheels(num_wheels_)
	{
		is_driving.resize(num_wheels_);
		is_alternate.resize(num_wheels_);
	}

	/*
	* Computes desired wheel steering angles and velocities based on commanded
	* platform velocity and yawrate.
	*
	* @return Vector of OmniWheels with wheel_angle and wheel_vel set to desired values.
	*/
	std::vector<OmniWheel> compute(const std::vector<OmniWheel>& wheels, double move_vel_x, double move_vel_y, double move_yawrate)
	{
		if(wheels.size() != num_wheels) {
			throw std::logic_error("wheels.size() != num_wheels");
		}
		std::vector<OmniWheel> result;

		// if no move command given set velocity to zero and optionally steer to home
		if(move_vel_x == 0 && move_vel_y == 0 && move_yawrate == 0)
		{
			for(int i = 0; i < num_wheels; ++i)
			{
				OmniWheel new_wheel = wheels[i];
				if(home_on_stop) {
					new_wheel.wheel_angle = wheels[i].home_angle;
				}
				new_wheel.wheel_vel = 0;
				result.push_back(new_wheel);
			}
			return result;
		}

		for(int i = 0; i < num_wheels; ++i)
		{
			const OmniWheel& wheel = wheels[i];
			const double wheel_pos_radius = wheel.get_wheel_pos_radius();		// wheel position in polar coords [m]
			const double wheel_pos_angle = wheel.get_wheel_pos_angle();			// wheel position in polar coords [rad]
			const double tangential = wheel_pos_radius * move_yawrate;					// tangential velocity
			const double vel_x = move_vel_x + tangential * -sin(wheel_pos_angle);		// tangential is 90 deg rotated (ie. in y direction at phi=0)
			const double vel_y = move_vel_y + tangential * cos(wheel_pos_angle);

			// convert desired x + y velocity to steering angle and drive velocity
			double new_wheel_angle = ::atan2(vel_y, vel_x);
			double new_wheel_vel = ::hypot(vel_x, vel_y);

			// check if wheel is currently driving or not
			if(fabs(wheel.wheel_vel) > (is_driving[i] ? zero_vel_threshold : 2 * zero_vel_threshold))
			{
				// if wheel is driving choose the closest solution in terms of velocity direction
				if(new_wheel_vel * wheel.wheel_vel < 0)
				{
					new_wheel_angle = angles::normalize_angle(new_wheel_angle + M_PI);
					new_wheel_vel = -1 * new_wheel_vel;
					is_alternate[i] = true;
				} else {
					is_alternate[i] = false;
				}
				is_driving[i] = true;
			}
			else
			{
				// compute outer steering angle
				const double center_pos_angle = ::atan2(wheel.center_pos_y, wheel.center_pos_x);
				const double outer_wheel_angle = angles::normalize_angle(center_pos_angle - M_PI / 2);

				// if wheel is not driving choose the solution which is closer to outer wheel angle
				if(fabs(angles::shortest_angular_distance(new_wheel_angle, outer_wheel_angle))
						> M_PI / 2 + (is_alternate[i] ? -1 : 1) * steering_hysteresis)
				{
					new_wheel_angle = angles::normalize_angle(new_wheel_angle + M_PI);
					new_wheel_vel = -1 * new_wheel_vel;
					is_alternate[i] = true;
				} else {
					is_alternate[i] = false;
				}
				is_driving[i] = false;
			}

			// store new values
			OmniWheel new_wheel = wheel;
			new_wheel.set_wheel_angle(new_wheel_angle);
			new_wheel.wheel_vel = new_wheel_vel;
			result.push_back(new_wheel);
		}
		return result;
	}

private:
	int num_wheels = 0;

	std::vector<bool> is_driving;
	std::vector<bool> is_alternate;

};


#endif // INCLUDE_OMNI_KINEMATICS_H_