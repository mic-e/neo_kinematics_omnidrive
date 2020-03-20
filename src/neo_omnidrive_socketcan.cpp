/*
 * neo_omnidrive_socketcan.cpp
 *
 *  Created on: Mar 19, 2020
 *      Author: mad
 */

#include <ros/ros.h>
#include <angles/angles.h>
#include <sensor_msgs/JointState.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <neo_msgs/EmergencyStopState.h>

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>


class NeoSocketCanNode {
public:
	enum motor_state_e
	{
		ST_PRE_INITIALIZED,
		ST_OPERATION_ENABLED,
		ST_OPERATION_DISABLED,
		ST_MOTOR_FAILURE
	};

	struct motor_t
	{
		std::string joint_name;					// ROS joint name
		int32_t can_id = -1;					// motor "CAN ID"
		int32_t rot_sign = 0;					// motor rotation direction
		int32_t enc_ticks_per_rev = 0;			// encoder ticks per motor revolution
		int32_t enc_home_offset = 0;			// encoder offset for true home position
		int32_t max_vel_enc_s = 1000000;		// max motor velocity in ticks/s (positive)
		int32_t max_accel_enc_s = 1000000;		// max motor acceleration in ticks/s^2 (positive)
		int32_t can_Tx_PDO1 = -1;
		int32_t can_Tx_PDO2 = -1;
		int32_t can_Rx_PDO2 = -1;
		int32_t can_Tx_SDO = -1;
		int32_t can_Rx_SDO = -1;
		double gear_ratio = 0;					// gear ratio

		motor_state_e state = ST_PRE_INITIALIZED;
		int32_t curr_enc_pos_inc = 0;			// current encoder position value in ticks
		int32_t curr_enc_vel_inc_s = 0;			// current encoder velocity value in ticks/s
		int32_t curr_status = 0;				// current status as received by SR msg
		int32_t curr_motor_failure = 0;			// current motor failure status as received by MF msg
		ros::Time request_send_time;			// time of last status update request
		ros::Time status_recv_time;				// time of last status update received
		ros::Time last_update_time;				// time of last sync update received
		int homing_state = -1;					// current homing state (-1 = unknown, 0 = active, 1 = finished)
	};

	struct module_t
	{
		motor_t drive;
		motor_t steer;

		int32_t home_dig_in = 0;				// digital input for homing switch
		double home_angle = 0;					// home steering angle in rad

		double curr_wheel_pos = 0;				// current wheel angle in rad
		double curr_wheel_vel = 0;				// current wheel velocity in rad/s
		double curr_steer_pos = 0;				// current steering angle in rad
		double curr_steer_vel = 0;				// current steering velocity in rad
	};

	struct can_msg_t
	{
		int id = -1;
		int length = 0;
		char data[8] = {};
	};

	NeoSocketCanNode()
	{
		if(!m_node_handle.getParam("num_wheels", m_num_wheels)) {
			throw std::logic_error("missing num_wheels param");
		}
		if(!m_node_handle.getParam("can_iface", m_can_iface)) {
			throw std::logic_error("missing can_iface param");
		}
		m_node_handle.param("motor_timeout", m_motor_timeout, 1.);
		m_node_handle.param("home_vel", m_home_vel, -1.);

		if(m_num_wheels < 1) {
			throw std::logic_error("invalid num_wheels param");
		}
		m_wheels.resize(m_num_wheels);

		for(int i = 0; i < m_num_wheels; ++i)
		{
			if(!m_node_handle.getParam("drive" + std::to_string(i) + "/can_id", m_wheels[i].drive.can_id)) {
				throw std::logic_error("can_id param missing for drive motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/can_id", m_wheels[i].steer.can_id)) {
				throw std::logic_error("can_id param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("drive" + std::to_string(i) + "/joint_name", m_wheels[i].drive.joint_name)) {
				throw std::logic_error("joint_name param missing for drive motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/joint_name", m_wheels[i].steer.joint_name)) {
				throw std::logic_error("joint_name param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("drive" + std::to_string(i) + "/rot_sign", m_wheels[i].drive.rot_sign)) {
				throw std::logic_error("rot_sign param missing for drive motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/rot_sign", m_wheels[i].steer.rot_sign)) {
				throw std::logic_error("rot_sign param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("drive" + std::to_string(i) + "/gear_ratio", m_wheels[i].drive.gear_ratio)) {
				throw std::logic_error("gear_ratio param missing for drive motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/gear_ratio", m_wheels[i].steer.gear_ratio)) {
				throw std::logic_error("gear_ratio param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("drive" + std::to_string(i) + "/enc_ticks_per_rev", m_wheels[i].drive.enc_ticks_per_rev)) {
				throw std::logic_error("enc_ticks_per_rev param missing for drive motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/enc_ticks_per_rev", m_wheels[i].steer.enc_ticks_per_rev)) {
				throw std::logic_error("enc_ticks_per_rev param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/home_angle", m_wheels[i].home_angle)) {
				throw std::logic_error("home_angle param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/home_dig_in", m_wheels[i].home_dig_in)) {
				throw std::logic_error("home_dig_in param missing for steering motor" + std::to_string(i));
			}
			if(!m_node_handle.getParam("steer" + std::to_string(i) + "/enc_home_offset", m_wheels[i].steer.enc_home_offset)) {
				throw std::logic_error("enc_home_offset param missing for steering motor" + std::to_string(i));
			}
		}

		m_pub_joint_state = m_node_handle.advertise<sensor_msgs::JointState>("drives/joint_states", 1);

		m_sub_joint_trajectory = m_node_handle.subscribe("drives/joint_trajectory", 1, &NeoSocketCanNode::joint_trajectory_callback, this);
		m_sub_emergency_stop = m_node_handle.subscribe("emergency_stop_state", 1, &NeoSocketCanNode::emergency_stop_callback, this);

		m_can_thread = std::thread(&NeoSocketCanNode::receive_loop, this);
	}

	void update()
	{
		std::lock_guard<std::mutex> lock(m_node_mutex);

		const ros::Time now = ros::Time::now();

		// check for motor timeouts
		for(auto& wheel : m_wheels)
		{
			check_motor_timeout(wheel.drive, now);
			check_motor_timeout(wheel.steer, now);
		}

		// check if we should stop motion
		if(!all_motors_operational())
		{
			stop_motion();
		}

		// check for motor reset done
		if(is_motor_reset)
		{
			if(all_motors_operational())
			{
				ROS_INFO_STREAM("All motors operational!");
				is_motor_reset = false;
			}
		}

		// check if we should start homing
		if(!is_all_homed && !is_homing_active && all_motors_operational())
		{
			ROS_INFO_STREAM("Start homing procedure ...");
			start_homing();
		}

		// check if homing done
		if(is_homing_active)
		{
			if(!all_motors_operational())
			{
				ROS_ERROR_STREAM("Homing has been interrupted!");
				is_homing_active = false;
			}
			else if(check_homing_done())
			{
				finish_homing();
				ROS_INFO_STREAM("Homing successful!");
			}
			else
			{
				// send status request
				for(auto& wheel : m_wheels)
				{
					canopen_query(wheel.steer, 'H', 'M', 1);
				}
				can_sync();
			}
		}

		// check if we should reset steering
		if(is_steer_reset_active)
		{
			if(all_motors_operational())
			{
				bool is_all_reached = true;

				for(auto& wheel : m_wheels)
				{
					if(fabs(angles::normalize_angle(wheel.curr_steer_pos)) > 0.01)
					{
						is_all_reached = false;
						motor_set_pos_abs(wheel.steer, 0);
					}
				}

				if(is_all_reached)
				{
					ROS_INFO_STREAM("Steering reset successful!");
					is_steer_reset_active = false;
				}
				else {
					begin_motion();
				}
			}
		}

		// check for update timeout
		if(m_last_update_time < m_last_sync_time)
		{
			ROS_WARN_STREAM("Sync update timeout!");
		}

		// request current motor values
		{
			can_msg_t msg;
			msg.id  = 0x80;
			msg.length = 0;
			can_transmit(msg);
		}
		can_sync();

		m_last_sync_time = ros::Time::now();
		m_sync_counter++;

		if(m_sync_counter % 10 == 0)
		{
			request_status_all();		// request status update
		}
	}

	void initialize()
	{
		std::lock_guard<std::mutex> lock(m_node_mutex);

		// wait for CAN socket to be available
		m_wait_for_can_sock = true;

		// reset states
		for(auto& wheel : m_wheels)
		{
			wheel.drive.state = ST_PRE_INITIALIZED;
			wheel.steer.state = ST_PRE_INITIALIZED;
		}
		is_all_homed = false;
		is_homing_active = false;
		is_steer_reset_active = false;

		// start network
		{
			can_msg_t msg;
			msg.id = 0;
			msg.length = 2;
			msg.data[0] = 1;
			msg.data[1] = 0;
			can_transmit(msg);
		}
		can_sync();

		::usleep(100 * 1000);

		all_motors_off();

		stop_motion();

		// set modulo to one wheel revolution (to preserve absolute position for homed motors)
		for(auto& wheel : m_wheels)
		{
			set_motor_modulo(wheel.drive, 1);
			set_motor_modulo(wheel.steer, 1);
		}
		can_sync();

		// set motion control to velocity mode first
		for(auto& wheel : m_wheels)
		{
			set_motion_vel_ctrl(wheel.drive);
			set_motion_vel_ctrl(wheel.steer);
		}
		can_sync();

		// set position counter to zero
		for(auto& wheel : m_wheels)
		{
			reset_pos_counter(wheel.drive);
			reset_pos_counter(wheel.steer);
		}
		can_sync();

		// ---------- set PDO mapping
		// Mapping of TPDO1:
		// - position
		// - velocity
		for(auto& wheel : m_wheels)
		{
			configure_PDO_mapping(wheel.drive);
			configure_PDO_mapping(wheel.steer);
		}
		can_sync();

		all_motors_on();

		request_status_all();
	}

	void shutdown()
	{
		std::lock_guard<std::mutex> lock(m_node_mutex);

		// disable waiting for CAN socket, since we are shutting down
		m_wait_for_can_sock = false;

		try {
			stop_motion();
			all_motors_off();
			can_sync();
		}
		catch(...) {
			// ignore
		}

		do_run = false;
		{
			std::lock_guard<std::mutex> lock(m_can_mutex);
			if(m_can_sock >= 0) {
				::shutdown(m_can_sock, SHUT_RDWR);		// trigger receive thread to exit
			}
		}
		if(m_can_thread.joinable()) {
			m_can_thread.join();
		}
	}

private:
	void joint_trajectory_callback(const trajectory_msgs::JointTrajectory& joint_trajectory)
	{
		std::lock_guard<std::mutex> lock(m_node_mutex);

		// check if we are ready for normal operation
		if(!is_all_homed || is_steer_reset_active)
		{
			return;
		}

		// check if we are fully operational
		if(!all_motors_operational()) {
			return;
		}

		// subtract home angle from commanded angle

		// TODO
	}

	void emergency_stop_callback(const neo_msgs::EmergencyStopState::ConstPtr& state)
	{
		std::lock_guard<std::mutex> lock(m_node_mutex);

		if(is_em_stop && state->emergency_state == neo_msgs::EmergencyStopState::EMFREE)
		{
			ROS_INFO_STREAM("Reactivating motors ...");

			// reset states
			for(auto& wheel : m_wheels)
			{
				wheel.drive.state = ST_PRE_INITIALIZED;
				wheel.steer.state = ST_PRE_INITIALIZED;
			}
			is_motor_reset = true;

			all_motors_on();			// re-activate the motors

			request_status_all();		// request new status
		}

		is_em_stop = state->emergency_state != neo_msgs::EmergencyStopState::EMFREE;
	}

	void check_motor_timeout(motor_t& motor, ros::Time now)
	{
		if(motor.status_recv_time < motor.request_send_time
			&& (now - motor.request_send_time).toSec() > m_motor_timeout)
		{
			if(motor.state != ST_MOTOR_FAILURE) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor status timeout!");
			}
			motor.state = ST_MOTOR_FAILURE;
		}
	}

	bool all_motors_operational() const
	{
		for(auto& wheel : m_wheels)
		{
			if(wheel.drive.state != ST_OPERATION_ENABLED || wheel.steer.state != ST_OPERATION_ENABLED) {
				return false;
			}
		}
		return !is_em_stop;
	}

	void start_homing()
	{
		if(!all_motors_operational()) {
			return;
		}

		stop_motion();

		for(auto& wheel : m_wheels)
		{
			// disarm homing
			canopen_set_int(wheel.steer, 'H', 'M', 1, 0);
			can_sync();

			// configure homing sequences
			// setting the value such that increment counter resets after the homing event occurs
			canopen_set_int(wheel.steer, 'H', 'M', 2, wheel.steer.enc_home_offset);
			can_sync();

			// choosing channel/switch on which controller has to listen for change of homing event(high/low/falling/rising)
			canopen_set_int(wheel.steer, 'H', 'M', 3, wheel.home_dig_in);
			can_sync();

			// choose the action that the controller shall perform after the homing event occurred
			// HM[4] = 0 : after Event stop immediately
			// HM[4] = 2 : do nothing
			canopen_set_int(wheel.steer, 'H', 'M', 4, 0);
			can_sync();

			// choose the setting of the position counter (i.e. to the value defined in 2.a) after the homing event occured
			// HM[5] = 0 : absolute setting of position counter: PX = HM[2]
			canopen_set_int(wheel.steer, 'H', 'M', 5, 0);
			can_sync();
		}

		// start turning motors
		for(auto& wheel : m_wheels)
		{
			motor_set_vel(wheel.drive, 0);
			motor_set_vel(wheel.steer, m_home_vel);
		}
		can_sync();

		begin_motion();

		::usleep(500 * 1000);		// TODO: why we need this?

		// arm homing
		for(auto& wheel : m_wheels)
		{
			canopen_set_int(wheel.steer, 'H', 'M', 1, 1);

			wheel.steer.homing_state = -1;		// reset switch state
		}
		can_sync();

		is_homing_active = true;
	}

	bool check_homing_done() const
	{
		for(auto& wheel : m_wheels)
		{
			if(wheel.steer.homing_state != 1) {
				return false;
			}
		}
		return true;
	}

	void finish_homing()
	{
		stop_motion();

		all_motors_off();

		// switch to position mode for steering
		for(auto& wheel : m_wheels)
		{
			set_motion_pos_ctrl(wheel.steer);
		}
		can_sync();

		all_motors_on();

		is_all_homed = true;
		is_homing_active = false;
		is_steer_reset_active = true;
	}

	void set_motor_can_id(motor_t& motor, int id)
	{
		motor.can_id = id;
		motor.can_Tx_PDO1 = id + 0x180;
		// motor.can_Rx_PDO1 = id + 0x200;
		motor.can_Tx_PDO2 = id + 0x280;
		motor.can_Rx_PDO2 = id + 0x300;
		motor.can_Tx_SDO = id + 0x580;
		motor.can_Rx_SDO = id + 0x600;
	}

	void configure_PDO_mapping(const motor_t& motor)
	{
		// stop all emissions of TPDO1
		canopen_SDO_download(motor, 0x1A00, 0, 0);

		// position 4 byte of TPDO1
		canopen_SDO_download(motor, 0x1A00, 1, 0x60640020);

		// velocity 4 byte of TPDO1
		canopen_SDO_download(motor, 0x1A00, 2, 0x60690020);

		// transmission type "synch"
		canopen_SDO_download(motor, 0x1800, 2, 1);

		// activate mapped objects
		canopen_SDO_download(motor, 0x1A00, 0, 2);

		can_sync();
	}

	void set_motor_modulo(const motor_t& motor, int32_t num_wheel_rev)
	{
		const int32_t ticks_per_rev = motor.enc_ticks_per_rev * motor.gear_ratio;
		canopen_set_int(motor, 'X', 'M', 1, -1 * num_wheel_rev * ticks_per_rev);
		canopen_set_int(motor, 'X', 'M', 2, num_wheel_rev * ticks_per_rev);

		can_sync();
	}

	void reset_pos_counter(const motor_t& motor)
	{
		canopen_set_int(motor, 'P', 'X', 0, 0);
	}

	void request_status(motor_t& motor)
	{
		canopen_query(motor, 'S', 'R', 0);
		motor.request_send_time = ros::Time::now();
	}

	void request_status_all()
	{
		for(auto& wheel : m_wheels)
		{
			request_status(wheel.drive);
			request_status(wheel.steer);
		}
		can_sync();
	}

	void motor_on(const motor_t& motor)
	{
		canopen_set_int(motor, 'M', 'O', 0, 1);
	}

	void motor_off(const motor_t& motor)
	{
		canopen_set_int(motor, 'M', 'O', 0, 0);
	}

	void all_motors_on()
	{
		for(auto& wheel : m_wheels)
		{
			motor_on(wheel.drive);
			motor_on(wheel.steer);
		}
		can_sync();
	}

	void all_motors_off()
	{
		for(auto& wheel : m_wheels)
		{
			motor_off(wheel.drive);
			motor_off(wheel.steer);
		}
		can_sync();

		is_motor_reset = true;
	}

	void set_motion_vel_ctrl(const motor_t& motor)
	{
		// switch Unit Mode
		canopen_set_int(motor, 'U', 'M', 0, 2);

		// set profile mode (only if Unit Mode = 2)
		canopen_set_int(motor, 'P', 'M', 0, 1);

		// set maximum acceleration to X Incr/s^2
		canopen_set_int(motor, 'A', 'C', 0, motor.max_accel_enc_s);

		// set maximum decceleration to X Incr/s^2
		canopen_set_int(motor, 'D', 'C', 0, motor.max_accel_enc_s);

		can_sync();
	}

	void set_motion_pos_ctrl(const motor_t& motor)
	{
		// switch Unit Mode
		canopen_set_int(motor, 'U', 'M', 0, 5);

		// set Target Radius to X Increments
		canopen_set_int(motor, 'T', 'R', 1, 15);		// TODO: add ROS param

		// set Target Time to X ms
		canopen_set_int(motor, 'T', 'R', 2, 100);		// TODO: add ROS param

		// set maximum acceleration to X Incr/s^2
		canopen_set_int(motor, 'A', 'C', 0, motor.max_accel_enc_s);

		// set maximum decceleration to X Incr/s^2
		canopen_set_int(motor, 'D', 'C', 0, motor.max_accel_enc_s);

		can_sync();
	}

	void begin_motion()
	{
		for(const auto& wheel : m_wheels)
		{
			canopen_query(wheel.drive, 'B', 'G', 0);
			canopen_query(wheel.steer, 'B', 'G', 0);
		}
		can_sync();
	}

	void stop_motion()
	{
		for(const auto& wheel : m_wheels)
		{
			canopen_query(wheel.drive, 'S', 'T', 0);
			canopen_query(wheel.steer, 'S', 'T', 0);
		}
		can_sync();
	}

	void motor_set_vel(const motor_t& motor, double rot_vel_rad_s)
	{
		const double motor_vel_rev_s = motor.gear_ratio * rot_vel_rad_s / (2 * M_PI);
		const int32_t motor_vel_inc_s = motor.rot_sign * int(motor_vel_rev_s * motor.enc_ticks_per_rev);
		const int32_t lim_motor_vel_inc_s = std::min(std::max(motor_vel_inc_s, -motor.max_vel_enc_s), motor.max_vel_enc_s);

		canopen_set_int(motor, 'J', 'V', 0, lim_motor_vel_inc_s);
	}

	void motor_set_pos_abs(const motor_t& motor, double angle_rad)
	{
		const double motor_pos_rev = motor.gear_ratio * angle_rad / (2 * M_PI);
		const int32_t motor_pos_inc = motor.rot_sign * int(motor_pos_rev * motor.enc_ticks_per_rev);

		canopen_set_int(motor, 'P', 'A', 0, motor_pos_inc);
	}

	void canopen_query(const motor_t& motor, char cmd_char_1, char cmd_char_2, int32_t index)
	{
		can_msg_t msg;
		msg.id = motor.can_Rx_PDO2;
		msg.length = 4;
		msg.data[0] = cmd_char_1;
		msg.data[1] = cmd_char_2;
		msg.data[2] = index;
		msg.data[3] = (index >> 8) & 0x3F;  // The two MSB must be 0. Cf. DSP 301 Implementation guide p. 39.
		can_transmit(msg);
	}

	void canopen_set_int(const motor_t& motor, char cmd_char_1, char cmd_char_2, int32_t index, int32_t data)
	{
		can_msg_t msg;
		msg.id = motor.can_Rx_PDO2;
		msg.length = 8;
		msg.data[0] = cmd_char_1;
		msg.data[1] = cmd_char_2;
		msg.data[2] = index;
		msg.data[3] = (index >> 8) & 0x3F;  // The two MSB must be 0. Cf. DSP 301 Implementation guide p. 39.
		msg.data[4] = data;
		msg.data[5] = data >> 8;
		msg.data[6] = data >> 16;
		msg.data[7] = data >> 24;
		can_transmit(msg);
	}

	void canopen_SDO_download(const motor_t& motor, int32_t obj_index, int32_t obj_sub_index, int32_t data)
	{
		const int32_t ciInitDownloadReq = 0x20;
		const int32_t ciNrBytesNoData = 0x00;
		const int32_t ciExpedited = 0x02;
		const int32_t ciDataSizeInd = 0x01;

		can_msg_t msg;
		msg.id = motor.can_Rx_SDO;
		msg.length = 8;
		msg.data[0] = ciInitDownloadReq | (ciNrBytesNoData << 2) | ciExpedited | ciDataSizeInd;
		msg.data[1] = obj_index;
		msg.data[2] = obj_index >> 8;
		msg.data[3] = obj_sub_index;
		msg.data[4] = data;
		msg.data[5] = data >> 8;
		msg.data[6] = data >> 16;
		msg.data[7] = data >> 24;
		can_transmit(msg);
	}

	void can_transmit(const can_msg_t& msg)
	{
		::can_frame out = {};
		out.can_id = msg.id;
		out.can_dlc = msg.length;
		for(int i = 0; i < msg.length; ++i) {
			out.data[i] = msg.data[i];
		}

		// wait for socket to be ready for writing
		if(m_wait_for_can_sock) {
			std::unique_lock<std::mutex> lock(m_can_mutex);
			while(do_run && m_can_sock < 0) {
				m_can_condition.wait(lock);
			}
		}
		if(!do_run) {
			throw std::runtime_error("shutdown");
		}

		// send msg
		{
			const auto res = ::write(m_can_sock, &out, sizeof(out));
			if(res < 0) {
				throw std::runtime_error("write() failed with: " + std::string(strerror(errno)));
			}
			if(res != sizeof(out))
			{
				// re-open socket
				::shutdown(m_can_sock, SHUT_RDWR);
				throw std::logic_error("write() buffer overflow!");
			}
		}
	}

	/*
	 * Waits till all msgs are sent on the bus.
	 */
	void can_sync()
	{
		if(::fsync(m_can_sock) != 0) {
			throw std::runtime_error("fsync() failed with: " + std::string(strerror(errno)));
		}
	}

	/*
	 * Processes incoming CAN msgs.
	 * Called by receive_loop() only!
	 */
	void handle(const can_msg_t& msg)
	{
		size_t num_motor_updates = 0;

		for(auto& wheel : m_wheels)
		{
			if(msg.id == wheel.drive.can_Tx_PDO1) {
				handle_PDO1(wheel.drive, msg);
			}
			if(msg.id == wheel.steer.can_Tx_PDO1) {
				handle_PDO1(wheel.steer, msg);
			}
			if(msg.id == wheel.drive.can_Tx_PDO2) {
				handle_PDO2(wheel.drive, msg);
			}
			if(msg.id == wheel.steer.can_Tx_PDO2) {
				handle_PDO2(wheel.steer, msg);
			}

			// re-compute wheel values
			if(wheel.drive.last_update_time > m_last_sync_time)
			{
				wheel.curr_wheel_pos = calc_wheel_pos(wheel.drive);
				wheel.curr_wheel_vel = calc_wheel_vel(wheel.drive);
				num_motor_updates++;
			}
			if(wheel.steer.last_update_time > m_last_sync_time)
			{
				wheel.curr_steer_pos = calc_wheel_pos(wheel.steer);
				wheel.curr_steer_vel = calc_wheel_vel(wheel.steer);
				num_motor_updates++;
			}
		}

		// check if we have all data for next update
		if(num_motor_updates >= m_wheels.size() * 2 && m_last_update_time < m_last_sync_time)
		{
			const ros::Time now = ros::Time::now();
			publish_joint_states(now);
			m_last_update_time = now;
		}
	}

	void publish_joint_states(ros::Time now)
	{
		sensor_msgs::JointState::Ptr joint_state = boost::make_shared<sensor_msgs::JointState>();
		joint_state->header.stamp = now;

		for(auto& wheel : m_wheels)
		{
			joint_state->name.push_back(wheel.drive.joint_name);
			joint_state->name.push_back(wheel.steer.joint_name);
			joint_state->position.push_back(wheel.curr_wheel_pos);
			joint_state->position.push_back(wheel.curr_steer_pos);
			joint_state->velocity.push_back(wheel.curr_wheel_vel);
			joint_state->velocity.push_back(wheel.curr_steer_vel);
			joint_state->effort.push_back(0);
			joint_state->effort.push_back(0);
		}
		m_pub_joint_state.publish(joint_state);
	}

	double calc_wheel_pos(motor_t& motor) const
	{
		return 2 * M_PI * double(motor.rot_sign * motor.curr_enc_pos_inc)
				/ motor.enc_ticks_per_rev / motor.gear_ratio;
	}

	double calc_wheel_vel(motor_t& motor) const
	{
		return 2 * M_PI * double(motor.rot_sign * motor.curr_enc_vel_inc_s)
				/ motor.enc_ticks_per_rev / motor.gear_ratio;
	}

	int32_t read_int32(const can_msg_t& msg, int offset) const
	{
		if(offset < 0 || offset > 4) {
			throw std::logic_error("invalid offset");
		}
		return (int32_t(msg.data[offset + 3]) << 24) | (int32_t(msg.data[offset + 2]) << 16)
				| (int32_t(msg.data[offset + 1]) << 8) | int32_t(msg.data[offset + 0]);
	}

	void handle_PDO1(motor_t& motor, const can_msg_t& msg)
	{
		motor.curr_enc_pos_inc = read_int32(msg, 0);
		motor.curr_enc_vel_inc_s = read_int32(msg, 4);
		motor.last_update_time = ros::Time::now();
	}

	void handle_PDO2(motor_t& motor, const can_msg_t& msg)
	{
		if(msg.data[0] == 'S' && msg.data[1] == 'R')
		{
			const auto prev_status = motor.curr_status;
			motor.curr_status = read_int32(msg, 4);
			evaluate_status(motor, prev_status);
			motor.status_recv_time = ros::Time::now();
		}
		if(msg.data[0] == 'M' && msg.data[1] == 'F')
		{
			const auto prev_status = motor.curr_motor_failure;
			motor.curr_motor_failure = read_int32(msg, 4);
			evaluate_motor_failure(motor, prev_status);
		}
		if(msg.data[0] == 'H' && msg.data[1] == 'M')
		{
			motor.homing_state = (msg.data[4] == 0 ? 1 : 0);
		}
	}

	void evaluate_status(motor_t& motor, int32_t prev_status)
	{
		if(motor.curr_status & 1)
		{
			if(motor.curr_status != prev_status)
			{
				if((motor.curr_status & 0xE) == 2) {
					ROS_ERROR_STREAM(motor.joint_name << ": drive error under voltage");
				}
				else if((motor.curr_status & 0xE) == 4) {
					ROS_ERROR_STREAM(motor.joint_name << ": drive error over voltage");
				}
				else if((motor.curr_status & 0xE) == 10) {
					ROS_ERROR_STREAM(motor.joint_name << ": drive error short circuit");
				}
				else if((motor.curr_status & 0xE) == 12) {
					ROS_ERROR_STREAM(motor.joint_name << ": drive error over-heating");
				}
				else {
					ROS_ERROR_STREAM(motor.joint_name << ": unknown failure: " << (motor.curr_status & 0xE));
				}
			}

			// request detailed description of failure
			canopen_query(motor, 'M', 'F', 0);

			motor.state = ST_MOTOR_FAILURE;
		}
		else if(motor.curr_status & (1 << 6))
		{
			// general failure
			if(motor.curr_status != prev_status)
			{
				ROS_ERROR_STREAM(motor.joint_name << ": failure latched");
			}

			// request detailed description of failure
			canopen_query(motor, 'M', 'F', 0);

			motor.state = ST_MOTOR_FAILURE;
		}
		else
		{
			// check if Bit 4 (-> Motor is ON) ist set
			if(motor.curr_status & (1 << 4))
			{
				if(motor.state != ST_OPERATION_ENABLED) {
					ROS_INFO_STREAM(motor.joint_name << ": operation enabled");
				}
				motor.state = ST_OPERATION_ENABLED;
			}
			else
			{
				if(motor.state != ST_OPERATION_DISABLED) {
					ROS_INFO_STREAM(motor.joint_name << ": operation disabled");
				}
				motor.state = ST_OPERATION_DISABLED;
			}
		}
	}

	void evaluate_motor_failure(motor_t& motor, int32_t prev_status)
	{
		if(motor.curr_motor_failure != prev_status)
		{
			if(motor.curr_motor_failure & (1 << 2)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: feedback loss");
			}
			else if(motor.curr_motor_failure & (1 << 3)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: peak current exceeded");
			}
			else if(motor.curr_motor_failure & (1 << 7)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: speed track error");
			}
			else if(motor.curr_motor_failure & (1 << 8)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: position track error");
			}
			else if(motor.curr_motor_failure & (1 << 17)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: speed limit exceeded");
			}
			else if(motor.curr_motor_failure & (1 << 21)) {
				ROS_ERROR_STREAM(motor.joint_name << ": motor failure: motor stuck");
			}
		}
	}

	void receive_loop()
	{
		bool is_error = false;

		while(do_run && ros::ok())
		{
			if(is_error || m_can_sock < 0)
			{
				std::lock_guard<std::mutex> lock(m_can_mutex);

				if(m_can_sock >= 0) {
					::close(m_can_sock);	// close first
				}
				if(is_error) {
					::usleep(1000 * 1000);	// in case of error sleep some time
					if(!do_run) {
						break;
					}
				}
				try {
					// open socket
					m_can_sock = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
					if(m_can_sock < 0) {
						throw std::runtime_error("socket() failed!");
					}
					// set can interface
					::ifreq ifr = {};
					::strncpy(ifr.ifr_name, m_can_iface.c_str(), IFNAMSIZ);
					if(::ioctl(m_can_sock, SIOCGIFINDEX, &ifr) < 0) {
						throw std::runtime_error("ioctl() failed!");
					}
					// bind to interface
					::sockaddr_can addr = {};
					addr.can_family = AF_CAN;
					addr.can_ifindex = ifr.ifr_ifindex;
					if(::bind(m_can_sock, (::sockaddr*)(&addr), sizeof(addr)) < 0) {
						throw std::runtime_error("bind() failed!");
					}
					ROS_INFO_STREAM("CAN interface '" << m_can_iface << "' opened successfully.");
				}
				catch(const std::exception& ex)
				{
					ROS_WARN_STREAM("Failed to open CAN interface '" << m_can_iface << "': "
							<< ex.what() << " (" << ::strerror(errno) << ")");
					is_error = true;
					continue;
				}
				is_error = false;
				m_can_condition.notify_all();	// notify that socket is ready
			}

			// read a frame
			can_frame frame = {};
			const auto res = ::read(m_can_sock, &frame, sizeof(frame));
			if(res != sizeof(frame)) {
				if(do_run) {
					ROS_WARN_STREAM("read() failed with " << ::strerror(errno));
				}
				is_error = true;
				continue;
			}

			// convert frame
			can_msg_t msg;
			msg.id = frame.can_id & 0x1FFFFFFF;
			msg.length = frame.can_dlc;
			for(int i = 0; i < frame.can_dlc; ++i) {
				msg.data[i] = frame.data[i];
			}

			// process it
			{
				std::lock_guard<std::mutex> lock(m_node_mutex);

				m_wait_for_can_sock = false;		// disable waiting for transmit (avoid dead-lock)
				try {
					handle(msg);
				}
				catch(const std::exception& ex) {
					ROS_WARN_STREAM(ex.what());
				}
				m_wait_for_can_sock = true;			// enable waiting again
			}
		}

		// close socket
		{
			std::lock_guard<std::mutex> lock(m_can_mutex);
			if(m_can_sock >= 0) {
				::close(m_can_sock);
				m_can_sock = -1;
			}
			do_run = false;							// tell node that we are done
			m_can_condition.notify_all();			// notify that socket is closed
		}
	}

private:
	std::mutex m_node_mutex;

	ros::NodeHandle m_node_handle;

	ros::Publisher m_pub_joint_state;

	ros::Subscriber m_sub_joint_trajectory;
	ros::Subscriber m_sub_emergency_stop;

	int m_num_wheels = 0;
	std::vector<module_t> m_wheels;

	std::string m_can_iface;
	double m_motor_timeout = 0;
	double m_home_vel = 0;

	volatile bool do_run = true;
	bool is_homing_active = false;
	bool is_steer_reset_active = false;
	bool is_all_homed = false;
	bool is_em_stop = true;
	bool is_motor_reset = true;

	uint64_t m_sync_counter = 0;
	ros::Time m_last_sync_time;
	ros::Time m_last_update_time;

	std::thread m_can_thread;
	std::mutex m_can_mutex;
	std::condition_variable m_can_condition;
	int m_can_sock = -1;
	bool m_wait_for_can_sock = true;

};


int main(int argc, char** argv)
{
	// initialize ROS
	ros::init(argc, argv, "neo_omnidrive_socketcan");

	ros::NodeHandle nh;

	double update_rate = 0;   // [1/s]
	nh.param("update_rate", update_rate, 50.0);

	// frequency of publishing states (cycle time)
	ros::Rate rate(update_rate);

	NeoSocketCanNode node;

	while(ros::ok())
	{
		try {
			node.initialize();
		}
		catch(std::exception& ex)
		{
			if(ros::ok()) {
				ROS_ERROR_STREAM(ex.what());
				::usleep(1000 * 1000);
			}
		}
	}

	while(ros::ok())
	{
		ros::spinOnce();

		try {
			node.update();
		}
		catch(std::exception& ex)
		{
			if(ros::ok()) {
				ROS_ERROR_STREAM(ex.what());
			}
		}

		rate.sleep();
	}

	node.shutdown();

	return 0;
}
