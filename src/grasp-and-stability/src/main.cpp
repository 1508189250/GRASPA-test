/*
* Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
*
* This software may be modified and distributed under the terms of the
* GPL-2+ license. See the accompanying LICENSE file for details.
*/

#include <cstdlib>
#include <string>
#include <iomanip>
#include <sstream>
#include <map>

#include <yarp/os/all.h>
#include <yarp/sig/all.h>
#include <yarp/math/Math.h>

#include <yarp/dev/CartesianControl.h>
#include <yarp/dev/PolyDriver.h>

#include <pugixml.hpp>

#include "src/GraspAndStability_IDL.h"

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::dev;
using namespace yarp::math;

class GraspAndStability: public RFModule, GraspAndStability_IDL
{
    string port_prefix;
    string moving_arm;
    string object_name;
    string output_path;
    string layout_name;
    bool can_grasp;

    // Map of objects names
    std::map<string, string> maps_object_name;

    // Marker pose
    Matrix marker_pose_matrix;

    // Grasp pose to test
    Vector grasp_pose;

    // Grasp stability trajectory
    vector<Vector> trajectory;

    // Port for thrift services
    RpcServer user_rpc;

    // Port to ask for grasp pose
    RpcClient grasp_pose_port;

    // Port to communicare with ARE
    RpcClient action_render_rpc;;

    // Port to communicate with IOL reaching calibration
    RpcClient reach_calib_rpc;

    // Robot params
    string robot;
    string robot_arm;

    // All poses collected for a single object
    vector<Vector> all_grasp_poses;
    vector<double> grasped_values;
    vector<double> grasp_stability_values;
    vector<double> hit_obstacles_values;

    // Home positions
    Vector home_pos_left, home_orie_left;
    Vector home_pos_right, home_orie_right;

    // Approach pose
    Vector grasper_approach_parameters_left;
    Vector grasper_approach_parameters_right;

    // Devices
    PolyDriver left_arm_client, right_arm_client;
    ICartesianControl *icart_right, *icart_left;

    // BufferedPort for reading aruko poses
    BufferedPort<Vector> port_marker_pose_in;

    /****************************************************************/
    void fillMapsObjNames()
    {
        maps_object_name["banana"] = "Banana";
        maps_object_name["chips_can"] = "ChipsCan";
        maps_object_name["cracker_box"] = "CrackerBox";
        maps_object_name["foam_brick"] = "FoamBrick";
        maps_object_name["gelatin_box"] = "GelatinBox";
        maps_object_name["hammer"] = "Hammer";
        maps_object_name["master_chef_can"] = "MasterChefCan";
        maps_object_name["medium_clamp"] = "MediumClamp";
        maps_object_name["mustard_bottle"] = "MustardBottle";
        maps_object_name["pear"] = "Pear";
        maps_object_name["potted_meat_can"] = "PottedMeatCan";
        maps_object_name["power_drill"] = "PowerDrill";
        maps_object_name["scissors"] = "Scissors";
        maps_object_name["strawberry"] = "Strawberry";
        maps_object_name["tennis_ball"] = "TennisBall";
        maps_object_name["tomato_soup_can"] = "TomatoSoupCan";
    }

    /****************************************************************/
    bool configure(ResourceFinder &rf)
    {
        port_prefix = "grasp-and-stability";
        string log_ID = "[Configure]";

        fillMapsObjNames();

        // Read robot name
        if(!rf.check("robot"))
        {
            robot = (rf.check("sim")? "icubSim" : "icub");
        }
        else
        {
            robot = rf.find("robot").asString();
        }

        // Load data path where to save all files
        output_path = rf.check("data-path", Value("grasps_data/")).toString();
        // Load name of benchmark under test
        layout_name = rf.check("layout-name", Value("Benchmark_Layout_0")).toString();
        // Read robot arm
        robot_arm = rf.check("robot-arm", Value("right")).toString();

        // Open devices
        if((robot == "icubSim") || (robot == "icub"))
        {
            Property optionLeftArm, optionRightArm;

            optionLeftArm.put("device", "cartesiancontrollerclient");
            optionLeftArm.put("remote", "/" + robot + "/cartesianController/left_arm");
            optionLeftArm.put("local", "/" + port_prefix + "/cartesianClient/left_arm");

            optionRightArm.put("device", "cartesiancontrollerclient");
            optionRightArm.put("remote", "/" + robot + "/cartesianController/right_arm");
            optionRightArm.put("local", "/" + port_prefix + "/cartesianClient/right_arm");

            if ((robot_arm == "both") || (robot_arm == "left"))
            {
                if (!left_arm_client.open(optionLeftArm))
                {
                    if (left_arm_client.isValid())
                    {
                        left_arm_client.close();
                    }

                    yError() << log_ID <<  "Could not open cartesian solver client for left arm";
                    return false;
                }
                else
                {
                    left_arm_client.view(icart_left);

                    icart_left->getPose(home_pos_left, home_orie_left);
                }
            }
            if ((robot_arm == "both") || (robot_arm == "right"))
            {
                if (!right_arm_client.open(optionRightArm))
                {
                    if (right_arm_client.isValid())
                    {
                        right_arm_client.close();
                    }
                    yError() << log_ID <<  "Could not open cartesian solver client for right arm";
                    return false;
                }
                else
                {
	            right_arm_client.view(icart_right);
                    icart_right->getPose(home_pos_right, home_orie_right);
                }
            }
        }

        // Set parameters for approaching phase during grasp
        grasper_approach_parameters_right.resize(4,0.0);
        grasper_approach_parameters_right[0] = -0.05;
        grasper_approach_parameters_right[1] = 0.0;
        grasper_approach_parameters_right[2] = -0.05;
        grasper_approach_parameters_right[3] = 0.0;

        grasper_approach_parameters_left.resize(4,0.0);
        grasper_approach_parameters_left[0] = -0.05;
        grasper_approach_parameters_left[1] = 0.0;
        grasper_approach_parameters_left[2] = +0.05;
        grasper_approach_parameters_left[3] = 0.0;

        can_grasp = false;
        grasp_pose.resize(7, 0.0);

        moving_arm = "none";

        // Open rpc port
        user_rpc.open("/" + port_prefix + "/cmd:rpc");

        // Open port for aruko pose
        action_render_rpc.open("/" + port_prefix + "/are:rpc");

        // Open port for aruko pose
        grasp_pose_port.open("/" + port_prefix + "/grasp_pose:rpc");

        // Open port for aruko pose
        port_marker_pose_in.open("/" + port_prefix + "/pose:in");

        // Open port for iolReachingCalibration
        reach_calib_rpc.open("/" + port_prefix + "/reach_cal:rpc");

        //  attach callback
        attach(user_rpc);

        bool marker_pose_received = getMarkerPose();

        return marker_pose_received;
    }

    /****************************************************************/
    bool getMarkerPose()
    {
        string log_ID = "[getMarkerPose]";

        // TODO Uncomment this to receive marker pose from port
        Vector *marker_pose = port_marker_pose_in.read();

        if (marker_pose != NULL)
        {
            marker_pose_matrix.resize(4,4);
            marker_pose_matrix.zero();
            marker_pose_matrix.setSubmatrix(axis2dcm(marker_pose->subVector(3,6)), 0, 0);
            marker_pose_matrix.setSubcol(marker_pose->subVector(0,2), 0,3);
            marker_pose_matrix(3,3) = 1.0;

            yInfo() << log_ID << "Received marker pose (Vector): " << marker_pose->toString();
            yInfo() << log_ID << "Received marker pose (Matrix): " << marker_pose_matrix.toString();

            return true;
        }
        else
        {
            yError() << log_ID << " No marker pose received!";
            return false;
        }
    }

    /*****************************************************************/
    bool attach(RpcServer &source)
    {
       return this->yarp().attachAsServer(source);
    }

    /****************************************************************/
    bool updateModule()
    {
        return true;
    }

    /****************************************************************/
    double getPeriod()
    {
        return 0.1;
    }

    /****************************************************************/
    bool interruptModule()
    {
        user_rpc.interrupt();
        action_render_rpc.interrupt();
        grasp_pose_port.interrupt();

        return true;
    }

    /****************************************************************/
    bool close()
    {
        user_rpc.close();
        action_render_rpc.close();
        grasp_pose_port.close();
        reach_calib_rpc.close();

        if (left_arm_client.isValid())
        {
            left_arm_client.close();
        }
        if (right_arm_client.isValid())
        {
            right_arm_client.close();
        }

        return true;
    }

    /****************************************************************/
    bool set_layout_name(const string &name)
    {
        if (name == "Benchmark_Layout_0" || name == "Benchmark_Layout_1"
            || name == "Benchmark_Layout_2")
        {
            layout_name = name;
            return true;
        }
        else
        {
            yError() << "Unknown layout name!";
            return false;
        }
    }

    /****************************************************************/
    bool get_grasp(const string &arm, const string &object)
    {
        string log_ID = "[get_grasp]";

        moving_arm = arm;
        object_name = object;

        Bottle cmd, reply;
        cmd.addString("get_grasp_pose");
        cmd.addString(object_name);
        cmd.addString(moving_arm);

        grasp_pose_port.write(cmd, reply);

    	if (reply.get(0).asString() != "nack")
    	{
    		if (reply.size() > 0)
    		{
    		    yDebug() << reply.toString();
    		    grasp_pose.resize(7,0.0);
    		    Bottle *list_values = reply.get(0).asList();

    		    for (size_t i = 0; i < list_values->size(); i++)
    		    {
    		        grasp_pose[i] = list_values->get(i).asDouble();
    		    }

    		    yInfo() << log_ID << "Received grasp pose (corrected by iolReachingCalibration): " << grasp_pose.toString();
    		    yInfo() << log_ID << "As rotation matrix: " << axis2dcm(grasp_pose.subVector(3,6)).submatrix(0,2,0,2).toString();
    		    can_grasp = true;

                // If the pose has been corrected by iolReachingCalibration we want to store
                // is the original one
                Vector grasp_pose_original(7,0.0);
                fixReachingOffset(grasp_pose, grasp_pose_original);
		yInfo() << log_ID << "Original grasp pose: " << grasp_pose_original.toString();
                all_grasp_poses.push_back(grasp_pose_original);
                yInfo() << log_ID << "Number of poses collected so far for object" << object << " :" << all_grasp_poses.size();

    		    return true;
    		}
    		else
    		{
    		    yError() << log_ID << "Grasp pose not received!";
    		    can_grasp = false;
    		    return false;
    		}
    	}
    	else
    	{
    		yError() << log_ID << "Reply from grasp processore module is nack!";
    		return false;
    	}
    }

    /****************************************************************/
    bool fixReachingOffset(const Vector &pose_received, Vector &pose_from_map,
                           const bool invert=true)
    {
        string log_ID = "[fix_reaching_offset]";
        //  fix the pose offset accordint to iolReachingCalibration
        //  pose is supposed to be (x y z gx gy gz theta)
        if ((robot == "r1" || robot == "icub") && reach_calib_rpc.getOutputCount() > 0)
        {
            Bottle command, reply;

            command.addString("get_location_nolook");
            if (moving_arm == "left")
            {
                command.addString("iol-left");
            }
            else
            {
                command.addString("iol-right");
            }

            command.addDouble(pose_received(0));    //  x value
            command.addDouble(pose_received(1));    //  y value
            command.addDouble(pose_received(2));    //  z value
            command.addInt(invert?1:0);         //  flag to invert input/output map

            reach_calib_rpc.write(command, reply);

            //  incoming reply is going to be (success x y z)
            if (reply.size() < 2)
            {
                yError() << log_ID << "Failure retrieving fixed pose";
                return false;
            }
            else if (reply.get(0).asVocab() == Vocab::encode("ok"))
            {
                pose_from_map = pose_received;
                pose_from_map(0) = reply.get(1).asDouble();
                pose_from_map(1) = reply.get(2).asDouble();
                pose_from_map(2) = reply.get(3).asDouble();
                return true;
            }
            else
            {
                yWarning() << log_ID << "Couldn't retrieve fixed pose. Continuing with unchanged pose";
                pose_from_map = pose_received;
            }
        }
        else
        {
            //  if we are working with the simulator or there is no calib map, the pose doesn't need to be corrected
            pose_from_map = pose_received;
            yWarning() << log_ID << "Connection to iolReachingCalibration not detected or calibration map not present: pose will not be changed";
            return true;
        }
    }


    /****************************************************************/
    bool grasp()
    {
        string log_ID = "[grasp]";

        yInfo() << log_ID << "Trying to move " << moving_arm << "arm";

        if (can_grasp)
        {
            bool reached_pose;
            //  Communication with actionRenderingEngine/cmd:io
            //  grasp("cartesian" x y z gx gy gz theta) ("approach" (-0.05 0 +-0.05 0.0)) "left"/"right"
            Bottle command, reply;

            // TODO Test with ARE
            // without lifting the object
            command.addString("grasp");
            Bottle &ptr = command.addList();
            ptr.addString("cartesian");
            ptr.addDouble(grasp_pose(0));
            ptr.addDouble(grasp_pose(1));
            ptr.addDouble(grasp_pose(2));
            ptr.addDouble(grasp_pose(3));
            ptr.addDouble(grasp_pose(4));
            ptr.addDouble(grasp_pose(5));
            ptr.addDouble(grasp_pose(6));

            // TODO Check if this is correct
            // This is for not make the robot goes back to home
            command.addString("still");

            Bottle &ptr1 = command.addList();
            ptr1.addString("approach");
            Bottle &ptr2 = ptr1.addList();
            if (moving_arm == robot_arm || robot_arm == "both")
            {
                if (moving_arm == "left")
                {
                    for(int i=0 ; i<4 ; i++) ptr2.addDouble(grasper_approach_parameters_left[i]);
                    command.addString("left");
                }
                else
                {
                    for(int i=0 ; i<4 ; i++) ptr2.addDouble(grasper_approach_parameters_right[i]);
                    command.addString("right");
                }

                yInfo() << command.toString();
                action_render_rpc.write(command, reply);
                if (reply.toString() == "[ack]")
                {
                    reached_pose = true;
                }
                else
                {
                    yError() << log_ID << "Problems in grasping the object!";
                    reached_pose = false;
                }

                can_grasp = false;
                return reached_pose;
            }
            else
            {
                yError() << log_ID << "Arm not valid!";
                return false;
            }
        }
        else
        {
            yError() << log_ID << "Grasp cannot be executed because no new pose has been computed";

            return false;
        }
    }

    /****************************************************************/
    bool remove_last_grasp()
    {
        all_grasp_poses.pop_back();
        yInfo() << "[remove_last_grasp]" << "Number of grasped values collected:" << all_grasp_poses.size();
    }

    /****************************************************************/
    bool remove_last_object_data()
    {
        grasped_values.pop_back();
        grasp_stability_values.pop_back();
        hit_obstacles_values.pop_back();

        yInfo() << "[remove_last_object_data]" << "Number of grasped values collected:" << grasped_values.size();
        yInfo() << "[remove_last_object_data]" << "Number of grasp_stability_values collected:" << grasp_stability_values.size();
        yInfo() << "[remove_last_object_data]" << "Number of hit_obstacles_values collected:" << hit_obstacles_values.size();
    }

    /****************************************************************/
    bool add_object_data(const double grasped_value, const double grasp_stability_value, const double hit_obstacles)
    {
        grasped_values.push_back(grasped_value);
        grasp_stability_values.push_back(grasp_stability_value);
        hit_obstacles_values.push_back(hit_obstacles);

        yInfo() << "[add_object_data]" << "Number of grasped values collected:" << grasped_values.size();
        yInfo() << "[add_object_data]" << "Number of grasp_stability_values collected:" << grasp_stability_values.size();
        yInfo() << "[add_object_data]" << "Number of hit_obstacles_values collected:" << hit_obstacles_values.size();

        return true;
    }

    /****************************************************************/
    bool save_grasp_data(const double graspable_value)
    {
        string log_ID = "acquire_grasp_data";

        // Fill file according to Simox convetion
        pugi::xml_document grasps_file;
        pugi::xml_node root = grasps_file.append_child("ManipulationObject");
        root.append_attribute("name") = object_name.c_str();

        pugi::xml_node visualization = root.append_child("Visualization");
        pugi::xml_node file_vis = visualization.append_child("File");
        file_vis.append_attribute("type") = "inventor";
        string model_path = "data/objects/YCB/" + object_name +"/./nontextured.stl";
        file_vis.text().set(model_path.c_str());


        pugi::xml_node collision = root.append_child("CollisionModel");
        pugi::xml_node file_collision = collision.append_child("File");
        file_collision.append_attribute("type") = "inventor";
        file_collision.text().set(model_path.c_str());

        pugi::xml_node grasps = root.append_child("GraspSet");
        grasps.append_attribute("name") =  layout_name.c_str();
        grasps.append_attribute("RobotType") =  "iCub";
        if (robot_arm == "left")
            grasps.append_attribute("EndEffector") =  "Left Hand";
        else
            grasps.append_attribute("EndEffector") =  "Right Hand";

        for (size_t i = 0 ; i < all_grasp_poses.size(); i++)
        {
            pugi::xml_node single_grasp = grasps.append_child("Grasp");
            single_grasp.append_attribute("name") = ("Grasp " + std::to_string(i)).c_str();
            single_grasp.append_attribute("quality") = 0.0;  // TODO This will be computed by the script
            single_grasp.append_attribute("Creation") = "Simox - GraspStudio - GraspWrenchSpace";
            single_grasp.append_attribute("Preshape") = "Grasp Preshape";

            pugi::xml_node pose = single_grasp.append_child("Transform");
            pugi::xml_node matrix = pose.append_child("Matrix4x4");

            // TODO tO CHECK CORRECTNESS
            Vector grasp_pose_om(4,1.0);
            grasp_pose_om.setSubvector(0,all_grasp_poses[i].subVector(0,2));

            Vector position = (SE3inv(marker_pose_matrix) * grasp_pose_om).subVector(0,2);

            Matrix R_3x3 = axis2dcm(all_grasp_poses[i].subVector(3,6)).submatrix(0,2,0,2);
            Matrix marker_pose_3x3_inv = SE3inv(marker_pose_matrix).submatrix(0,2,0,2);

            Matrix orientation_marker_frame = (marker_pose_3x3_inv * R_3x3);

            pugi::xml_node row1 = matrix.append_child("row1");
            row1.append_attribute("c1") = toStringPrecision(orientation_marker_frame(0,0),3).c_str();
            row1.append_attribute("c2") = toStringPrecision(orientation_marker_frame(0,1),3).c_str();
            row1.append_attribute("c3") = toStringPrecision(orientation_marker_frame(0,2),3).c_str();
            row1.append_attribute("c4") = toStringPrecision(position(0) * 1000,3).c_str();

            pugi::xml_node row2= matrix.append_child("row2");
            row2.append_attribute("c1") = toStringPrecision(orientation_marker_frame(1,0),3).c_str();
            row2.append_attribute("c2") = toStringPrecision(orientation_marker_frame(1,1),3).c_str();
            row2.append_attribute("c3") = toStringPrecision(orientation_marker_frame(1,2),3).c_str();
            row2.append_attribute("c4") = toStringPrecision(position(1) * 1000,3).c_str();

            pugi::xml_node row3 = matrix.append_child("row3");
            row3.append_attribute("c1") = toStringPrecision(orientation_marker_frame(2,0),3).c_str();
            row3.append_attribute("c2") = toStringPrecision(orientation_marker_frame(2,1),3).c_str();
            row3.append_attribute("c3") = toStringPrecision(orientation_marker_frame(2,2),3).c_str();
            row3.append_attribute("c4") = toStringPrecision(position(2) * 1000,3).c_str();

            pugi::xml_node row4 = matrix.append_child("row4");
            row4.append_attribute("c1") = 0;
            row4.append_attribute("c2") = 0;
            row4.append_attribute("c3") = 0;
            row4.append_attribute("c4") = 1;
        }

        pugi::xml_node graspable = grasps_file.append_child("Graspable");
        graspable.append_attribute("quality") = graspable_value;

        pugi::xml_node grasped = grasps_file.append_child("Grasped");
        for (size_t i = 0 ; i < all_grasp_poses.size(); i++)
        {
            pugi::xml_node grasped_item = grasped.append_child("Grasp");
            grasped_item.append_attribute("name") = ("Grasp " + std::to_string(i)).c_str();
            grasped_item.append_attribute("quality") = grasped_values[i];
        }

        pugi::xml_node grasp_stability = grasps_file.append_child("GraspStability");
        for (size_t i = 0 ; i < all_grasp_poses.size(); i++)
        {
            pugi::xml_node grasp_stability_item = grasp_stability.append_child("Grasp");
            grasp_stability_item.append_attribute("name") = ("Grasp " + std::to_string(i)).c_str();
            grasp_stability_item.append_attribute("quality") = grasp_stability_values[i];
        }

        pugi::xml_node obstacles_node = grasps_file.append_child("ObstacleAvoidance");
        for (size_t i = 0 ; i < all_grasp_poses.size(); i++)
        {
            pugi::xml_node obstacles_node_item = obstacles_node.append_child("Grasp");
            obstacles_node_item.append_attribute("name") = ("Grasp " + std::to_string(i)).c_str();
            obstacles_node_item.append_attribute("quality") = hit_obstacles_values[i];
        }

        string complete_path_file = "Ycb" + maps_object_name[object_name] + "_grasp.xml";
        grasps_file.save_file(complete_path_file.c_str());
        // TODO Use this if xml declaration in file is a problem
        //grasps_file.save_file(complete_path_file.c_str(),"\t", pugi::format_no_declaration);

        yInfo() << log_ID << "Grasps data saved in file " << "Ycb" + maps_object_name[object_name.c_str()] + "_grasp.xml";

        return true;
    }

    /****************************************************************/
    bool reset_poses()
    {
        all_grasp_poses.clear();
        grasped_values.clear();
        grasp_stability_values.clear();
        hit_obstacles_values.clear();
        yInfo() << "[reset_poses]" << "Grasp poses reset";
        return true;
    }

    /****************************************************************/
    string toStringPrecision(double input,int n)
    {
        stringstream stream;
        stream << fixed << setprecision(n) << input;
        return stream.str();
    }

    /****************************************************************/
    bool generate_trajectory()
    {
        return generateTrajectory();
    }

    /****************************************************************/
    bool execute_trajectory()
    {
        string log_ID = "[execute_trajectory]";
        if (moving_arm != robot_arm && robot_arm != "both")
        {
            yError() << log_ID << "Not valid arm!";
            return false;
        }

        if (trajectory.size() == 0)
        {
            yError() << log_ID << "Trajectory size 0!";
            return false;
        }
        else
        {
            int count = 0;

            for (auto t : trajectory)
            {
                if (moving_arm == "left")
                {
                    int context_0;
                    icart_left->storeContext(&context_0);

                    yInfo() << log_ID << " Storing old context of cartesian and setting new parameters...";

                    Vector dof;
                    icart_left->getDOF(dof);
                    Vector new_dof(10, 1);
                    new_dof(1) = 0.0;
                    icart_left->setDOF(new_dof, dof);
                    icart_left->setInTargetTol(0.005);
                    icart_left->setLimits(0, 0.0, 25.0);

                    yInfo() << log_ID << "Going to pose No." << count << " with position: " << t.subVector(0,2).toString();
                    yInfo() << log_ID << "                   and orientation: " << "and orientation: " << t.subVector(3,6).toString();
                    icart_left->goToPoseSync(t.subVector(0,2), t.subVector(3,6));
                    icart_left->waitMotionDone(0.4, 4.0);

                    icart_left->restoreContext(context_0);

                    yInfo() << log_ID << " Restored context of cartesian";
                }
                else if (moving_arm == "right")
                {
                    int context_0;
                    icart_right->storeContext(&context_0);

                    yInfo() << log_ID << " Storing old context of cartesian and setting new parameters...";

                    Vector dof;
                    icart_right->getDOF(dof);
                    Vector new_dof(10, 1);
                    new_dof(1) = 0.0;
                    icart_right->setDOF(new_dof, dof);
                    icart_right->setInTargetTol(0.005);
                    icart_right->setLimits(0, 0.0, 25.0);

                    yInfo() << log_ID << "Going to pose No." << count << " with position: " << t.subVector(0,2).toString();
                    yInfo() << log_ID << "                   and orientation: " << t.subVector(3,6).toString();
                    icart_right->goToPoseSync(t.subVector(0,2), t.subVector(3,6));
                    icart_right->waitMotionDone(0.4, 4.0);

                    icart_right->restoreContext(context_0);

                    yInfo() << log_ID << " Restored context of cartesian";
                }
                count ++;
            }
            return true;
        }
    }

    /****************************************************************/
    bool home()
    {
        string log_ID = "[home]";

        Bottle cmd, reply;
        cmd.addVocab(Vocab::encode("home"));
        cmd.addVocab(Vocab::encode("all"));
        action_render_rpc.write(cmd, reply);
        bool cmd_success = (reply.get(0).asVocab() == Vocab::encode("ack"));

        if (cmd_success)
            return true;
        else
        {
            yError() << log_ID << "Problems in going home";
            return false;
        }
    }

    /****************************************************************/
    bool generateTrajectory()
    {
        string log_ID = "[generateTrajectory]";
        trajectory.clear();

        if (norm(grasp_pose.subVector(0,2)) > 0.0)
        {
            Vector pose_0 = grasp_pose;

            Vector pose_tmp = pose_0;
            pose_tmp[2] += 0.15;

            // Compute waypoint 0
            Matrix pose_rotate_hf(3,3);
            // This is express in hand reference frame
            Vector aa_rotate_hf(4, 0.0);
            aa_rotate_hf[0] = 1.0;
            aa_rotate_hf[3] = M_PI/4,0;

            pose_rotate_hf = axis2dcm(aa_rotate_hf).submatrix(0,2,0,2);

            // Hand pose in robot frame
            Matrix pose_hand_rf(3,3);
            pose_hand_rf = axis2dcm(pose_0.subVector(3,6)).submatrix(0,2,0,2);

            // Rotation required in robot reference frame
            Matrix pose_rotate_rf = pose_hand_rf * pose_rotate_hf;

            // Add waypoint 0
            pose_tmp.setSubvector(3, dcm2axis(pose_rotate_rf));
            trajectory.push_back(pose_tmp);

            // Add waypoint 1
            pose_tmp = pose_0;
            pose_tmp[2] += 0.15;
            trajectory.push_back(pose_tmp);

            // Compute waypoint 2
            // This is express in hand reference frame
            aa_rotate_hf.resize(4, 0.0);
            aa_rotate_hf[0] = 1.0;
            aa_rotate_hf[3] = -M_PI/4,0;

            pose_rotate_hf = axis2dcm(aa_rotate_hf).submatrix(0,2,0,2);

            // Rotation required in robot reference frame
            pose_rotate_rf = pose_hand_rf * pose_rotate_hf;

            // Add waypoint 2
            pose_tmp.setSubvector(3, dcm2axis(pose_rotate_rf));
            trajectory.push_back(pose_tmp);

            yInfo() << log_ID << "Generated trajectory";
            int count = 0;
            for (auto p : trajectory)
            {
                yInfo() << log_ID << "No. " << count << " : " << p.toString();
                yInfo() << log_ID << "As rotation matrix: " << axis2dcm(p.subVector(3,6)).submatrix(0,2,0,2).toString();
                count++;
            }

            return true;
        }
        else
        {
            yError() << log_ID << "Error in trajectory size: " << trajectory.size();
            return false;
        }
    }
};

int main(int argc, char** argv)
{
    const std::string log_ID = "[Main]";
    yInfo() << log_ID << "Configuring and starting module...";

    const std::string port_prefix = "grasp-and-stability";

    std::unique_ptr<Network> yarp;
    yarp = std::move(std::unique_ptr<Network>(new Network()));

    if (!yarp->checkNetwork())
    {
            yError() << log_ID << "YARP seems unavailable!";
            return EXIT_FAILURE;
    }

    ResourceFinder rf;
    rf.setDefaultContext("grasp-and-stability");
    rf.setDefaultConfigFile("config.ini");
    rf.configure(argc, argv);

    GraspAndStability module;

    return module.runModule(rf);
}
