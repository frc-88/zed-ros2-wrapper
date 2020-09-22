#include "zed_camera_component.hpp"
#include "sl_tools.h"
#include <type_traits>

#include <sensor_msgs/distortion_models.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <sensor_msgs/msg/point_field.hpp>

using namespace std::chrono_literals;
using namespace std::placeholders;

#ifndef TIMER_ELAPSED
#define TIMER_ELAPSED double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count()
#endif

namespace stereolabs {

#ifndef DEG2RAD
#define DEG2RAD 0.017453293
#define RAD2DEG 57.295777937
#endif

ZedCamera::ZedCamera(const rclcpp::NodeOptions &options)
    : Node("zed_node", options)
    , mVideoQos(1)
    , mDepthQos(1)
    , mSensQos(1)
    , mPoseQos(1) {

    std::string ros_namespace = get_namespace();
    std::string node_name = get_name();

    RCLCPP_INFO(get_logger(), "********************************");
    RCLCPP_INFO(get_logger(), "      ZED Camera Component ");
    RCLCPP_INFO(get_logger(), "********************************");
    RCLCPP_INFO(get_logger(), " * namespace: %s", get_namespace());
    RCLCPP_INFO(get_logger(), " * node name: %s", get_name());
    RCLCPP_INFO(get_logger(), "********************************");

    // ----> Parameters initialization
    getParam( "general.debug_mode", mDebugMode, mDebugMode );
    if (mDebugMode) {
        rcutils_ret_t res = rcutils_logging_set_logger_level(get_logger().get_name(), RCUTILS_LOG_SEVERITY_DEBUG);

        if (res != RCUTILS_RET_OK) {
            RCLCPP_INFO(get_logger(), "Error setting DEBUG level fot logger");
        } else {
            RCLCPP_INFO(get_logger(), "*** Debug Mode enabled ***");
        }
    } else {
        rcutils_ret_t res = rcutils_logging_set_logger_level(get_logger().get_name(), RCUTILS_LOG_SEVERITY_INFO);

        if (res != RCUTILS_RET_OK) {
            RCLCPP_INFO(get_logger(), "Error setting INFO level for logger");
        } else {
            RCLCPP_INFO(get_logger(), "*** Debug Mode enabled ***");
        }
    }

    RCLCPP_DEBUG(get_logger(), "[ROS2] Using RMW_IMPLEMENTATION = %s", rmw_get_implementation_identifier());

    initParameters();
    // <---- Parameters initialization

    // Start camera
    startCamera();
}

ZedCamera::~ZedCamera() {
    RCLCPP_DEBUG(get_logger(), "Destroying node");

    // ----> Verify that all the threads are not active
    if (!mThreadStop) {
        mThreadStop = true;

        RCLCPP_DEBUG(get_logger(), "Stopping grab thread...");
        try {
            if (mGrabThread.joinable()) {
                mGrabThread.join();
            }
        } catch (std::system_error& e) {
            RCLCPP_WARN(get_logger(), "Grab thread joining exception: %s", e.what());
        }
        RCLCPP_DEBUG(get_logger(), "... grab thread stopped");

        RCLCPP_DEBUG(get_logger(), "Stopping Point Cloud thread...");
        try {
            if (mPcThread.joinable()) {
                mPcThread.join();
            }
        } catch (std::system_error& e) {
            RCLCPP_WARN(get_logger(), "Pointcloud thread joining exception: %s", e.what());
        }
        RCLCPP_DEBUG(get_logger(), "... Point Cloud thread stopped");
    }
    // <---- Verify that the grab thread is not active
}

template<typename T>
void ZedCamera::getParam(std::string paramName, T defValue, T& outVal, std::string log_info )
{
    declare_parameter(paramName, rclcpp::ParameterValue(defValue) );

    if (!get_parameter(paramName, outVal)) {
        RCLCPP_WARN_STREAM(get_logger(), "The parameter '" << paramName << "' is not available or is not valid, using the default value: " <<  defValue);
    }

    if ( !log_info.empty() ) {
        RCLCPP_INFO_STREAM(get_logger(),  log_info << outVal );
    }
}

void ZedCamera::initParameters() {
    // GENERAL parameters
    getGeneralParams();

    // VIDEO parameters
    getVideoParams();

    // DEPTH parameters
    getDepthParams();

    // POS. TRACKING parameters
    getPosTrackingParams();

    // SENSORS parameters
    if (mCamUserModel != sl::MODEL::ZED) {
        //getSensorsParams();
    }

    // TODO MAPPING PARAMETERS

    // TODO OD PARAMETERS

    // Dynamic parameters callback
    set_on_parameters_set_callback(std::bind(&ZedCamera::callback_paramChange, this, _1));
}

void ZedCamera::getGeneralParams() {
    rclcpp::Parameter paramVal;
    std::string paramName;

    RCLCPP_INFO(get_logger(), "*** GENERAL parameters ***");

    std::string camera_model = "zed";
    getParam( "general.camera_model", camera_model, camera_model );
    if (camera_model == "zed") {
        mCamUserModel = sl::MODEL::ZED;
    } else if (camera_model == "zedm") {
        mCamUserModel = sl::MODEL::ZED_M;
    } else if (camera_model == "zed2") {
        mCamUserModel = sl::MODEL::ZED2;
    } else {
        RCLCPP_ERROR_STREAM(get_logger(), "Camera model not valid in parameter values: " << camera_model);
    }
    RCLCPP_INFO(get_logger(), " * Camera model: %s (%s)", camera_model.c_str(),
                sl::toString(static_cast<sl::MODEL>(mCamUserModel)).c_str());

    getParam( "general.sdk_verbose", mVerbose, mVerbose,  " * SDK Verbose: ");
    getParam( "general.svo_file", std::string(), mSvoFilepath, " * SVO: ");
    getParam( "general.svo_loop", mSvoLoop, mSvoLoop);
    RCLCPP_INFO(get_logger(), " * SVO Loop: %s", mSvoLoop?"TRUE":"FALSE");
    getParam( "general.camera_name", mCameraName, mCameraName,  " * Camera name: ");
    getParam( "general.zed_id", mCamId, mCamId,  " * Camera ID: ");
    getParam( "general.serial_number", mCamSerialNumber, mCamSerialNumber,  " * Camera SN: ");
    getParam( "general.camera_timeout_sec", mCamTimeoutSec, mCamTimeoutSec,  " * Camera timeout [sec]: ");
    getParam( "general.camera_reactivate", mCamReactivate, mCamReactivate,  " * Camera reconnection if disconnected: ");
    getParam( "general.camera_max_reconnect", mMaxReconnectTemp, mMaxReconnectTemp,  " * Camera reconnection temptatives: ");
    getParam( "general.grab_frame_rate", mCamFrameRate, mCamFrameRate,  " * Camera framerate: ");
    getParam( "general.gpu_id", mGpuId, mGpuId,  " * GPU ID: ");
    getParam( "general.base_frame", mBaseFrameId, mBaseFrameId,  " * Base frame id: ");

    // TODO ADD SVO SAVE COMPRESSION PARAMETERS

    int resol = static_cast<int>(mCamResol);
    getParam( "general.resolution", resol, resol );
    mCamResol = static_cast<sl::RESOLUTION>(resol);
    RCLCPP_INFO(get_logger(), " * Camera resolution: %d (%s)", resol, sl::toString(mCamResol).c_str());

    getParam( "general.self_calib", mCameraSelfCalib, mCameraSelfCalib );
    RCLCPP_INFO(get_logger(), " * Camera self calibration: %s", mCameraSelfCalib?"TRUE":"FALSE");
    getParam( "general.camera_flip", mCameraFlip, mCameraFlip );
    RCLCPP_INFO(get_logger(), " * Camera flip: %s", mCameraFlip?"TRUE":"FALSE");

    // Dynamic parameters


    getParam( "general.pub_frame_rate", mPubFrameRate, mPubFrameRate );
    if ( mPubFrameRate>mCamFrameRate )
    {
        RCLCPP_WARN(get_logger(), "'pub_frame_rate' cannot be bigger than 'grab_frame_rate'", paramName.c_str());
    }
    RCLCPP_INFO(get_logger(), " * [DYN] Publish framerate [Hz]: %g ", mPubFrameRate);
}

void ZedCamera::getVideoParams() {
    rclcpp::Parameter paramVal;
    std::string paramName;

    RCLCPP_INFO(get_logger(), "*** VIDEO parameters ***");

    rmw_qos_history_policy_t qos_hist = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    size_t qos_depth = 10;
    rmw_qos_reliability_policy_t qos_reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    rmw_qos_durability_policy_t qos_durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;

    getParam( "video.extrinsic_in_camera_frame", mUseOldExtrinsic, mUseOldExtrinsic,  " * Use old extrinsic parameters: ");


    getParam( "video.img_downsample_factor", mImgDownsampleFactor, mImgDownsampleFactor );
    if (mImgDownsampleFactor < 0.1) {
        mImgDownsampleFactor = 0.1;
        RCLCPP_WARN(get_logger(), "The minimum value allowed for '%s' is 0.1", paramName.c_str());
    } else if (mImgDownsampleFactor > 1.0) {
        mImgDownsampleFactor = 1.0;
        RCLCPP_WARN(get_logger(), "The maximum value allowed for '%s' is 1.0", paramName.c_str());
    }
    RCLCPP_INFO(get_logger(), " * [DYN] Image downsample factor: %g ", mImgDownsampleFactor);

    getParam( "video.brightness", mCamBrightness, mCamBrightness,  " * [DYN] Brightness: ");
    getParam( "video.contrast", mCamContrast, mCamContrast,  " * [DYN] Contrast: ");
    getParam( "video.hue", mCamHue, mCamHue,  " * [DYN] Hue: ");
    getParam( "video.saturation", mCamSaturation, mCamSaturation,  " * [DYN] Saturation: ");
    getParam( "video.sharpness", mCamSharpness, mCamSharpness,  " * [DYN] Sharpness: ");
    getParam( "video.gamma", mCamGamma, mCamGamma,  " * [DYN] Gamma: ");
    getParam( "video.auto_exposure_gain", mCamAutoExpGain, mCamAutoExpGain);
    RCLCPP_INFO(get_logger(), " * [DYN] Auto Exposure/Gain: %s", mCamAutoExpGain?"TRUE":"FALSE");
    if (mCamAutoExpGain) {
        mTriggerAutoExpGain = true;
    }
    getParam( "video.exposure", mCamExposure, mCamExposure,  " * [DYN] Exposure: ");
    getParam( "video.gain", mCamGain, mCamGain,  " * [DYN] Gain: ");
    getParam( "video.auto_whitebalance", mCamAutoWB, mCamAutoWB);
    RCLCPP_INFO(get_logger(), " * [DYN] Auto White Balance: %s", mCamAutoWB?"TRUE":"FALSE");
    if (mCamAutoWB) {
        mTriggerAutoWB = true;
    }
    getParam( "video.whitebalance_temperature", mCamWBTemp, mCamWBTemp,  " * [DYN] White Balance Temperature: ");


    // ------------------------------------------

    paramName = "video.qos_history";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_hist = paramVal.as_int() == 0 ? RMW_QOS_POLICY_HISTORY_KEEP_LAST : RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        mVideoQos.history(qos_hist);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Video QoS History: '%s'", sl_tools::qos2str(qos_hist).c_str());

    // ------------------------------------------

    paramName = "video.qos_depth";
    declare_parameter(paramName, rclcpp::ParameterValue(10) );

    if (get_parameter(paramName, paramVal)) {
        qos_depth = paramVal.as_int();
        mVideoQos.keep_last( qos_depth );
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Video QoS History depth: '%d'", qos_depth);

    // ------------------------------------------

    paramName = "video.qos_reliability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_reliability = paramVal.as_int() == 0 ? RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT :
                                                   RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        mVideoQos.reliability(qos_reliability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Video QoS Reliability: '%s'", sl_tools::qos2str(qos_reliability).c_str());

    // ------------------------------------------

    paramName = "video.qos_durability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_durability= paramVal.as_int() == 0 ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL :
                                                 RMW_QOS_POLICY_DURABILITY_VOLATILE;
        mVideoQos.durability(qos_durability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Video QoS Durability: '%s'", sl_tools::qos2str(qos_durability).c_str());
}

void ZedCamera::getDepthParams() {
    rclcpp::Parameter paramVal;
    std::string paramName;

    rmw_qos_history_policy_t qos_hist = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    size_t qos_depth = 10;
    rmw_qos_reliability_policy_t qos_reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    rmw_qos_durability_policy_t qos_durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;

    RCLCPP_INFO(get_logger(), "*** DEPTH parameters ***");

    getParam( "depth.depth_downsample_factor", mDepthDownsampleFactor, mDepthDownsampleFactor );
    if (mDepthDownsampleFactor < 0.1) {
        mDepthDownsampleFactor = 0.1;
        RCLCPP_WARN(get_logger(), "The minimum value allowed for '%s' is 0.1", paramName.c_str());
    } else if (mDepthDownsampleFactor > 1.0) {
        mDepthDownsampleFactor = 1.0;
        RCLCPP_WARN(get_logger(), "The maximum value allowed for '%s' is 1.0", paramName.c_str());
    }
    RCLCPP_INFO(get_logger(), " * Depth downsample factor: %g ", mDepthDownsampleFactor);

    int depth_quality = static_cast<int>(mDepthQuality);
    getParam( "depth.quality", depth_quality, depth_quality );
    mDepthQuality = static_cast<sl::DEPTH_MODE>(depth_quality);
    RCLCPP_INFO(get_logger(), " * Depth quality: %d (%s)", depth_quality, sl::toString(mDepthQuality).c_str());

    getParam( "depth.min_depth", mCamMinDepth, mCamMinDepth, " * Min depth [m]: ");
    getParam( "depth.max_depth", mCamMaxDepth, mCamMaxDepth, " * Max depth [m]: ");

    int sens_mode = static_cast<int>(mDepthSensingMode);
    getParam( "depth.sensing_mode", sens_mode, sens_mode );
    mDepthSensingMode = static_cast<sl::SENSING_MODE>(sens_mode);
    RCLCPP_INFO(get_logger(), " * Depth Sensing Mode: %d (%s)", sens_mode, sl::toString(mDepthSensingMode).c_str());

    getParam( "depth.depth_stabilization", mDepthStabilization, mDepthStabilization );
    RCLCPP_INFO(get_logger(), " * Depth Stabilization: %s", mDepthStabilization?"TRUE":"FALSE");

    getParam( "depth.openni_depth_mode", mOpenniDepthMode, mOpenniDepthMode );
    RCLCPP_INFO(get_logger(), " * OpenNI mode (16bit point cloud): %s", mOpenniDepthMode?"TRUE":"FALSE");

    getParam( "depth.point_cloud_freq", mPcPubRate, mPcPubRate, " * [DYN] Point cloud rate [Hz]: " );

    getParam( "depth.depth_confidence", mDepthConf, mDepthConf, " * [DYN] Depth Confidence: " );
    getParam( "depth.depth_texture_conf", mDepthTextConf, mDepthTextConf, " * [DYN] Depth Texture Confidence: " );

    // ------------------------------------------

    paramName = "depth.qos_history";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_hist = paramVal.as_int() == 0 ? RMW_QOS_POLICY_HISTORY_KEEP_LAST : RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        mDepthQos.history(qos_hist);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Depth QoS History: '%s'", sl_tools::qos2str(qos_hist).c_str());

    // ------------------------------------------

    paramName = "depth.qos_depth";
    declare_parameter(paramName, rclcpp::ParameterValue(10) );

    if (get_parameter(paramName, paramVal)) {
        qos_depth  = paramVal.as_int();
        mDepthQos.keep_last(qos_depth);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Depth QoS History depth: '%d'", qos_depth);

    // ------------------------------------------

    paramName = "depth.qos_reliability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_reliability = paramVal.as_int() == 0 ? RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT :
                                                   RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        mDepthQos.reliability(qos_reliability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Depth QoS Reliability: '%s'", sl_tools::qos2str(qos_reliability).c_str());

    // ------------------------------------------

    paramName = "depth.qos_durability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_durability = paramVal.as_int() == 0 ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL :
                                                  RMW_QOS_POLICY_DURABILITY_VOLATILE;
        mDepthQos.durability(qos_durability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Depth QoS Durability: '%s'", sl_tools::qos2str(qos_durability).c_str());
}

void ZedCamera::getPosTrackingParams() {
    rclcpp::Parameter paramVal;
    std::string paramName;

    rmw_qos_history_policy_t qos_hist = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    size_t qos_depth = 10;
    rmw_qos_reliability_policy_t qos_reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    rmw_qos_durability_policy_t qos_durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;

    RCLCPP_INFO(get_logger(), "*** POSITIONAL TRACKING parameters ***");

    getParam( "pos_tracking.publish_tf", mPublishTF, mPublishTF );
    RCLCPP_INFO_STREAM(get_logger(), " * Broadcast Odometry TF: " << (mPublishTF?"TRUE":"FALSE") );
    if (mPublishTF) {
        getParam( "pos_tracking.publish_map_tf", mPublishMapTF, mPublishMapTF );
        RCLCPP_INFO_STREAM(get_logger(), " * Broadcast Pose TF: " << (mPublishMapTF?"TRUE":"FALSE") );
        getParam( "pos_tracking.publish_imu_tf", mPublishImuTF, mPublishImuTF );
        RCLCPP_INFO_STREAM(get_logger(), " * Broadcast Static IMU TF [not for ZED]: " << (mPublishImuTF?"TRUE":"FALSE") );
    }

    getParam( "pos_tracking.path_pub_rate", mPathPubRate, mPathPubRate, " * [DYN] Path publishing rate: " );
    getParam( "pos_tracking.path_max_count", mPathMaxCount, mPathMaxCount );
    if (mPathMaxCount < 2 && mPathMaxCount != -1) {
        mPathMaxCount = 2;
    }
    RCLCPP_INFO_STREAM(get_logger(), " * Path history lenght: " << mPathMaxCount);

    paramName = "pos_tracking.initial_base_pose";
    declare_parameter(paramName, rclcpp::ParameterValue(mInitialBasePose) );
    if (!get_parameter(paramName, mInitialBasePose)) {
        RCLCPP_WARN_STREAM(get_logger(), "The parameter '" << paramName << "' is not available or is not valid, using the default value");
        mInitialBasePose = std::vector<double>(6, 0.0);
    }
    if (mInitialBasePose.size()<6)
    {
        RCLCPP_WARN_STREAM(get_logger(), "The parameter '" << paramName << "' must be a vector of 6 values of double type");
        mInitialBasePose = std::vector<double>(6, 0.0);
    }
    RCLCPP_INFO(get_logger(), " * Initial pose: [%g,%g,%g,%g,%g,%g,]",
                mInitialBasePose[0],mInitialBasePose[1],mInitialBasePose[2],
            mInitialBasePose[3],mInitialBasePose[4],mInitialBasePose[5]);

    getParam( "pos_tracking.area_memory", mAreaMemory, mAreaMemory );
    RCLCPP_INFO_STREAM(get_logger(), " * Area Memory: " << (mAreaMemory?"TRUE":"FALSE") );
    getParam( "pos_tracking.area_memory_db_path", mAreaMemoryDbPath, mAreaMemoryDbPath, " * Area Memory DB: " );
    getParam( "pos_tracking.imu_fusion", mImuFusion, mImuFusion );
    RCLCPP_INFO_STREAM(get_logger(), " * IMU Fusion [not for ZED]: " << (mImuFusion?"TRUE":"FALSE") );
    getParam( "pos_tracking.floor_alignment", mFloorAlignment, mFloorAlignment );
    RCLCPP_INFO_STREAM(get_logger(), " * Floor Alignment: " << (mFloorAlignment?"TRUE":"FALSE") );
    getParam( "pos_tracking.init_odom_with_first_valid_pose", mInitOdomWithPose, mInitOdomWithPose );
    RCLCPP_INFO_STREAM(get_logger(), " * Init Odometry with first valid pose data: " << (mInitOdomWithPose?"TRUE":"FALSE") );
    getParam( "pos_tracking.two_d_mode", mTwoDMode, mTwoDMode );
    RCLCPP_INFO_STREAM(get_logger(), " * 2D mode: " << (mTwoDMode?"TRUE":"FALSE") );
    if (mTwoDMode) {
        getParam( "pos_tracking.fixed_z_value", mFixedZValue, mFixedZValue, " * Fixed Z value: " );
    }

    // ------------------------------------------

    paramName = "tracking.qos_history";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_hist = paramVal.as_int() == 0 ? RMW_QOS_POLICY_HISTORY_KEEP_LAST : RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        mPoseQos.history(qos_hist);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Pose/Odometry QoS History: '%s'", sl_tools::qos2str(qos_hist).c_str());

    // ------------------------------------------

    paramName = "tracking.qos_depth";
    declare_parameter(paramName, rclcpp::ParameterValue(10) );

    if (get_parameter(paramName, paramVal)) {
        qos_depth = paramVal.as_int();
        mPoseQos.keep_last(qos_depth);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Pose/Odometry QoS History depth: '%d'", qos_depth);

    // ------------------------------------------

    paramName = "tracking.qos_reliability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_reliability = paramVal.as_int() == 0 ? RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT :
                                                   RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        mPoseQos.reliability(qos_reliability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Pose/Odometry QoS Reliability: '%s'", sl_tools::qos2str(qos_reliability).c_str());

    // ------------------------------------------

    paramName = "tracking.qos_durability";
    declare_parameter(paramName, rclcpp::ParameterValue(0) );

    if (get_parameter(paramName, paramVal)) {
        qos_durability = paramVal.as_int() == 0 ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL :
                                                  RMW_QOS_POLICY_DURABILITY_VOLATILE;
        mPoseQos.durability(qos_durability);
    } else {
        RCLCPP_WARN(get_logger(), "The parameter '%s' is not available, using the default value", paramName.c_str());
    }

    RCLCPP_INFO(get_logger(), " * Pose/Odometry QoS Durability: '%s'", sl_tools::qos2str(qos_durability).c_str());

}

rcl_interfaces::msg::SetParametersResult ZedCamera::callback_paramChange(std::vector<rclcpp::Parameter> parameters) {

    auto result = rcl_interfaces::msg::SetParametersResult();
    result.successful = false;

    //    for (size_t i = 0; i < parameters.size(); i++) {
    //        rclcpp::Parameter param = parameters[i];

    //        if (param.get_name() == "general.mat_resize_factor") {
    //            if (param.get_type() == rclcpp::PARAMETER_DOUBLE) {

    //                double new_val = param.as_double();

    //                if (new_val > 0.01 && new_val <= 1.0) {
    //                    mZedImgDownsampleFactor = new_val;
    //                    RCLCPP_INFO(get_logger(), "The param '%s' has changed to %g", param.get_name().c_str(), mZedImgDownsampleFactor);
    //                    result.successful = true;

    //                    // ----> Modify data sizes
    //                    mCamDataMutex.lock();
    //                    mMatWidth = static_cast<size_t>(mCamWidth * mZedImgDownsampleFactor);
    //                    mMatHeight = static_cast<size_t>(mCamHeight * mZedImgDownsampleFactor);
    //                    RCLCPP_INFO(get_logger(), "Data Mat size : %d x %d", mMatResolDepth);

    //                    // Update Camera Info
    //                    fillCamInfo(mZed, mLeftCamInfoMsg, mRightCamInfoMsg, mLeftCamOptFrameId, mRightCamOptFrameId);
    //                    fillCamInfo(mZed, mLeftCamInfoRawMsg, mRightCamInfoRawMsg, mLeftCamOptFrameId, mRightCamOptFrameId, true);
    //                    mRgbCamInfoMsg = mDepthCamInfoMsg = mLeftCamInfoMsg;
    //                    mRgbCamInfoRawMsg = mLeftCamInfoRawMsg;
    //                    mCamDataMutex.unlock();
    //                    // <---- Modify data sizes
    //                } else {
    //                    RCLCPP_WARN(get_logger(), "The param '%s' requires a FLOATING POINT value in the range ]0.0,1.0]",
    //                                param.get_name().c_str());
    //                    result.successful = false;
    //                    return result;
    //                }
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires a FLOATING POINT positive value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else if (param.get_name() == "video.auto_exposure_gain") {
    //            if (param.get_type() == rclcpp::PARAMETER_BOOL) {

    //                mZedAutoExpGain = param.as_bool();

    //                if (mZedAutoExpGain) {
    //                    mTriggerAutoExposure = true;
    //                }

    //                RCLCPP_INFO(get_logger(), "The param '%s' has changed to %s", param.get_name().c_str(),
    //                            mZedAutoExpGain ? "ENABLED" : "DISABLED");
    //                result.successful = true;
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires a BOOL value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else if (param.get_name() == "video.exposure") {
    //            if (param.get_type() == rclcpp::PARAMETER_INTEGER) {

    //                int new_val = param.as_int();

    //                if (new_val > 0 && new_val <= 100) {
    //                    mZedExposure = new_val;
    //                    RCLCPP_INFO(get_logger(), "The param '%s' has changed to %d", param.get_name().c_str(), mZedExposure);
    //                    result.successful = true;
    //                } else {
    //                    RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value in the range ]0,100]", param.get_name().c_str());
    //                    result.successful = false;
    //                    return result;
    //                }
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else if (param.get_name() == "video.gain") {
    //            if (param.get_type() == rclcpp::PARAMETER_INTEGER) {

    //                int new_val = param.as_int();

    //                if (new_val > 0 && new_val <= 100) {
    //                    mZedGain = new_val;
    //                    RCLCPP_INFO(get_logger(), "The param '%s' has changed to %d", param.get_name().c_str(), mZedGain);
    //                    result.successful = true;
    //                } else {
    //                    RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value in the range ]0,100]", param.get_name().c_str());
    //                    result.successful = false;
    //                    return result;
    //                }
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else if (param.get_name() == "depth.confidence") {
    //            if (param.get_type() == rclcpp::PARAMETER_INTEGER) {
    //                int new_val = param.as_int();

    //                if (new_val > 0 && new_val <= 100) {
    //                    mDepthConf = new_val;
    //                    RCLCPP_INFO(get_logger(), "The param '%s' has changed to %d", param.get_name().c_str(), mDepthConf);
    //                    result.successful = true;
    //                } else {
    //                    RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value in the range ]0,100]", param.get_name().c_str());
    //                    result.successful = false;
    //                    return result;
    //                }
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires an INTEGER value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else if (param.get_name() == "depth.max_depth") {
    //            if (param.get_type() == rclcpp::PARAMETER_DOUBLE) {

    //                double new_val = param.as_double();

    //                if (new_val > 0) {
    //                    mZedMaxDepth = new_val;
    //                    RCLCPP_INFO(get_logger(), "The param '%s' has changed to %g", param.get_name().c_str(), mZedMaxDepth);
    //                    result.successful = true;
    //                } else {
    //                    RCLCPP_WARN(get_logger(), "The param '%s' requires a FLOATING POINT positive value", param.get_name().c_str());
    //                    result.successful = false;
    //                    return result;
    //                }
    //            } else {
    //                RCLCPP_WARN(get_logger(), "The param '%s' requires a FLOATING POINT positive value!", param.get_name().c_str());
    //                result.successful = false;
    //                return result;
    //            }
    //        } else {
    //            RCLCPP_WARN(get_logger(), "The param '%s' cannot be dinamically changed!", param.get_name().c_str());
    //            result.successful = false;
    //            return result;
    //        }
    //    }

    return result;
}



void ZedCamera::setTFCoordFrameNames()
{
    // ----> Coordinate frames
    mCameraFrameId = mCameraName + "_camera_center";
    mLeftCamFrameId = mCameraName + "_left_camera_frame";
    mLeftCamOptFrameId = mCameraName + "_left_camera_optical_frame";
    mRightCamFrameId = mCameraName + "_right_camera_frame";
    mRightCamOptFrameId = mCameraName + "_right_camera_optical_frame";

    mImuFrameId = mCameraName + "_imu_link";
    mBaroFrameId = mCameraName + "_baro_link";
    mMagFrameId = mCameraName + "_mag_link";
    mTempLeftFrameId = mCameraName + "_temp_left_link";
    mTempRightFrameId = mCameraName + "_temp_right_link";

    mDepthFrameId = mLeftCamFrameId;
    mDepthOptFrameId = mLeftCamOptFrameId;

    // Note: Depth image frame id must match color image frame id
    mCloudFrameId = mDepthOptFrameId;
    mRgbFrameId = mDepthFrameId;
    mRgbOptFrameId = mCloudFrameId;
    mDisparityFrameId = mDepthFrameId;
    mDisparityOptFrameId = mDepthOptFrameId;
    mConfidenceFrameId = mDepthFrameId;
    mConfidenceOptFrameId = mDepthOptFrameId;

    // Print TF frames
    RCLCPP_INFO_STREAM(get_logger(), "*** TF FRAMES ***");
    RCLCPP_INFO_STREAM(get_logger(), " * Map\t\t\t-> " << mMapFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Odometry\t\t\t-> " << mOdomFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Base\t\t\t-> " << mBaseFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Camera\t\t\t-> " << mCameraFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Left\t\t\t-> " << mLeftCamFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Left Optical\t\t-> " << mLeftCamOptFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * RGB\t\t\t-> " << mRgbFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * RGB Optical\t\t-> " << mRgbFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Right\t\t\t-> " << mRightCamFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Right Optical\t\t-> " << mRightCamOptFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Depth\t\t\t-> " << mDepthFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Depth Optical\t\t-> " << mDepthOptFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Point Cloud\t\t-> " << mCloudFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Disparity\t\t-> " << mDisparityFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Disparity Optical\t-> " << mDisparityOptFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Confidence\t\t-> " << mConfidenceFrameId);
    RCLCPP_INFO_STREAM(get_logger(), " * Confidence Optical\t-> " << mConfidenceOptFrameId);
    if (mCamRealModel!=sl::MODEL::ZED)
    {
        RCLCPP_INFO_STREAM(get_logger(), " * IMU\t\t\t-> " << mImuFrameId);

        if (mCamUserModel==sl::MODEL::ZED2)
        {
            RCLCPP_INFO_STREAM(get_logger(), " * Barometer\t\t-> " << mBaroFrameId);
            RCLCPP_INFO_STREAM(get_logger(), " * Magnetometer\t\t-> " << mMagFrameId);
            RCLCPP_INFO_STREAM(get_logger(), " * Left Temperature\t\t-> " << mTempLeftFrameId);
            RCLCPP_INFO_STREAM(get_logger(), " * Right Temperature\t-> " << mTempRightFrameId);
        }
    }
    // <---- Coordinate frames
}

void ZedCamera::fillCamInfo(sl::Camera& zed, std::shared_ptr<sensor_msgs::msg::CameraInfo> leftCamInfoMsg,
                            std::shared_ptr<sensor_msgs::msg::CameraInfo> rightCamInfoMsg,
                            std::string leftFrameId, std::string rightFrameId,
                            bool rawParam /*= false*/) {
    sl::CalibrationParameters zedParam;

#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
    if (rawParam) {
        zedParam = zed.getCameraInformation(mMatResolVideo).calibration_parameters_raw; // ok
    } else {
        zedParam = zed.getCameraInformation(mMatResolVideo).calibration_parameters; // ok
    }
#else
    if (rawParam) {
        zedParam = zed.getCameraInformation(mMatResolVideo).camera_configuration.calibration_parameters_raw;
    } else {
        zedParam = zed.getCameraInformation(mMatResolVideo).camera_configuration.calibration_parameters;
    }
#endif

    float baseline = zedParam.getCameraBaseline();
    leftCamInfoMsg->distortion_model =
            sensor_msgs::distortion_models::PLUMB_BOB;
    rightCamInfoMsg->distortion_model =
            sensor_msgs::distortion_models::PLUMB_BOB;
    leftCamInfoMsg->d.resize(5);
    rightCamInfoMsg->d.resize(5);
    leftCamInfoMsg->d[0] = zedParam.left_cam.disto[0];   // k1
    leftCamInfoMsg->d[1] = zedParam.left_cam.disto[1];   // k2
    leftCamInfoMsg->d[2] = zedParam.left_cam.disto[4];   // k3
    leftCamInfoMsg->d[3] = zedParam.left_cam.disto[2];   // p1
    leftCamInfoMsg->d[4] = zedParam.left_cam.disto[3];   // p2
    rightCamInfoMsg->d[0] = zedParam.right_cam.disto[0]; // k1
    rightCamInfoMsg->d[1] = zedParam.right_cam.disto[1]; // k2
    rightCamInfoMsg->d[2] = zedParam.right_cam.disto[4]; // k3
    rightCamInfoMsg->d[3] = zedParam.right_cam.disto[2]; // p1
    rightCamInfoMsg->d[4] = zedParam.right_cam.disto[3]; // p2
    leftCamInfoMsg->k.fill(0.0);
    rightCamInfoMsg->k.fill(0.0);
    leftCamInfoMsg->k[0] = static_cast<double>(zedParam.left_cam.fx);
    leftCamInfoMsg->k[2] = static_cast<double>(zedParam.left_cam.cx);
    leftCamInfoMsg->k[4] = static_cast<double>(zedParam.left_cam.fy);
    leftCamInfoMsg->k[5] = static_cast<double>(zedParam.left_cam.cy);
    leftCamInfoMsg->k[8] = 1.0;
    rightCamInfoMsg->k[0] = static_cast<double>(zedParam.right_cam.fx);
    rightCamInfoMsg->k[2] = static_cast<double>(zedParam.right_cam.cx);
    rightCamInfoMsg->k[4] = static_cast<double>(zedParam.right_cam.fy);
    rightCamInfoMsg->k[5] = static_cast<double>(zedParam.right_cam.cy);
    rightCamInfoMsg->k[8] = 1.0;
    leftCamInfoMsg->r.fill(0.0);
    rightCamInfoMsg->r.fill(0.0);

    for (size_t i = 0; i < 3; i++) {
        // identity
        rightCamInfoMsg->r[i + i * 3] = 1;
        leftCamInfoMsg->r[i + i * 3] = 1;
    }

#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
    if (rawParam) {
        std::vector<float> R_ = sl_tools::convertRodrigues(zedParam.R);
        float* p = R_.data();

        for (int i = 0; i < 9; i++) {
            rightCamInfoMsg->r[i] = p[i];
        }
    }
#else
    if (rawParam) {
        if (mUseOldExtrinsic) { // Camera frame (Z forward, Y down, X right)

            std::vector<float> R_ = sl_tools::convertRodrigues(zedParam.R);
            float* p = R_.data();

            for (int i = 0; i < 9; i++) {
                rightCamInfoMsg->r[i] = p[i];
            }
        } else { // ROS frame (X forward, Z up, Y left)
            for (int i = 0; i < 9; i++) {
                rightCamInfoMsg->r[i] = zedParam.stereo_transform.getRotationMatrix().r[i];
            }
        }
    }
#endif

    leftCamInfoMsg->p.fill(0.0);
    rightCamInfoMsg->p.fill(0.0);
    leftCamInfoMsg->p[0] = static_cast<double>(zedParam.left_cam.fx);
    leftCamInfoMsg->p[2] = static_cast<double>(zedParam.left_cam.cx);
    leftCamInfoMsg->p[5] = static_cast<double>(zedParam.left_cam.fy);
    leftCamInfoMsg->p[6] = static_cast<double>(zedParam.left_cam.cy);
    leftCamInfoMsg->p[10] = 1.0;
    // http://docs.ros.org/api/sensor_msgs/html/msg/CameraInfo.html
    rightCamInfoMsg->p[3] = static_cast<double>(-1 * zedParam.left_cam.fx * baseline);
    rightCamInfoMsg->p[0] = static_cast<double>(zedParam.right_cam.fx);
    rightCamInfoMsg->p[2] = static_cast<double>(zedParam.right_cam.cx);
    rightCamInfoMsg->p[5] = static_cast<double>(zedParam.right_cam.fy);
    rightCamInfoMsg->p[6] = static_cast<double>(zedParam.right_cam.cy);
    rightCamInfoMsg->p[10] = 1.0;
    leftCamInfoMsg->width = rightCamInfoMsg->width = static_cast<uint32_t>(mMatResolVideo.width);
    leftCamInfoMsg->height = rightCamInfoMsg->height = static_cast<uint32_t>(mMatResolVideo.height);
    leftCamInfoMsg->header.frame_id = leftFrameId;
    rightCamInfoMsg->header.frame_id = rightFrameId;
}

void ZedCamera::initPublishers() {
    RCLCPP_INFO(get_logger(), "*** PUBLISHED TOPICS ***");

    std::string topicPrefix = get_namespace();

    if (topicPrefix.length() > 1) {
        topicPrefix += "/";
    }

    topicPrefix += get_name();
    topicPrefix += "/";
    // ----> Topics names definition
    std::string rgbTopicRoot = "rgb";
    std::string rightTopicRoot = "right";
    std::string leftTopicRoot = "left";
    std::string stereoTopicRoot = "stereo";
    std::string img_topic = "/image_rect_color";
    std::string img_raw_topic = "/image_raw_color";
    std::string img_gray_topic = "/image_rect_gray";
    std::string img_raw_gray_topic_ = "/image_raw_gray";
    std::string raw_suffix = "_raw";
    std::string left_topic = mTopicRoot + leftTopicRoot + img_topic;
    std::string left_raw_topic = mTopicRoot + leftTopicRoot + raw_suffix + img_raw_topic;
    std::string right_topic = mTopicRoot + rightTopicRoot + img_topic;
    std::string right_raw_topic = mTopicRoot + rightTopicRoot + raw_suffix + img_raw_topic;
    std::string rgb_topic = mTopicRoot + rgbTopicRoot + img_topic;
    std::string rgb_raw_topic = mTopicRoot + rgbTopicRoot + raw_suffix + img_raw_topic;
    std::string stereo_topic = mTopicRoot + stereoTopicRoot + img_topic;
    std::string stereo_raw_topic = mTopicRoot + stereoTopicRoot + raw_suffix + img_raw_topic;
    std::string left_gray_topic = mTopicRoot + leftTopicRoot + img_gray_topic;
    std::string left_raw_gray_topic = mTopicRoot + leftTopicRoot + raw_suffix + img_raw_gray_topic_;
    std::string right_gray_topic = mTopicRoot + rightTopicRoot + img_gray_topic;
    std::string right_raw_gray_topic = mTopicRoot + rightTopicRoot + raw_suffix + img_raw_gray_topic_;
    std::string rgb_gray_topic = mTopicRoot + rgbTopicRoot + img_gray_topic;
    std::string rgb_raw_gray_topic = mTopicRoot + rgbTopicRoot + raw_suffix + img_raw_gray_topic_;

    // Set the disparity topic name
    std::string disparity_topic = mTopicRoot + "disparity/disparity_image";

    // Set the depth topic names
    std::string depth_topic_root = "depth";

    if (mOpenniDepthMode) {
        RCLCPP_INFO_STREAM( get_logger(), "Openni depth mode activated -> Units: mm, Encoding: MONO16");
    }
    std::string depth_topic = mTopicRoot + depth_topic_root + "/depth_registered";

    std::string pointcloud_topic = mTopicRoot + "point_cloud/cloud_registered";
    std::string pointcloud_fused_topic = mTopicRoot + "mapping/fused_cloud";

    std::string object_det_topic_root = "obj_det";
    std::string object_det_topic = mTopicRoot + object_det_topic_root + "/objects";
    std::string object_det_rviz_topic = mTopicRoot + object_det_topic_root + "/object_markers";

    std::string confImgRoot = "confidence";
    std::string conf_map_topic_name = "confidence_map";
    std::string conf_map_topic = mTopicRoot + confImgRoot + "/" + conf_map_topic_name;

    // Set the positional tracking topic names
    mPoseTopic = mTopicRoot + "pose";
    mPoseCovTopic = mPoseTopic + "_with_covariance";

    mOdomTopic = mTopicRoot + "odom";
    std::string odom_path_topic = mTopicRoot + "path_odom";
    std::string map_path_topic = mTopicRoot + "path_map";

    // Set the Sensors topic names
    std::string temp_topic_root = "temperature";
    std::string imuTopicRoot = "imu";
    std::string imu_topic_name = "data";
    std::string imu_topic_raw_name = "data_raw";
    std::string imu_topic_mag_name = "mag";
    //std::string imu_topic_mag_raw_name = "mag_raw";
    std::string pressure_topic_name = "atm_press";

    std::string imu_topic = mTopicRoot + imuTopicRoot + "/" + imu_topic_name;
    std::string imu_topic_raw = mTopicRoot + imuTopicRoot + "/" + imu_topic_raw_name;
    std::string imu_temp_topic = mTopicRoot + temp_topic_root + "/" + imuTopicRoot;
    std::string imu_mag_topic = mTopicRoot + imuTopicRoot + "/" + imu_topic_mag_name;
    //std::string imu_mag_topic_raw = imuTopicRoot + "/" + imu_topic_mag_raw_name;
    std::string pressure_topic = mTopicRoot + /*imuTopicRoot + "/" +*/ pressure_topic_name;
    std::string temp_topic_left = mTopicRoot + temp_topic_root + "/left";
    std::string temp_topic_right = mTopicRoot + temp_topic_root + "/right";
    // <---- Topics names definition

    // ----> Camera publishers
    mPubRgb = image_transport::create_camera_publisher( this, rgb_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRgb.getTopic());
    mPubRgbGray = image_transport::create_camera_publisher( this, rgb_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRgbGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRgb.getInfoTopic());
    mPubRawRgb = image_transport::create_camera_publisher( this, rgb_raw_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRgb.getTopic());
    mPubRawRgbGray = image_transport::create_camera_publisher( this, rgb_raw_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRgbGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRgb.getInfoTopic());
    mPubLeft = image_transport::create_camera_publisher( this, left_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubLeft.getTopic());
    mPubLeftGray = image_transport::create_camera_publisher( this, left_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubLeftGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubLeft.getInfoTopic());
    mPubRawLeft = image_transport::create_camera_publisher( this, left_raw_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawLeft.getTopic());
    mPubRawLeftGray = image_transport::create_camera_publisher( this, left_raw_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawLeftGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawLeft.getInfoTopic());
    mPubRight = image_transport::create_camera_publisher( this, right_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRight.getTopic());
    mPubRightGray = image_transport::create_camera_publisher( this, right_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRightGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRight.getInfoTopic());
    mPubRawRight = image_transport::create_camera_publisher( this, right_raw_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRight.getTopic());
    mPubRawRightGray = image_transport::create_camera_publisher( this, right_raw_gray_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRightGray.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawRight.getInfoTopic());

    mPubDepth = image_transport::create_camera_publisher( this, depth_topic, mDepthQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubDepth.getTopic());
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubDepth.getInfoTopic());

    mPubStereo = image_transport::create_publisher( this, stereo_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubStereo.getTopic());
    mPubRawStereo = image_transport::create_publisher( this, stereo_raw_topic, mVideoQos.get_rmw_qos_profile() );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubRawStereo.getTopic());// <---- Camera publishers

    // ----> Depth publishers
    mPubConfMap = create_publisher<sensor_msgs::msg::Image>(conf_map_topic, mDepthQos);
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubConfMap->get_topic_name());
    mPubDisparity = create_publisher<stereo_msgs::msg::DisparityImage>( disparity_topic, mDepthQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubDisparity->get_topic_name());
    mPubCloud = create_publisher<sensor_msgs::msg::PointCloud2>( pointcloud_topic, mDepthQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubCloud->get_topic_name());
    // <---- Depth publishers

    // ----> Pos Tracking
    mPubPose = create_publisher<geometry_msgs::msg::PoseStamped>( mPoseTopic, mPoseQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubPose->get_topic_name());
    mPubPoseCov = create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>( mPoseCovTopic, mPoseQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubPoseCov->get_topic_name());
    mPubOdom = create_publisher<nav_msgs::msg::Odometry>( mOdomTopic, mPoseQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubOdom->get_topic_name());
    mPubPosePath = create_publisher<nav_msgs::msg::Path>( map_path_topic, mPoseQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubPosePath->get_topic_name());
    mPubOdomPath = create_publisher<nav_msgs::msg::Path>( odom_path_topic, mPoseQos );
    RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubOdomPath->get_topic_name());
    // <---- Pos Tracking

    // ----> Mapping
    if (mMappingEnabled) {
        mPubFusedCloud = create_publisher<sensor_msgs::msg::PointCloud2>( pointcloud_fused_topic, mDepthQos );
        RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubFusedCloud->get_topic_name());
    }
    // <---- Mapping

    // ----> Object Detection
    // TODO SEE ROS1
    // <---- Object Detection

    // ----> Sensors
    if (mCamRealModel != sl::MODEL::ZED) {
        mPubImu = create_publisher<sensor_msgs::msg::Imu>( imu_topic, mSensQos );
        RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubImu->get_topic_name());
        mPubImuRaw = create_publisher<sensor_msgs::msg::Imu>( imu_topic_raw, mSensQos );
        RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubImuRaw->get_topic_name());
        mPubImuTemp = create_publisher<sensor_msgs::msg::Temperature>( imu_temp_topic, mSensQos );
        RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubImuTemp->get_topic_name());

        if (mCamRealModel == sl::MODEL::ZED2) {
            mPubImuMag = create_publisher<sensor_msgs::msg::MagneticField>( imu_mag_topic, mSensQos );
            RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubImuMag->get_topic_name());
            mPubPressure = create_publisher<sensor_msgs::msg::FluidPressure>( pressure_topic, mSensQos );
            RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubPressure->get_topic_name());
            mPubTempL = create_publisher<sensor_msgs::msg::Temperature>( temp_topic_left, mSensQos );
            RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubTempL->get_topic_name());
            mPubTempR = create_publisher<sensor_msgs::msg::Temperature>( temp_topic_right, mSensQos );
            RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubTempR->get_topic_name());
        }

        // ----> Publish latched camera/imu transform message
        publishStaticImuFrameAndTopic();
        // <---- Publish latched camera/imu transform message
    }
    // <---- Sensors
}

bool ZedCamera::startCamera() {
    RCLCPP_INFO( get_logger(), "***** STARTING CAMERA *****");

    // ----> Check SDK version
    RCLCPP_INFO(get_logger(), "SDK Version: %d.%d.%d - Build %s",
                ZED_SDK_MAJOR_VERSION, ZED_SDK_MINOR_VERSION, ZED_SDK_PATCH_VERSION, ZED_SDK_BUILD_ID);
#if (ZED_SDK_MAJOR_VERSION<3)
    RCLCPP_ERROR(get_logger(), "ROS2 ZED node requires at least ZED SDK v3");

    return false;
#endif
    // <---- Check SDK version

    // ----> TF2 Transform
    mTfBuffer = std::make_unique<tf2_ros::Buffer>(this->get_clock());
    mTfListener = std::make_shared<tf2_ros::TransformListener>(*mTfBuffer);
    mTfBroadcaster = std::make_shared<tf2_ros::TransformBroadcaster>(this);
    mStaticTfBroadcaster = std::make_shared<tf2_ros::StaticTransformBroadcaster>(this);
    // <---- TF2 Transform

    // ----> ZED configuration
    if (!mSvoFilepath.empty()) {
        RCLCPP_INFO(get_logger(), "*** SVO OPENING ***");

        mInitParams.input.setFromSVOFile(mSvoFilepath.c_str());
        mInitParams.svo_real_time_mode = true;
        mSvoMode = true;
    } else {
        RCLCPP_INFO(get_logger(), "*** CAMERA OPENING ***");

        mInitParams.camera_fps = mCamFrameRate;
        mInitParams.camera_resolution = static_cast<sl::RESOLUTION>(mCamResol);

        if (mCamSerialNumber == 0) {
            mInitParams.input.setFromCameraID(mCamId);
        } else {
            mInitParams.input.setFromSerialNumber(mCamSerialNumber);
        }
    }

    mInitParams.coordinate_system = sl::COORDINATE_SYSTEM::RIGHT_HANDED_Z_UP_X_FWD;
    mInitParams.coordinate_units = sl::UNIT::METER;
    mInitParams.depth_mode = mDepthQuality;
    mInitParams.sdk_verbose = mVerbose;
    mInitParams.sdk_gpu_id = mGpuId;
    mInitParams.depth_stabilization = static_cast<int>(mDepthStabilization);
    mInitParams.camera_image_flip = mCameraFlip;
    mInitParams.depth_minimum_distance = mCamMinDepth;
    mInitParams.depth_maximum_distance = mCamMaxDepth;

    mInitParams.camera_disable_self_calib = !mCameraSelfCalib;
    mInitParams.enable_image_enhancement = true;
    mInitParams.enable_right_side_measure = false;
    // <---- ZED configuration

    // ----> Try to open ZED camera or to load SVO
    INIT_TIMER;
    START_TIMER;

    mThreadStop = false;

    if (!mSvoMode) {
        if (mCamSerialNumber == 0) {
            mInitParams.input.setFromCameraID(mCamId);
        } else {
            bool waiting_for_camera = true;

            while (waiting_for_camera) {
                // Ctrl+C check
                if (!rclcpp::ok()) {
                    return false;
                }

                sl::DeviceProperties prop = sl_tools::getZEDFromSN(mCamSerialNumber);

                if (prop.id < -1 || prop.camera_state == sl::CAMERA_STATE::NOT_AVAILABLE) {
                    std::string msg = "Camera with SN " + std::to_string(mCamSerialNumber) +
                            " not detected! Please verify the connection.";
                    RCLCPP_INFO(get_logger(), msg.c_str());
                } else {
                    waiting_for_camera = false;
                    mInitParams.input.setFromCameraID(prop.id);
                }

                TIMER_ELAPSED; // Initialize a variable named "elapsed" with the msec elapsed from the latest "START_TIMER" call

                if (elapsed >= mMaxReconnectTemp * mCamTimeoutSec * 1000) {
                    RCLCPP_ERROR(get_logger(), "Camera detection timeout");

                    return false;
                }

                std::this_thread::sleep_for(std::chrono::seconds(mCamTimeoutSec));
            }
        }
    }

    while (1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        mConnStatus = mZed.open(mInitParams);

        if (mConnStatus == sl::ERROR_CODE::SUCCESS) {
            RCLCPP_DEBUG(get_logger(), "Opening successfull");
            break;
        }

        if (mSvoMode) {
            RCLCPP_WARN(get_logger(), "Error opening SVO: %s", sl::toString(mConnStatus).c_str());

            return false;
        }

        RCLCPP_WARN(get_logger(), "Error opening camera: %s", sl::toString(mConnStatus).c_str());

        if (mConnStatus == sl::ERROR_CODE::CAMERA_DETECTION_ISSUE && mCamUserModel == sl::MODEL::ZED_M) {
            RCLCPP_INFO(get_logger(), "Try to flip the USB3 Type-C connector");
        } else {
            RCLCPP_INFO(get_logger(), "Please verify the USB3 connection");
        }

        if (!rclcpp::ok() || mThreadStop) {
            RCLCPP_INFO(get_logger(), "ZED activation interrupted");

            return false;
        }

        TIMER_ELAPSED;

        if (elapsed > mMaxReconnectTemp * mCamTimeoutSec * 1000) {
            RCLCPP_ERROR(get_logger(), "Camera detection timeout");

            return false;
        }

        std::this_thread::sleep_for(std::chrono::seconds(mCamTimeoutSec));
    }
    // <---- Try to open ZED camera or to load SVO

    // ----> Camera information
    //CUdevice devid;
    cuCtxGetDevice(&mGpuId);
    RCLCPP_INFO_STREAM( get_logger(), "ZED SDK running on GPU #" << mGpuId);

    sl::CameraInformation camInfo = mZed.getCameraInformation();

    // Camera model
    mCamRealModel = camInfo.camera_model;

    if (mCamRealModel == sl::MODEL::ZED) {
        if (mCamUserModel != sl::MODEL::ZED) {
            RCLCPP_WARN(get_logger(), "Camera model does not match user parameter. Please modify "
                                      "the value of the parameter 'general.camera_model' to 'zed'");
        }
    } else if (mCamRealModel == sl::MODEL::ZED_M) {
        if (mCamUserModel != sl::MODEL::ZED_M) {
            RCLCPP_WARN(get_logger(), "Camera model does not match user parameter. Please modify "
                                      "the value of the parameter 'general.camera_model' to 'zedm'");
        }
    } else if (mCamRealModel == sl::MODEL::ZED2) {
        if (mCamUserModel != sl::MODEL::ZED2) {
            RCLCPP_WARN(get_logger(), "Camera model does not match user parameter. Please modify "
                                      "the value of the parameter 'general.camera_model' to 'zed2'");
        }
    }

    RCLCPP_INFO_STREAM(get_logger(), " * Camera Model\t-> " << sl::toString(mCamRealModel).c_str());
    mCamSerialNumber = camInfo.serial_number;
    RCLCPP_INFO_STREAM(get_logger(), " * Serial Number\t-> " << mCamSerialNumber);

    RCLCPP_INFO_STREAM( get_logger(), " * Input type\t-> " << sl::toString(mZed.getCameraInformation().input_type).c_str());
    if (mSvoMode) {
        RCLCPP_INFO(get_logger(), " * SVO resolution\t-> %dx%d",
                    mZed.getCameraInformation().camera_configuration.resolution.width,
                    mZed.getCameraInformation().camera_configuration.resolution.height);
        RCLCPP_INFO_STREAM(get_logger(), " * SVO framerate\t-> " << (mZed.getCameraInformation().camera_configuration.fps) );
    }

    // Firmwares
    if (!mSvoMode) {
#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
        mCamFwVersion = camInfo.camera_firmware_version;
#else
        mCamFwVersion = camInfo.camera_configuration.firmware_version;
#endif

        RCLCPP_INFO_STREAM(get_logger()," * Camera FW Version -> " << mCamFwVersion);
        if (mCamRealModel!=sl::MODEL::ZED) {
#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
            mSensFwVersion = camInfo.sensors_firmware_version;
#else
            mSensFwVersion = camInfo.sensors_configuration.firmware_version;
#endif
            RCLCPP_INFO_STREAM(get_logger()," * Sensors FW Version -> " << mSensFwVersion);
        }
    }

    // Camera/IMU transform
    if (mCamRealModel != sl::MODEL::ZED) {
#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
        mSlCamImuTransf = camInfo.camera_imu_transform;
#else
        mSlCamImuTransf = camInfo.sensors_configuration.camera_imu_transform;
#endif

        RCLCPP_INFO( get_logger(), "Camera-IMU Transform: \n %s", mSlCamImuTransf.getInfos().c_str() );
    }

    mCamWidth = camInfo.camera_configuration.resolution.width;
    mCamHeight = camInfo.camera_configuration.resolution.height;

    RCLCPP_DEBUG_STREAM( get_logger(), "Camera Frame size : " << mCamWidth << "x" << mCamHeight);
    int v_w = static_cast<int>(mCamWidth * mImgDownsampleFactor);
    int v_h = static_cast<int>(mCamHeight * mImgDownsampleFactor);
    mMatResolVideo = sl::Resolution(v_w,v_h);
    RCLCPP_DEBUG_STREAM( get_logger(),"Image Mat size : " << mMatResolVideo.width << "x" << mMatResolVideo.height);
    int d_w = static_cast<int>(mCamWidth * mDepthDownsampleFactor);
    int d_h = static_cast<int>(mCamHeight * mDepthDownsampleFactor);
    mMatResolDepth = sl::Resolution(d_w,d_h);
    RCLCPP_DEBUG_STREAM( get_logger(),"Depth Mat size : " << mMatResolDepth.width << "x" << mMatResolDepth.height);
    // <---- Camera information

    // ----> Camera Info messages
    mRgbCamInfoMsg = std::make_shared<sensor_msgs::msg::CameraInfo>(); // TODO ARE THOSE MESSAGES USED???
    mRgbCamInfoRawMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();
    mLeftCamInfoMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();
    mLeftCamInfoRawMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();
    mRightCamInfoMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();
    mRightCamInfoRawMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();
    mDepthCamInfoMsg = std::make_shared<sensor_msgs::msg::CameraInfo>();

    fillCamInfo(mZed, mLeftCamInfoMsg, mRightCamInfoMsg, mLeftCamOptFrameId, mRightCamOptFrameId);
    fillCamInfo(mZed, mLeftCamInfoRawMsg, mRightCamInfoRawMsg, mLeftCamOptFrameId, mRightCamOptFrameId, true);
    mRgbCamInfoMsg = mLeftCamInfoMsg;
    mRgbCamInfoRawMsg = mLeftCamInfoRawMsg;
    mDepthCamInfoMsg = mLeftCamInfoMsg;
    // <---- Camera Info messages

    setTFCoordFrameNames(); // Requires mZedRealCamModel available only after camera opening
    initPublishers(); // Requires mZedRealCamModel available only after camera opening

    // Disable AEC_AGC and Auto Whitebalance to trigger it if user set it to automatic
    mZed.setCameraSettings(sl::VIDEO_SETTINGS::AEC_AGC, 0);
    mZed.setCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_AUTO, 0);
    // Force parameters with a dummy grab
    mZed.grab();

    // TODO INITIALIZE SERVICES

    // ----> Initialize Diagnostic statistics
    mElabPeriodMean_sec = std::make_unique<sl_tools::SmartMean>(mCamFrameRate);
    mGrabPeriodMean_usec = std::make_unique<sl_tools::SmartMean>(mCamFrameRate);
    mVideoDepthPeriodMean_sec = std::make_unique<sl_tools::SmartMean>(mCamFrameRate);
    mPcPeriodMean_usec = std::make_unique<sl_tools::SmartMean>(mCamFrameRate);
    mObjDetPeriodMean_msec = std::make_unique<sl_tools::SmartMean>(mCamFrameRate);
    mImuPeriodMean_usec = std::make_unique<sl_tools::SmartMean>(mSensPubRate);
    mBaroPeriodMean_usec = std::make_unique<sl_tools::SmartMean>(mSensPubRate);
    mMagPeriodMean_usec = std::make_unique<sl_tools::SmartMean>(mSensPubRate);
    // <---- Initialize Diagnostic statistics

    // ----> Start Pointcloud thread
    mPcDataReady = false;
    //RCLCPP_DEBUG(get_logger(), "on_activate -> mPcDataReady FALSE")
    mPcThread = std::thread(&ZedCamera::threadFunc_pointcloudElab, this);
    // <---- Start Pointcloud thread

    // Start pool thread
    mGrabThread = std::thread(&ZedCamera::threadFunc_zedGrab, this);

    // Start data publishing timer
    std::chrono::milliseconds videoDepthPubPeriod_msec(static_cast<int>(1000.0 / (mPubFrameRate)));
    mVideoDepthTimer = create_wall_timer(
                std::chrono::duration_cast<std::chrono::milliseconds>(videoDepthPubPeriod_msec),
                std::bind(&ZedCamera::callback_pubVideoDepth, this) );

    if (mCamRealModel != sl::MODEL::ZED) {
        mFrameTimestamp = now(); // Initialialized timestamp to avoid wrong sensors data

        std::chrono::milliseconds sensorsPubPeriod_msec(static_cast<int>(1000.0 / (mSensPubRate*1.5)));
        mSensTimer = create_wall_timer(
                    std::chrono::duration_cast<std::chrono::milliseconds>(sensorsPubPeriod_msec),
                    std::bind(&ZedCamera::callback_pubSensorsData, this) );
    }

    return true;
}

bool ZedCamera::startPosTracking() {
    RCLCPP_INFO_STREAM(get_logger(),"*** Starting Positional Tracking ***");

    RCLCPP_INFO(get_logger()," * Waiting for valid static transformations...");

    bool transformOk = false;
    double elapsed = 0.0;
    mPosTrackingReady=false;

    auto start = std::chrono::high_resolution_clock::now();

    do {
        transformOk = set_pose(mInitialBasePose[0], mInitialBasePose[1], mInitialBasePose[2],
                mInitialBasePose[3], mInitialBasePose[4], mInitialBasePose[5]);

        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() -
                                                                        start).count();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (elapsed > 10000) {
            RCLCPP_WARN(get_logger()," !!! Failed to get static transforms. Is the 'ROBOT STATE PUBLISHER' node correctly working? ");
            break;
        }

    } while (transformOk == false);

    if (transformOk) {
        RCLCPP_DEBUG(get_logger(),"Time required to get valid static transforms: %g sec", elapsed / 1000.);
    }

    RCLCPP_INFO(get_logger(),"Initial ZED left camera pose (ZED pos. tracking): ");
    RCLCPP_INFO(get_logger()," * T: [%g,%g,%g]",
                mInitialPoseSl.getTranslation().x, mInitialPoseSl.getTranslation().y, mInitialPoseSl.getTranslation().z);
    RCLCPP_INFO(get_logger()," * Q: [%g,%g,%g,%g]",
                mInitialPoseSl.getOrientation().ox, mInitialPoseSl.getOrientation().oy,
                mInitialPoseSl.getOrientation().oz, mInitialPoseSl.getOrientation().ow);

    if (mAreaMemoryDbPath != "" && !sl_tools::file_exist(mAreaMemoryDbPath)) {
        mAreaMemoryDbPath = "";
        RCLCPP_WARN_STREAM(get_logger(),"'area_memory_db_path' path doesn't exist or is unreachable: " << mAreaMemoryDbPath);
    }

    // Tracking parameters
    sl::PositionalTrackingParameters trackParams;

    trackParams.area_file_path = mAreaMemoryDbPath.c_str();

    mPoseSmoothing = false; // Always false. Pose Smoothing is to be enabled only for VR/AR applications
    trackParams.enable_pose_smoothing = mPoseSmoothing;

    trackParams.enable_area_memory = mAreaMemory;
    trackParams.enable_imu_fusion = mImuFusion;
    trackParams.initial_world_transform = mInitialPoseSl;

    trackParams.set_floor_as_origin = mFloorAlignment;

    sl::ERROR_CODE err = mZed.enablePositionalTracking(trackParams);

    if (err == sl::ERROR_CODE::SUCCESS) {
        mPosTrackingEnabled = true;
    } else {
        mPosTrackingEnabled = false;

        RCLCPP_WARN(get_logger(),"Tracking not activated: %s", sl::toString(err).c_str());
    }
}

void ZedCamera::initTransforms() {
    // According to REP 105 -> http://www.ros.org/reps/rep-0105.html

    // base_link <- odom <- map
    //     ^                 |
    //     |                 |
    //     -------------------

    // ----> Dynamic transforms
    mOdom2BaseTransf.setIdentity();     // broadcasted if `publish_tf` is true
    mMap2OdomTransf.setIdentity();      // broadcasted if `publish_map_tf` is true
    mMap2BaseTransf.setIdentity();      // used internally, but not broadcasted
    // <---- Dynamic transforms
}

bool ZedCamera::getCamera2BaseTransform() {
    RCLCPP_DEBUG(get_logger(),"Getting static TF from '%s' to '%s'", mCameraFrameId.c_str(), mBaseFrameId.c_str());

    mCamera2BaseTransfValid = false;
    static bool first_error = true;


    // ----> Static transforms
    // Sensor to Base link
    try {
        // Save the transformation
        geometry_msgs::msg::TransformStamped c2b =
                mTfBuffer->lookupTransform(mCameraFrameId, mBaseFrameId, rclcpp::Time(0,0,RCL_ROS_TIME), rclcpp::Duration(0.1));

        // Get the TF2 transformation
        //tf2::fromMsg(c2b.transform, mCamera2BaseTransf);
        geometry_msgs::msg::Transform in = c2b.transform;
        mCamera2BaseTransf.setOrigin(tf2::Vector3(in.translation.x, in.translation.y, in.translation.z));
        // w at the end in the constructor
        mCamera2BaseTransf.setRotation(tf2::Quaternion(in.rotation.x, in.rotation.y, in.rotation.z, in.rotation.w));

        double roll, pitch, yaw;
        tf2::Matrix3x3(mCamera2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

        RCLCPP_INFO(get_logger(),"Static transform Camera Center to Base [%s -> %s]",
                    mCameraFrameId.c_str(), mBaseFrameId.c_str());
        RCLCPP_INFO(get_logger()," * Translation: {%.3f,%.3f,%.3f}",
                    mCamera2BaseTransf.getOrigin().x(), mCamera2BaseTransf.getOrigin().y(), mCamera2BaseTransf.getOrigin().z());
        RCLCPP_INFO(get_logger()," * Rotation: {%.3f,%.3f,%.3f}",
                    roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);

    } catch (tf2::TransformException& ex) {
        if (!first_error) {
            rclcpp::Clock steady_clock(RCL_STEADY_TIME);
            RCLCPP_DEBUG_THROTTLE(get_logger(),steady_clock,1.0, "Transform error: %s", ex.what());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "The tf from '%s' to '%s' is not available.",
                                 mCameraFrameId.c_str(), mBaseFrameId.c_str());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "Note: one of the possible cause of the problem is the absense of an instance "
                                                                "of the `robot_state_publisher` node publishing the correct static TF transformations "
                                                                "or a modified URDF not correctly reproducing the ZED "
                                                                "TF chain '%s' -> '%s' -> '%s'",
                                 mBaseFrameId.c_str(), mCameraFrameId.c_str(),mDepthFrameId.c_str());
            first_error=false;
        }

        mCamera2BaseTransf.setIdentity();
        return false;
    }

    // <---- Static transforms
    mCamera2BaseTransfValid = true;
    return true;
}

bool ZedCamera::getSens2CameraTransform() {
    RCLCPP_DEBUG(get_logger(),"Getting static TF from '%s' to '%s'", mDepthFrameId.c_str(), mCameraFrameId.c_str());

    mSensor2CameraTransfValid = false;

    static bool first_error = true;

    // ----> Static transforms
    // Sensor to Camera Center
    try {
        // Save the transformation
        geometry_msgs::msg::TransformStamped s2c =
                mTfBuffer->lookupTransform(mDepthFrameId, mCameraFrameId, rclcpp::Time(0,0,RCL_ROS_TIME), rclcpp::Duration(0.1));

        // Get the TF2 transformation
        //tf2::fromMsg(s2c.transform, mSensor2CameraTransf);
        geometry_msgs::msg::Transform in = s2c.transform;
        mSensor2CameraTransf.setOrigin(tf2::Vector3(in.translation.x, in.translation.y, in.translation.z));
        // w at the end in the constructor
        mSensor2CameraTransf.setRotation(tf2::Quaternion(in.rotation.x, in.rotation.y, in.rotation.z, in.rotation.w));


        double roll, pitch, yaw;
        tf2::Matrix3x3(mSensor2CameraTransf.getRotation()).getRPY(roll, pitch, yaw);

        RCLCPP_INFO(get_logger(),"Static transform Sensor to Camera Center [%s -> %s]",
                    mDepthFrameId.c_str(), mCameraFrameId.c_str());
        RCLCPP_INFO(get_logger()," * Translation: {%.3f,%.3f,%.3f}",
                    mSensor2CameraTransf.getOrigin().x(), mSensor2CameraTransf.getOrigin().y(), mSensor2CameraTransf.getOrigin().z());
        RCLCPP_INFO(get_logger()," * Rotation: {%.3f,%.3f,%.3f}",
                    roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);
    } catch (tf2::TransformException& ex) {
        if (!first_error) {
            rclcpp::Clock steady_clock(RCL_STEADY_TIME);
            RCLCPP_DEBUG_THROTTLE(get_logger(),steady_clock,1.0, "Transform error: %s", ex.what());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "The tf from '%s' to '%s' is not available.",
                                 mDepthFrameId.c_str(), mCameraFrameId.c_str());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "Note: one of the possible cause of the problem is the absense of an instance "
                                                                "of the `robot_state_publisher` node publishing the correct static TF transformations "
                                                                "or a modified URDF not correctly reproducing the ZED "
                                                                "TF chain '%s' -> '%s' -> '%s'",
                                 mBaseFrameId.c_str(), mCameraFrameId.c_str(),mDepthFrameId.c_str());
            first_error = false;
        }

        mSensor2CameraTransf.setIdentity();
        return false;
    }

    // <---- Static transforms

    mSensor2CameraTransfValid = true;
    return true;
}

bool ZedCamera::getSens2BaseTransform() {
    RCLCPP_DEBUG(get_logger(),"Getting static TF from '%s' to '%s'", mDepthFrameId.c_str(), mBaseFrameId.c_str());

    mSensor2BaseTransfValid = false;
    static bool first_error = true;

    // ----> Static transforms
    // Sensor to Base link
    try {
        // Save the transformation
        geometry_msgs::msg::TransformStamped s2b =
                mTfBuffer->lookupTransform(mDepthFrameId, mBaseFrameId, rclcpp::Time(0,0,RCL_ROS_TIME), rclcpp::Duration(0.1));

        // Get the TF2 transformation
        //tf2::fromMsg(s2b.transform, mSensor2BaseTransf);
        geometry_msgs::msg::Transform in = s2b.transform;
        mSensor2BaseTransf.setOrigin(tf2::Vector3(in.translation.x, in.translation.y, in.translation.z));
        // w at the end in the constructor
        mSensor2BaseTransf.setRotation(tf2::Quaternion(in.rotation.x, in.rotation.y, in.rotation.z, in.rotation.w));

        double roll, pitch, yaw;
        tf2::Matrix3x3(mSensor2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

        RCLCPP_INFO(get_logger(),"Static transform Sensor to Base [%s -> %s]",
                    mDepthFrameId.c_str(), mBaseFrameId.c_str());
        RCLCPP_INFO(get_logger()," * Translation: {%.3f,%.3f,%.3f}",
                    mSensor2BaseTransf.getOrigin().x(), mSensor2BaseTransf.getOrigin().y(), mSensor2BaseTransf.getOrigin().z());
        RCLCPP_INFO(get_logger()," * Rotation: {%.3f,%.3f,%.3f}",
                    roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);

    } catch (tf2::TransformException& ex) {
        if (!first_error) {
            rclcpp::Clock steady_clock(RCL_STEADY_TIME);
            RCLCPP_DEBUG_THROTTLE(get_logger(),steady_clock,1.0, "Transform error: %s", ex.what());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "The tf from '%s' to '%s' is not available.",
                                 mDepthFrameId.c_str(), mBaseFrameId.c_str());
            RCLCPP_WARN_THROTTLE(get_logger(),steady_clock,1.0, "Note: one of the possible cause of the problem is the absense of an instance "
                                                                "of the `robot_state_publisher` node publishing the correct static TF transformations "
                                                                "or a modified URDF not correctly reproducing the ZED "
                                                                "TF chain '%s' -> '%s' -> '%s'",
                                 mBaseFrameId.c_str(), mCameraFrameId.c_str(),mDepthFrameId.c_str());
            first_error = false;
        }

        mSensor2BaseTransf.setIdentity();
        return false;
    }

    // <---- Static transforms

    mSensor2BaseTransfValid = true;
    return true;
}

bool ZedCamera::set_pose(float xt, float yt, float zt, float rr, float pr, float yr) {
    initTransforms();

    if (!mSensor2BaseTransfValid) {
        getSens2BaseTransform();
    }

    if (!mSensor2CameraTransfValid) {
        getSens2CameraTransform();
    }

    if (!mCamera2BaseTransfValid) {
        getCamera2BaseTransform();
    }

    // Apply Base to sensor transform
    tf2::Transform initPose;
    tf2::Vector3 origin(xt, yt, zt);
    initPose.setOrigin(origin);
    tf2::Quaternion quat;
    quat.setRPY(rr, pr, yr);
    initPose.setRotation(quat);

    initPose = initPose * mSensor2BaseTransf.inverse();

    // SL pose
    sl::float3 t_vec;
    t_vec[0] = initPose.getOrigin().x();
    t_vec[1] = initPose.getOrigin().y();
    t_vec[2] = initPose.getOrigin().z();

    sl::float4 q_vec;
    q_vec[0] = initPose.getRotation().x();
    q_vec[1] = initPose.getRotation().y();
    q_vec[2] = initPose.getRotation().z();
    q_vec[3] = initPose.getRotation().w();

    sl::Translation trasl(t_vec);
    sl::Orientation orient(q_vec);
    mInitialPoseSl.setTranslation(trasl);
    mInitialPoseSl.setOrientation(orient);

    return (mSensor2BaseTransfValid & mSensor2CameraTransfValid & mCamera2BaseTransfValid);

}

void ZedCamera::publishStaticImuFrameAndTopic() {
    sl::Orientation sl_rot = mSlCamImuTransf.getOrientation();
    sl::Translation sl_tr = mSlCamImuTransf.getTranslation();

    mCameraImuTransfMgs = std::make_shared<geometry_msgs::msg::TransformStamped>();

    mCameraImuTransfMgs->header.stamp = now();
    mCameraImuTransfMgs->header.frame_id = mLeftCamFrameId;
    mCameraImuTransfMgs->child_frame_id = mImuFrameId;

    mCameraImuTransfMgs->transform.rotation.x = sl_rot.ox;
    mCameraImuTransfMgs->transform.rotation.y = sl_rot.oy;
    mCameraImuTransfMgs->transform.rotation.z = sl_rot.oz;
    mCameraImuTransfMgs->transform.rotation.w = sl_rot.ow;

    mCameraImuTransfMgs->transform.translation.x = sl_tr.x;
    mCameraImuTransfMgs->transform.translation.y = sl_tr.y;
    mCameraImuTransfMgs->transform.translation.z = sl_tr.z;

    if (!mStaticImuTopicPublished)
    {
        rclcpp::QoS transf_qos = mSensQos;
        transf_qos.durability(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL); // Latched topic
        transf_qos.keep_last(1);
        std::string cam_imu_tr_topic = mTopicRoot + "left_cam_imu_transform";
        mPubCamImuTransf = create_publisher<geometry_msgs::msg::TransformStamped>(cam_imu_tr_topic, transf_qos);

        mPubCamImuTransf->publish( *(mCameraImuTransfMgs.get()) );

        RCLCPP_INFO_STREAM( get_logger(), "Advertised on topic: " << mPubCamImuTransf->get_topic_name() << " [LATCHED]");
        RCLCPP_INFO( get_logger(), "Camera-IMU Translation: \n %g %g %g", sl_tr.x, sl_tr.y, sl_tr.z );
        RCLCPP_INFO( get_logger(), "Camera-IMU Rotation: \n %s", sl_rot.getRotationMatrix().getInfos().c_str() );

        mStaticImuTopicPublished=true;
    }

    // Publish IMU TF as static TF
    if ( !mPublishImuTF ) {
        return;
    }

    if (mStaticImuFramePublished) {
        return;
    }

    // Publish transformation
    mStaticTfBroadcaster->sendTransform(*(mCameraImuTransfMgs.get()));

    RCLCPP_INFO_STREAM(get_logger(), "Published static TF: '" << mImuFrameId << "' -> '" << mLeftCamFrameId << "'" );

    mStaticImuFramePublished = true;
}

void ZedCamera::threadFunc_zedGrab() {
    RCLCPP_DEBUG(get_logger(), "Grab thread started");

    mFrameCount=0;

    // ----> Grab Runtime parameters
    mRunParams.sensing_mode = static_cast<sl::SENSING_MODE>(mDepthSensingMode);
    mRunParams.enable_depth = false;
    mRunParams.measure3D_reference_frame = sl::REFERENCE_FRAME::CAMERA;
    // <---- Grab Runtime parameters

    // Infinite grab thread
    while(1) {
        // ----> Interruption check
        if (!rclcpp::ok()) {
            RCLCPP_INFO(get_logger(), "Ctrl+C received");
            break;
        }

        if (mThreadStop) {
            RCLCPP_INFO(get_logger(), "Grab thread stopped");
            break;
        }
        // <---- Interruption check

        // ----> Apply depth settings
        applyDepthSettings();
        // <---- Apply depth settings

        // ----> Apply video dynamic parameters
        applyVideoSettings();
        // <---- Apply video dynamic parameters

        // ----> Check for Positional Tracking requirement
        if ( isPosTrackingRequired() && !mPosTrackingEnabled ) {
            startPosTracking();
        }
        // ----> Check for Positional Tracking requirement

        // ZED grab
        mGrabStatus = mZed.grab(mRunParams);

        // ----> Check SVO status
        if ( mSvoMode && mGrabStatus==sl::ERROR_CODE::END_OF_SVOFILE_REACHED )
        {
            if ( mSvoLoop ) {
                mZed.setSVOPosition(0);
                RCLCPP_WARN(get_logger(), "SVO reached the end and has been restarted.");
            } else {
                RCLCPP_WARN(get_logger(), "SVO reached the end. The node has been stopped.");
                break;
            }
        }
        // <---- Check SVO status

        // ----> Grab errors?
        // Note: disconnection are automatically handled by the ZED SDK
        if(mGrabStatus!=sl::ERROR_CODE::SUCCESS) {
            RCLCPP_ERROR_STREAM( get_logger(), "Camera error: " << sl::toString(mGrabStatus).c_str());
            break; // TODO verify what to do in case of grab errors
        }
        // <---- Grab errors?

        mFrameCount++;

        // ----> Timestamp
        if (mSvoMode) {
            mFrameTimestamp = sl_tools::slTime2Ros(mZed.getTimestamp(sl::TIME_REFERENCE::CURRENT));
        } else {
            mFrameTimestamp = sl_tools::slTime2Ros(mZed.getTimestamp(sl::TIME_REFERENCE::IMAGE));
        }
        // <---- Timestamp

        // ----> Grab freq calculation
        static std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        double elapsed_usec = std::chrono::duration_cast<std::chrono::microseconds>(now - last_time).count();
        last_time = now;

        mGrabPeriodMean_usec->addValue(elapsed_usec);

        //        RCLCPP_INFO_STREAM( get_logger(), "Grab period: " << mGrabPeriodMean_usec->getMean()/1e6 <<
        //                            " Freq: " << 1e6/mGrabPeriodMean_usec->getMean() );
        // <---- Grab freq calculation

        // ----> Check recording status
        mRecMutex.lock();
        if (mRecording) {
            mRecStatus = mZed.getRecordingStatus().status;

            if (!mRecStatus) {
                rclcpp::Clock steady_clock(RCL_STEADY_TIME);
                RCLCPP_ERROR_THROTTLE(get_logger(), steady_clock, 1.0, "Error saving frame to SVO");
            }

            // TODO update disgnostic
            //mDiagUpdater.force_update();
        }
        mRecMutex.unlock();
        // <---- Check recording status

        if (mPosTrackingEnabled) {
            processOdometry();
            processPose();

            if(mCamRealModel == sl::MODEL::ZED || !mPublishImuTF) {
                publishTFs(mFrameTimestamp);
            }
        }

        // Update previous frame timestamp
        mPrevFrameTimestamp = mFrameTimestamp;
    }

    RCLCPP_DEBUG(get_logger(), "Grab thread finished");
}

void ZedCamera::publishTFs(rclcpp::Time t) {
    if(t==rclcpp::Time(0,0,RCL_ROS_TIME)) {
        return;
    }

    // Publish pose tf only if enabled
    if(mPublishTF) {
        publishOdomTF(t); // publish the base Frame in odometry frame

        if(mPublishMapTF) {
            publishPoseTF(t); // publish the odometry Frame in map frame
        }

        if(mPublishImuTF && !mStaticImuFramePublished )
        {
            publishStaticImuFrameAndTopic();
        }
    }
}

void ZedCamera::publishOdomTF(rclcpp::Time t) {
    if (!mSensor2BaseTransfValid) {
        getSens2BaseTransform();
    }

    if (!mSensor2CameraTransfValid) {
        getSens2CameraTransform();
    }

    if (!mCamera2BaseTransfValid) {
        getCamera2BaseTransform();
    }

    transfMsgPtr transformStamped = std::make_shared<geometry_msgs::msg::TransformStamped>();

    transformStamped->header.stamp = t;
    transformStamped->header.frame_id = mOdomFrameId;
    transformStamped->child_frame_id = mBaseFrameId;
    // conversion from Tranform to message
    tf2::Vector3 translation = mOdom2BaseTransf.getOrigin();
    tf2::Quaternion quat = mOdom2BaseTransf.getRotation();
    transformStamped->transform.translation.x = translation.x();
    transformStamped->transform.translation.y = translation.y();
    transformStamped->transform.translation.z = translation.z();
    transformStamped->transform.rotation.x = quat.x();
    transformStamped->transform.rotation.y = quat.y();
    transformStamped->transform.rotation.z = quat.z();
    transformStamped->transform.rotation.w = quat.w();

    // Publish transformation
    mTfBroadcaster->sendTransform(*(transformStamped.get()));
}

void ZedCamera::publishPoseTF(rclcpp::Time t) {
    if (!mSensor2BaseTransfValid) {
        getSens2BaseTransform();
    }

    if (!mSensor2CameraTransfValid) {
        getSens2CameraTransform();
    }

    if (!mCamera2BaseTransfValid) {
        getCamera2BaseTransform();
    }

    transfMsgPtr transformStamped = std::make_shared<geometry_msgs::msg::TransformStamped>();

    transformStamped->header.stamp = t;
    transformStamped->header.frame_id = mMapFrameId;
    transformStamped->child_frame_id = mOdomFrameId;
    // conversion from Tranform to message
    tf2::Vector3 translation = mMap2OdomTransf.getOrigin();
    tf2::Quaternion quat = mMap2OdomTransf.getRotation();
    transformStamped->transform.translation.x = translation.x();
    transformStamped->transform.translation.y = translation.y();
    transformStamped->transform.translation.z = translation.z();
    transformStamped->transform.rotation.x = quat.x();
    transformStamped->transform.rotation.y = quat.y();
    transformStamped->transform.rotation.z = quat.z();
    transformStamped->transform.rotation.w = quat.w();

    // Publish transformation
    mTfBroadcaster->sendTransform(*(transformStamped.get()));
}

void ZedCamera::threadFunc_pointcloudElab() {
    std::unique_lock<std::mutex> lock(mPcMutex);

    RCLCPP_DEBUG(get_logger(), "Point Cloud thread started");

    while (1) {
        if (!rclcpp::ok()) {
            RCLCPP_INFO(get_logger(), "Ctrl+C received");
            break;
        }

        if (mThreadStop) {
            RCLCPP_INFO(get_logger(), "Point Cloud thread stopped");
            break;
        }

        //RCLCPP_DEBUG(get_logger(), "pointcloudThreadFunc -> mPcDataReady value: %s", mPcDataReady ? "TRUE" : "FALSE");

        while (!mPcDataReady) { // loop to avoid spurious wakeups
            if (mPcDataReadyCondVar.wait_for(lock, std::chrono::milliseconds(500)) == std::cv_status::timeout) {
                // Check thread stopping
                if (mThreadStop) {
                    RCLCPP_INFO(get_logger(), "Point Cloud thread stopped");
                    break;
                } else {
                    //RCLCPP_DEBUG(get_logger(), "pointcloudThreadFunc -> WAIT FOR CLOUD DATA");
                    continue;
                }
            }
        }

        publishPointCloud();

        // ----> Check publishing frequency
        double pc_period_msec = 1000.0 / mPcPubRate;

        static std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        double elapsed_msec = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();



        if (elapsed_msec < pc_period_msec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long int>(pc_period_msec - elapsed_msec)));
        }

        last_time = std::chrono::steady_clock::now();
        // <---- Check publishing frequency

        mPcDataReady = false;
        //RCLCPP_DEBUG(get_logger(), "pointcloudThreadFunc -> mPcDataReady FALSE")
    }

    RCLCPP_DEBUG(get_logger(), "Pointcloud thread finished");
}

void ZedCamera::callback_pubVideoDepth() {
    RCLCPP_DEBUG_ONCE(get_logger(), "Video Depth callback called");

    static rclcpp::Time prev_ts=rclcpp::Time(0,0,RCL_ROS_TIME);

    std::lock_guard<std::mutex> lock(mCloseZedMutex);
    mPublishingData = false;

    rclcpp::Time ros_ts = mFrameTimestamp; // Fix timestamp for images and depth to avoid async transmissions

    mPublishingData |= publishImages(ros_ts);
    mPublishingData |= publishDepthData(ros_ts);

    if (prev_ts!=rclcpp::Time(0,0,RCL_ROS_TIME)) {
        double mean = mVideoDepthPeriodMean_sec->addValue( (ros_ts-prev_ts).seconds() );
        //RCLCPP_INFO_STREAM( get_logger(),"Pub freq: " << 1.0/mean );
    }
    prev_ts = ros_ts;
}

void ZedCamera::callback_pubSensorsData() {
    RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback called");

    static rclcpp::Time prev_ts=rclcpp::Time(0,0,RCL_ROS_TIME);

    std::lock_guard<std::mutex> lock(mCloseZedMutex);
    rclcpp::Time ros_ts = mFrameTimestamp; // Fix timestamp for images and depth to avoid async transmissions

    if (!mZed.isOpened()) {
        return;
    }

    // ----> Subscribers count
    RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: counting subscribers");

    size_t imu_SubNumber = 0;
    size_t imu_RawSubNumber = 0;
    size_t imu_TempSubNumber = 0;
    size_t imu_MagSubNumber = 0;
    size_t pressSubNumber = 0;
    size_t tempLeftSubNumber = 0;
    size_t tempRightSubNumber = 0;

    try {
        imu_SubNumber = count_subscribers(mPubImu->get_topic_name());
        imu_RawSubNumber = count_subscribers(mPubImuRaw->get_topic_name());
        imu_TempSubNumber = 0;
        imu_MagSubNumber = 0;
        pressSubNumber = 0;
        tempLeftSubNumber = 0;
        tempRightSubNumber = 0;

        if( mCamRealModel == sl::MODEL::ZED2 ) {
            imu_TempSubNumber = count_subscribers(mPubImuTemp->get_topic_name());
            imu_MagSubNumber = count_subscribers(mPubImuMag->get_topic_name());
            //imu_MagRawSubNumber = count_subscribers(mPubImuMagRaw->get_topic_name());
            pressSubNumber = count_subscribers(mPubPressure->get_topic_name());
            tempLeftSubNumber = count_subscribers(mPubTempL->get_topic_name());
            tempRightSubNumber = count_subscribers(mPubTempR->get_topic_name());
        }
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return;
    }
    // <---- Subscribers count

    int totSub = imu_SubNumber + imu_RawSubNumber + imu_TempSubNumber + imu_MagSubNumber + /*imu_MagRawSubNumber +*/
            pressSubNumber + tempLeftSubNumber + tempRightSubNumber;

    // ----> Grab data and setup timestamps
    RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: Grab data and setup timestamps");
    rclcpp::Time ts_imu;
    rclcpp::Time ts_baro;
    rclcpp::Time ts_mag;
    //ros::Time ts_mag_raw;

    static rclcpp::Time lastTs_imu = now();
    static rclcpp::Time lastTs_baro = now();
    static rclcpp::Time lastTs_mag = now();
    //static ros::Time lastT_mag_raw = ros::Time();

    sl::SensorsData sens_data;

    if(mSvoMode) {
        if( mZed.getSensorsData(sens_data, sl::TIME_REFERENCE::IMAGE) != sl::ERROR_CODE::SUCCESS ) {
            return;
        }

        ts_imu = now();
        ts_baro = now();
        ts_mag = now();
        //ts_mag_raw = now();
    } else {
        if ( mSensTimestampSync) {
            if( mZed.getSensorsData(sens_data, sl::TIME_REFERENCE::IMAGE) != sl::ERROR_CODE::SUCCESS ) {
                return;
            }
        } else {
            if( mZed.getSensorsData(sens_data, sl::TIME_REFERENCE::CURRENT) != sl::ERROR_CODE::SUCCESS ) {
                return;
            }
        }
    }
    ts_imu = sl_tools::slTime2Ros(sens_data.imu.timestamp);
    ts_baro = sl_tools::slTime2Ros(sens_data.barometer.timestamp);
    ts_mag = sl_tools::slTime2Ros(sens_data.magnetometer.timestamp);
    //ts_mag_raw = sl_tools::slTime2Ros(sens_data.magnetometer.timestamp);
    // <---- Grab data and setup timestamps

    // ----> Check for duplicated data
    bool new_imu_data = ts_imu!=lastTs_imu;
    lastTs_imu = ts_imu;
    bool new_baro_data = ts_baro!=lastTs_baro;
    lastTs_baro = ts_baro;
    bool new_mag_data = ts_mag!=lastTs_mag;
    lastTs_mag = ts_mag;


    if( !new_imu_data && !new_baro_data && !new_mag_data) {
        return;
    }
    // <---- Check for duplicated data

    RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: temperatures");

    if( mCamRealModel == sl::MODEL::ZED2 ) {
        // Update temperatures for Diagnostic
        sens_data.temperature.get( sl::SensorsData::TemperatureData::SENSOR_LOCATION::ONBOARD_LEFT, mTempLeft);
        sens_data.temperature.get( sl::SensorsData::TemperatureData::SENSOR_LOCATION::ONBOARD_RIGHT, mTempRight);
    }

    // ----> Sensors freq for diagnostic
    if( new_imu_data) {
        RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: IMU FREQ");
        static std::chrono::steady_clock::time_point imu_last_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        double elapsed_usec = std::chrono::duration_cast<std::chrono::microseconds>(now - imu_last_time).count();
        imu_last_time = now;

        double mean = mImuPeriodMean_usec->addValue(elapsed_usec);
        //RCLCPP_INFO_STREAM(get_logger(), "IMU freq: " << 1e6/mean);
    }

    if( new_baro_data) {
        RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: BARO FREQ");
        static std::chrono::steady_clock::time_point baro_last_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        double elapsed_usec = std::chrono::duration_cast<std::chrono::microseconds>(now - baro_last_time).count();
        baro_last_time = now;

        double mean = mBaroPeriodMean_usec->addValue(elapsed_usec);
        //RCLCPP_INFO_STREAM(get_logger(), "Barometer freq: " << 1e6/mean);
    }

    if( new_mag_data) {
        RCLCPP_DEBUG_ONCE(get_logger(), "Sensors callback: MAG FREQ");
        static std::chrono::steady_clock::time_point mag_last_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        double elapsed_usec = std::chrono::duration_cast<std::chrono::microseconds>(now - mag_last_time).count();
        mag_last_time = now;

        double mean = mMagPeriodMean_usec->addValue(elapsed_usec);
        //RCLCPP_INFO_STREAM(get_logger(), "Magnetometer freq: " << 1e6/mean);
    }
    // <---- Sensors freq for diagnostic

    if(!totSub<0 && !mPublishImuTF) {
        return; // Nothing to publish
    }

    // ----> Sensors data publishing
    if(new_imu_data) {
        if(imu_SubNumber>0) {
            imuMsgPtr imuMsg = std::make_unique<sensor_msgs::msg::Imu>();

            imuMsg->header.stamp = ts_imu;
            imuMsg->header.frame_id = mImuFrameId;

            imuMsg->orientation.x = sens_data.imu.pose.getOrientation()[0];
            imuMsg->orientation.y = sens_data.imu.pose.getOrientation()[1];
            imuMsg->orientation.z = sens_data.imu.pose.getOrientation()[2];
            imuMsg->orientation.w = sens_data.imu.pose.getOrientation()[3];

            imuMsg->angular_velocity.x = sens_data.imu.angular_velocity[0] * DEG2RAD;
            imuMsg->angular_velocity.y = sens_data.imu.angular_velocity[1] * DEG2RAD;
            imuMsg->angular_velocity.z = sens_data.imu.angular_velocity[2] * DEG2RAD;

            imuMsg->linear_acceleration.x = sens_data.imu.linear_acceleration[0];
            imuMsg->linear_acceleration.y = sens_data.imu.linear_acceleration[1];
            imuMsg->linear_acceleration.z = sens_data.imu.linear_acceleration[2];

            // ----> Covariances copy
            // Note: memcpy not allowed because ROS2 uses double and ZED SDK uses float
            for (int i = 0; i < 3; ++i) {
                int r = 0;

                if (i == 0) {
                    r = 0;
                } else if (i == 1) {
                    r = 1;
                } else {
                    r = 2;
                }

                imuMsg->orientation_covariance[i * 3 + 0] =
                        sens_data.imu.pose_covariance.r[r * 3 + 0] * DEG2RAD * DEG2RAD;
                imuMsg->orientation_covariance[i * 3 + 1] =
                        sens_data.imu.pose_covariance.r[r * 3 + 1] * DEG2RAD * DEG2RAD;
                imuMsg->orientation_covariance[i * 3 + 2] =
                        sens_data.imu.pose_covariance.r[r * 3 + 2] * DEG2RAD * DEG2RAD;

                imuMsg->linear_acceleration_covariance[i * 3 + 0] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 0];
                imuMsg->linear_acceleration_covariance[i * 3 + 1] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 1];
                imuMsg->linear_acceleration_covariance[i * 3 + 2] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 2];

                imuMsg->angular_velocity_covariance[i * 3 + 0] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 0] * DEG2RAD * DEG2RAD;
                imuMsg->angular_velocity_covariance[i * 3 + 1] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 1] * DEG2RAD * DEG2RAD;
                imuMsg->angular_velocity_covariance[i * 3 + 2] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 2] * DEG2RAD * DEG2RAD;
            }
            // <---- Covariances copy

            mPubImu->publish(std::move(imuMsg));
        }

        if(imu_RawSubNumber>0) {
            imuMsgPtr imuRawMsg = std::make_unique<sensor_msgs::msg::Imu>();

            imuRawMsg->header.stamp = ts_imu;
            imuRawMsg->header.frame_id = mImuFrameId;

            imuRawMsg->angular_velocity.x = sens_data.imu.angular_velocity[0] * DEG2RAD;
            imuRawMsg->angular_velocity.y = sens_data.imu.angular_velocity[1] * DEG2RAD;
            imuRawMsg->angular_velocity.z = sens_data.imu.angular_velocity[2] * DEG2RAD;

            imuRawMsg->linear_acceleration.x = sens_data.imu.linear_acceleration[0];
            imuRawMsg->linear_acceleration.y = sens_data.imu.linear_acceleration[1];
            imuRawMsg->linear_acceleration.z = sens_data.imu.linear_acceleration[2];

            // ----> Covariances copy
            // Note: memcpy not allowed because ROS2 uses double and ZED SDK uses float
            for (int i = 0; i < 3; ++i) {
                int r = 0;

                if (i == 0) {
                    r = 0;
                } else if (i == 1) {
                    r = 1;
                } else {
                    r = 2;
                }

                imuRawMsg->linear_acceleration_covariance[i * 3 + 0] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 0];
                imuRawMsg->linear_acceleration_covariance[i * 3 + 1] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 1];
                imuRawMsg->linear_acceleration_covariance[i * 3 + 2] =
                        sens_data.imu.linear_acceleration_covariance.r[r * 3 + 2];

                imuRawMsg->angular_velocity_covariance[i * 3 + 0] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 0] * DEG2RAD * DEG2RAD;
                imuRawMsg->angular_velocity_covariance[i * 3 + 1] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 1] * DEG2RAD * DEG2RAD;
                imuRawMsg->angular_velocity_covariance[i * 3 + 2] =
                        sens_data.imu.angular_velocity_covariance.r[r * 3 + 2] * DEG2RAD * DEG2RAD;
            }
            // <---- Covariances copy

            mPubImuRaw->publish(std::move(imuRawMsg));
        }

        if (imu_TempSubNumber>0) {
            tempMsgPtr imuTempMsg = std::make_unique<sensor_msgs::msg::Temperature>();

            imuTempMsg->header.stamp = ts_imu;

            imuTempMsg->header.frame_id = mImuFrameId;
            float imu_temp;
            sens_data.temperature.get( sl::SensorsData::TemperatureData::SENSOR_LOCATION::IMU, imu_temp);
            imuTempMsg->temperature = static_cast<double>(imu_temp);
            imuTempMsg->variance = 0.0;

            mPubImuTemp->publish(std::move(imuTempMsg));
        }
    }

    if( sens_data.barometer.is_available && new_baro_data ) {
        if(pressSubNumber>0) {
            if( pressSubNumber>0 ) {
                pressMsgPtr pressMsg = std::make_unique<sensor_msgs::msg::FluidPressure>();

                pressMsg->header.stamp = ts_baro;
                pressMsg->header.frame_id = mBaroFrameId;
                pressMsg->fluid_pressure = sens_data.barometer.pressure * 1e-2; // Pascal
                pressMsg->variance = 1.0585e-2;

                mPubPressure->publish(std::move(pressMsg));
            }
        }

        if (tempLeftSubNumber>0) {
            tempMsgPtr leftTempMsg = std::make_unique<sensor_msgs::msg::Temperature>();

            leftTempMsg->header.stamp = ts_baro;

            leftTempMsg->header.frame_id = mLeftCamFrameId;
            leftTempMsg->temperature = static_cast<double>(mTempLeft);
            leftTempMsg->variance = 0.0;

            mPubTempL->publish(std::move(leftTempMsg));
        }

        if (tempRightSubNumber>0) {
            tempMsgPtr rightTempMsg = std::make_unique<sensor_msgs::msg::Temperature>();

            rightTempMsg->header.stamp = ts_baro;

            rightTempMsg->header.frame_id = mRightCamFrameId;
            rightTempMsg->temperature = static_cast<double>(mTempRight);
            rightTempMsg->variance = 0.0;

            mPubTempR->publish(std::move(rightTempMsg));
        }
    }

    if( sens_data.magnetometer.is_available && new_mag_data ) {
        if( imu_MagSubNumber>0) {
            magMsgPtr magMsg = std::make_unique<sensor_msgs::msg::MagneticField>();

            magMsg->header.stamp = ts_mag;
            magMsg->header.frame_id = mMagFrameId;
            magMsg->magnetic_field.x = sens_data.magnetometer.magnetic_field_calibrated.x*1e-6; // Tesla
            magMsg->magnetic_field.y = sens_data.magnetometer.magnetic_field_calibrated.y*1e-6; // Tesla
            magMsg->magnetic_field.z = sens_data.magnetometer.magnetic_field_calibrated.z*1e-6; // Tesla
            magMsg->magnetic_field_covariance[0] = 0.039e-6;
            magMsg->magnetic_field_covariance[1] = 0.0f;
            magMsg->magnetic_field_covariance[2] = 0.0f;
            magMsg->magnetic_field_covariance[3] = 0.0f;
            magMsg->magnetic_field_covariance[4] = 0.037e-6;
            magMsg->magnetic_field_covariance[5] = 0.0f;
            magMsg->magnetic_field_covariance[6] = 0.0f;
            magMsg->magnetic_field_covariance[7] = 0.0f;
            magMsg->magnetic_field_covariance[8] = 0.047e-6;

            mPubImuMag->publish(std::move(magMsg));
        }
    }
    // <---- Sensors data publishing


    // Publish TF at the same frequency of IMU data, so they are always synchronized
    if(new_imu_data && mPublishImuTF) {
        publishTFs(ts_imu);
    }
}

bool ZedCamera::publishImages(rclcpp::Time timeStamp) {
    static sl::Timestamp lastTs_img;

    size_t rgbSubnumber = 0;
    size_t rgbRawSubnumber = 0;
    size_t rgbGraySubnumber = 0;
    size_t rgbRawGraySubnumber = 0;
    size_t leftSubnumber = 0;
    size_t leftRawSubnumber = 0;
    size_t leftGraySubnumber = 0;
    size_t leftRawGraySubnumber = 0;
    size_t rightSubnumber = 0;
    size_t rightRawSubnumber = 0;
    size_t rightGraySubNumber = 0;
    size_t rightRawGraySubNumber = 0;
    size_t stereoSubnumber = 0;
    size_t stereoRawSubnumber = 0;

    try {
        rgbSubnumber = count_subscribers(mPubRgb.getTopic());
        rgbRawSubnumber = count_subscribers(mPubRawRgb.getTopic());
        rgbGraySubnumber = count_subscribers(mPubRgbGray.getTopic());
        rgbRawGraySubnumber = count_subscribers(mPubRawRgbGray.getTopic());
        leftSubnumber = count_subscribers(mPubLeft.getTopic());
        leftRawSubnumber = count_subscribers(mPubRawLeft.getTopic());
        leftGraySubnumber = count_subscribers(mPubLeftGray.getTopic());
        leftRawGraySubnumber = count_subscribers(mPubRawLeftGray.getTopic());
        rightSubnumber = count_subscribers(mPubRight.getTopic());
        rightRawSubnumber = count_subscribers(mPubRawRight.getTopic());
        rightGraySubNumber = count_subscribers(mPubRightGray.getTopic());
        rightRawGraySubNumber = count_subscribers(mPubRawRightGray.getTopic());
        stereoSubnumber = count_subscribers(mPubStereo.getTopic());
        stereoRawSubnumber = count_subscribers(mPubRawStereo.getTopic());
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return false;
    }

    size_t tot_sub = rgbSubnumber+rgbRawSubnumber+
            rgbGraySubnumber+rgbRawGraySubnumber+
            leftSubnumber+leftRawSubnumber+
            leftGraySubnumber+leftRawGraySubnumber+
            rightSubnumber+rightRawSubnumber+
            rightGraySubNumber+rightRawGraySubNumber+
            stereoSubnumber+stereoRawSubnumber;

    if (tot_sub<1) {
        return false;
    }

    sl::Mat slMat;

    // ----> Update img timestamp
    sl::Timestamp img_ts;
    mZed.retrieveImage(slMat, sl::VIEW::LEFT, sl::MEM::CPU, mMatResolVideo);
    img_ts = slMat.timestamp;

    if (img_ts==lastTs_img) {
        // Image not updated
        return true;
    }
    //    RCLCPP_INFO_STREAM( get_logger(),"Pub freq: " << 1e9/static_cast<double>(img_ts-lastTs_img) );
    lastTs_img=img_ts;
    // <---- Update img timestamp

    // ----> Publish the left == rgb image if someone has subscribed to
    if (leftSubnumber > 0 || rgbSubnumber > 0) {
        if (leftSubnumber > 0) {
            publishImageWithInfo( slMat, mPubLeft, mLeftCamInfoMsg, mLeftCamOptFrameId, timeStamp);
        }

        if (rgbSubnumber > 0) {
            publishImageWithInfo(slMat, mPubRgb, mRgbCamInfoMsg, mDepthOptFrameId, timeStamp);
        }
    }
    // <---- Publish the left == rgb image if someone has subscribed to

    // ----> Publish the left_raw == rgb_raw image if someone has subscribed to
    if (leftRawSubnumber > 0 || rgbRawSubnumber > 0) {
        // Retrieve RGBA Left image
        mZed.retrieveImage(slMat, sl::VIEW::LEFT_UNRECTIFIED, sl::MEM::CPU, mMatResolVideo);

        if (leftRawSubnumber > 0) {
            publishImageWithInfo(slMat, mPubRawLeft, mLeftCamInfoRawMsg, mLeftCamOptFrameId, timeStamp);
        }

        if (rgbRawSubnumber > 0) {
            publishImageWithInfo(slMat, mPubRawRgb, mRgbCamInfoRawMsg, mDepthOptFrameId, timeStamp);
        }
    }
    // <---- Publish the left_raw == rgb_raw image if someone has subscribed to

    // ----> Publish the left_gray == rgb_gray image if someone has subscribed to
    if (leftGraySubnumber > 0 || rgbGraySubnumber > 0) {
        // Retrieve RGBA Left image
        mZed.retrieveImage(slMat, sl::VIEW::LEFT_GRAY, sl::MEM::CPU, mMatResolVideo);

        if (leftGraySubnumber > 0) {
            publishImageWithInfo( slMat, mPubLeftGray, mLeftCamInfoMsg, mLeftCamOptFrameId, timeStamp);
        }

        if (rgbGraySubnumber > 0) {
            publishImageWithInfo(slMat, mPubRgbGray, mRgbCamInfoMsg, mDepthOptFrameId, timeStamp);
        }
    }
    // <---- Publish the left_raw == rgb_raw image if someone has subscribed to

    // ----> Publish the left_raw_gray == rgb_raw_gray image if someone has subscribed to
    if (leftRawGraySubnumber > 0 || rgbRawGraySubnumber > 0) {
        // Retrieve RGBA Left image
        mZed.retrieveImage(slMat, sl::VIEW::LEFT_UNRECTIFIED_GRAY, sl::MEM::CPU, mMatResolVideo);

        if (leftRawGraySubnumber > 0) {
            publishImageWithInfo( slMat, mPubRawLeftGray, mLeftCamInfoRawMsg, mLeftCamOptFrameId, timeStamp);
        }

        if (rgbRawGraySubnumber > 0) {
            publishImageWithInfo(slMat, mPubRawRgbGray, mRgbCamInfoRawMsg, mDepthOptFrameId, timeStamp);
        }
    }
    // ----> Publish the left_raw_gray == rgb_raw_gray image if someone has subscribed to

    // ----> Publish the right image if someone has subscribed to
    if (rightSubnumber > 0) {
        // Retrieve RGBA Right image
        mZed.retrieveImage(slMat, sl::VIEW::RIGHT, sl::MEM::CPU, mMatResolVideo);

        publishImageWithInfo( slMat, mPubRight, mRightCamInfoMsg, mRightCamOptFrameId, timeStamp);
    }
    // <---- Publish the right image if someone has subscribed to

    // ----> Publish the right raw image if someone has subscribed to
    if (rightRawSubnumber > 0) {
        // Retrieve RGBA Right image
        mZed.retrieveImage(slMat, sl::VIEW::RIGHT_UNRECTIFIED, sl::MEM::CPU, mMatResolVideo);

        publishImageWithInfo( slMat, mPubRawRight, mRightCamInfoRawMsg, mRightCamOptFrameId, timeStamp);
    }
    // <---- Publish the right raw image if someone has subscribed to

    // ----> Publish the right gray image if someone has subscribed to
    if (rightGraySubNumber > 0) {
        // Retrieve RGBA Right image
        mZed.retrieveImage(slMat, sl::VIEW::RIGHT_GRAY, sl::MEM::CPU, mMatResolVideo);

        publishImageWithInfo( slMat, mPubRightGray, mRightCamInfoMsg, mRightCamOptFrameId, timeStamp);
    }
    // <---- Publish the right gray image if someone has subscribed to

    // ----> Publish the right raw gray image if someone has subscribed to
    if (rightRawGraySubNumber > 0) {
        // Retrieve RGBA Right image
        mZed.retrieveImage(slMat, sl::VIEW::RIGHT_UNRECTIFIED_GRAY, sl::MEM::CPU, mMatResolVideo);

        publishImageWithInfo( slMat, mPubRawRightGray, mRightCamInfoRawMsg, mRightCamOptFrameId, timeStamp);
    }
    // <---- Publish the right raw gray image if someone has subscribed to

    sl::Mat left;
    sl::Mat right;

    // ----> Publish the side-by-side image if someone has subscribed to
    if (stereoSubnumber > 0) {
        // Retrieve RGBA RECTIFIED images
        mZed.retrieveImage(left, sl::VIEW::LEFT, sl::MEM::CPU, mMatResolVideo);
        mZed.retrieveImage(right, sl::VIEW::RIGHT, sl::MEM::CPU, mMatResolVideo);

        auto combined = sl_tools::imagesToROSmsg( left, right, mCameraFrameId, timeStamp );
        mPubStereo.publish(combined);
    }
    // <---- Publish the side-by-side image if someone has subscribed to

    // ----> Publish the side-by-side image if someone has subscribed to
    if (stereoRawSubnumber > 0) {

        // Retrieve RGBA UNRECTIFIED images
        mZed.retrieveImage(left, sl::VIEW::LEFT_UNRECTIFIED, sl::MEM::CPU, mMatResolVideo);
        mZed.retrieveImage(right, sl::VIEW::RIGHT_UNRECTIFIED, sl::MEM::CPU, mMatResolVideo);

        auto combined = sl_tools::imagesToROSmsg( left, right, mCameraFrameId, timeStamp );
        mPubRawStereo.publish(combined);
    }
    // <---- Publish the side-by-side image if someone has subscribed to

    return true;
}

void ZedCamera::publishImageWithInfo(sl::Mat& img,
                                     image_transport::CameraPublisher& pubImg,
                                     camInfoMsgPtr& camInfoMsg,
                                     std::string imgFrameId, rclcpp::Time t) {
    auto image = sl_tools::imageToROSmsg(img, imgFrameId, t);
    camInfoMsg->header.stamp = t;
    pubImg.publish(image, camInfoMsg); // TODO CHECK FOR ZERO-COPY

}

void ZedCamera::processOdometry() {
    rclcpp::Clock steady_clock(RCL_STEADY_TIME);

    if (!mSensor2BaseTransfValid) {
        getSens2BaseTransform();
    }

    if (!mSensor2CameraTransfValid) {
        getSens2CameraTransform();
    }

    if (!mCamera2BaseTransfValid) {
        getCamera2BaseTransform();
    }

    if (!mInitOdomWithPose) {
        sl::Pose deltaOdom;

        mPosTrackingStatus = mZed.getPosition(deltaOdom, sl::REFERENCE_FRAME::CAMERA);

        sl::Translation translation = deltaOdom.getTranslation();
        sl::Orientation quat = deltaOdom.getOrientation();

#if 0
        RCLCPP_DEBUG(get_logger(), "delta ODOM [%s] - %.2f,%.2f,%.2f %.2f,%.2f,%.2f,%.2f",
                     sl::toString(mPosTrackingStatus).c_str(),
                     translation(0), translation(1), translation(2),
                     quat(0), quat(1), quat(2), quat(3));
#endif

        if (mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::OK ||
                mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::SEARCHING ||
                mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::FPS_TOO_LOW ) {

            // Transform ZED delta odom pose in TF2 Transformation
            tf2::Transform deltaOdomTf;
            deltaOdomTf.setOrigin(tf2::Vector3(translation(0), translation(1), translation(2)));
            // w at the end in the constructor
            deltaOdomTf.setRotation(tf2::Quaternion(quat(0), quat(1), quat(2), quat(3)));


            // delta odom from sensor to base frame
            tf2::Transform deltaOdomTf_base =
                    mSensor2BaseTransf.inverse() * deltaOdomTf * mSensor2BaseTransf;

            // Propagate Odom transform in time
            mOdom2BaseTransf = mOdom2BaseTransf * deltaOdomTf_base;

            if (mTwoDMode) {
                tf2::Vector3 tr_2d = mOdom2BaseTransf.getOrigin();
                tr_2d.setZ(mFixedZValue);
                mOdom2BaseTransf.setOrigin(tr_2d);

                double roll, pitch, yaw;
                tf2::Matrix3x3(mOdom2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

                tf2::Quaternion quat_2d;
                quat_2d.setRPY(0.0, 0.0, yaw);

                mOdom2BaseTransf.setRotation(quat_2d);
            }

#if 0
            double roll, pitch, yaw;
            tf2::Matrix3x3(mOdom2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

            RCLCPP_DEBUG(get_logger(), "+++ Odometry [%s -> %s] - {%.3f,%.3f,%.3f} {%.3f,%.3f,%.3f}",
                         mOdomFrameId.c_str(), mBaseFrameId.c_str(),
                         mOdom2BaseTransf.getOrigin().x(),
                         mOdom2BaseTransf.getOrigin().y(),
                         mOdom2BaseTransf.getOrigin().z(),
                         roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);
#endif

            // Publish odometry message
            publishOdom(mOdom2BaseTransf, deltaOdom, mFrameTimestamp);
            mPosTrackingReady = true;
        }
    } else if (mFloorAlignment) {
        RCLCPP_DEBUG_THROTTLE(get_logger(),steady_clock,5.0,
                              "Odometry will be published as soon as the floor as been detected for the first time");
    }
}

void ZedCamera::publishOdom(tf2::Transform& odom2baseTransf, sl::Pose& slPose, rclcpp::Time t) {
    odomMsgPtr odomMsg = std::make_unique<nav_msgs::msg::Odometry>();

    odomMsg->header.stamp = t;
    odomMsg->header.frame_id = mOdomFrameId; // frame
    odomMsg->child_frame_id = mBaseFrameId;      // camera_frame

    // Add all value in odometry message
    odomMsg->pose.pose.position.x = odom2baseTransf.getOrigin().x();
    odomMsg->pose.pose.position.y = odom2baseTransf.getOrigin().y();
    odomMsg->pose.pose.position.z = odom2baseTransf.getOrigin().z();
    odomMsg->pose.pose.orientation.x = odom2baseTransf.getRotation().x();
    odomMsg->pose.pose.orientation.y = odom2baseTransf.getRotation().y();
    odomMsg->pose.pose.orientation.z = odom2baseTransf.getRotation().z();
    odomMsg->pose.pose.orientation.w = odom2baseTransf.getRotation().w();

    // Odometry pose covariance
    for (size_t i = 0; i < odomMsg->pose.covariance.size(); i++) {
        odomMsg->pose.covariance[i] = static_cast<double>(slPose.pose_covariance[i]);

        if (mTwoDMode) {
            if (i == 14 || i == 21 || i == 28) {
                odomMsg->pose.covariance[i] = 1e-9;    // Very low covariance if 2D mode
            } else if ((i >= 2 && i <= 4) ||
                       (i >= 8 && i <= 10) ||
                       (i >= 12 && i <= 13) ||
                       (i >= 15 && i <= 16) ||
                       (i >= 18 && i <= 20) ||
                       (i == 22) ||
                       (i >= 24 && i <= 27)) {
                odomMsg->pose.covariance[i] = 0.0;
            }
        }
    }

    // Publish odometry message
    mPubOdom->publish(std::move(odomMsg));
}

void ZedCamera::processPose() {
    if (!mSensor2BaseTransfValid) {
        getSens2BaseTransform();
    }

    if (!mSensor2CameraTransfValid) {
        getSens2CameraTransform();
    }

    if (!mCamera2BaseTransfValid) {
        getCamera2BaseTransform();
    }

    size_t odomSub=0;
    try {
        odomSub = count_subscribers(mOdomTopic);         // mPubOdom subscribers
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return;
    }

    static sl::POSITIONAL_TRACKING_STATE oldStatus;
    mPosTrackingStatus = mZed.getPosition(mLastZedPose, sl::REFERENCE_FRAME::WORLD);

    sl::Translation translation = mLastZedPose.getTranslation();
    sl::Orientation quat = mLastZedPose.getOrientation();

#if 0 // Enable for TF checking
    double roll, pitch, yaw;
    tf2::Matrix3x3(tf2::Quaternion(quat.ox, quat.oy, quat.oz, quat.ow)).getRPY(roll, pitch, yaw);

    RCLCPP_DEBUG(get_logger(), "Sensor POSE [%s -> %s] - {%.2f,%.2f,%.2f} {%.2f,%.2f,%.2f}",
                 mLeftCamFrameId.c_str(), mMapFrameId.c_str(),
                 translation.x, translation.y, translation.z,
                 roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);

    RCLCPP_DEBUG(get_logger(), "MAP -> Tracking Status: %s", sl::toString(mTrackingStatus).c_str());
#endif

    if (mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::OK ||
            mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::SEARCHING ) {

        tf2::Transform map_to_sens_transf;
        map_to_sens_transf.setOrigin(tf2::Vector3(translation(0),translation(1),translation(2)));
        map_to_sens_transf.setRotation(tf2::Quaternion(quat(0),quat(1),quat(2),quat(3)));

        mMap2BaseTransf = map_to_sens_transf * mSensor2BaseTransf; // Base position in map frame

        if (mTwoDMode) {
            tf2::Vector3 tr_2d = mMap2BaseTransf.getOrigin();
            tr_2d.setZ(mFixedZValue);
            mMap2BaseTransf.setOrigin(tr_2d);

            double roll, pitch, yaw;
            tf2::Matrix3x3(mMap2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

            tf2::Quaternion quat_2d;
            quat_2d.setRPY(0.0, 0.0, yaw);

            mMap2BaseTransf.setRotation(quat_2d);
        }

#if 0 // Enable for TF checking
        double roll, pitch, yaw;
        tf2::Matrix3x3(mMap2BaseTransf.getRotation()).getRPY(roll, pitch, yaw);

        RCLCPP_DEBUG(get_logger(), "*** Base POSE [%s -> %s] - {%.3f,%.3f,%.3f} {%.3f,%.3f,%.3f}",
                     mMapFrameId.c_str(), mBaseFrameId.c_str(),
                     mMap2BaseTransf.getOrigin().x(), mMap2BaseTransf.getOrigin().y(), mMap2BaseTransf.getOrigin().z(),
                     roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);
#endif

        bool initOdom = false;

        if (!(mFloorAlignment)) {
            initOdom = mInitOdomWithPose;
        } else {
            initOdom = mInitOdomWithPose & (mPosTrackingStatus == sl::POSITIONAL_TRACKING_STATE::OK);
        }

        if (initOdom || mResetOdom) {
            RCLCPP_INFO(get_logger(), "Odometry aligned to last tracking pose");

            // Propagate Odom transform in time
            mOdom2BaseTransf = mMap2BaseTransf;
            mMap2BaseTransf.setIdentity();

            if (odomSub > 0) {
                // Publish odometry message
                publishOdom(mOdom2BaseTransf, mLastZedPose, mFrameTimestamp);
            }

            mInitOdomWithPose = false;
            mResetOdom = false;
        } else {
            // Transformation from map to odometry frame
            mMap2OdomTransf = mMap2BaseTransf * mOdom2BaseTransf.inverse();

#if 0 // Enable for TF checking
            double roll, pitch, yaw;
            tf2::Matrix3x3(mMap2OdomTransf.getRotation()).getRPY(roll, pitch, yaw);

            RCLCPP_DEBUG(get_logger(), "+++ Diff [%s -> %s] - {%.3f,%.3f,%.3f} {%.3f,%.3f,%.3f}",
                         mMapFrameId.c_str(), mOdomFrameId.c_str(),
                         mMap2OdomTransf.getOrigin().x(), mMap2OdomTransf.getOrigin().y(), mMap2OdomTransf.getOrigin().z(),
                         roll * RAD2DEG, pitch * RAD2DEG, yaw * RAD2DEG);
#endif
        }

        // Publish Pose message
        publishPose();
        mPosTrackingReady = true;
    }

    oldStatus = mPosTrackingStatus;
}

void ZedCamera::publishPose() {
    size_t poseSub = 0;
    size_t poseCovSub = 0;

    try {
        poseSub = count_subscribers(mPoseTopic);         // mPubPose subscribers
        poseCovSub = count_subscribers(mPoseCovTopic);   // mPubPoseCov subscribers
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return;
    }

    tf2::Transform base_pose;
    base_pose.setIdentity();

    base_pose = mMap2BaseTransf;

    std_msgs::msg::Header header;
    header.stamp = mFrameTimestamp;
    header.frame_id = mMapFrameId; // frame

    geometry_msgs::msg::Pose pose;

    // Add all value in Pose message
    pose.position.x = mMap2BaseTransf.getOrigin().x();
    pose.position.y = mMap2BaseTransf.getOrigin().y();
    pose.position.z = mMap2BaseTransf.getOrigin().z();
    pose.orientation.x = mMap2BaseTransf.getRotation().x();
    pose.orientation.y = mMap2BaseTransf.getRotation().y();
    pose.orientation.z = mMap2BaseTransf.getRotation().z();
    pose.orientation.w = mMap2BaseTransf.getRotation().w();

    if (poseSub > 0) {
        poseMsgPtr poseNoCov = std::make_unique<geometry_msgs::msg::PoseStamped>();

        poseNoCov->header = header;
        poseNoCov->pose = pose;

        // Publish pose stamped message
        mPubPose->publish(std::move(poseNoCov));
    }

    if (mPublishPoseCov) {
        if (poseCovSub > 0) {
            poseCovMsgPtr poseCov = std::make_unique<geometry_msgs::msg::PoseWithCovarianceStamped>();

            poseCov->header = header;
            poseCov->pose.pose = pose;

            // Odometry pose covariance if available

            for (size_t i = 0; i < poseCov->pose.covariance.size(); i++) {
                poseCov->pose.covariance[i] = static_cast<double>(mLastZedPose.pose_covariance[i]);

                if (mTwoDMode) {
                    if ((i >= 2 && i <= 4) ||
                            (i >= 8 && i <= 10) ||
                            (i >= 12 && i <= 29) ||
                            (i >= 32 && i <= 34)) {
                        poseCov->pose.covariance[i] = 1e-9; // Very low covariance if 2D mode
                    }
                }
            }

            // Publish pose with covariance stamped message
            mPubPoseCov->publish(std::move(poseCov));
        }
    }
}

bool ZedCamera::isDepthRequired() {
    if ( mDepthQuality==sl::DEPTH_MODE::NONE ) {
        return false;
    }

    size_t topics_sub = 0;
    try {
        topics_sub = count_subscribers(mPubDepth.getTopic())
                + count_subscribers(mPubConfMap->get_topic_name())
                + count_subscribers(mPubDisparity->get_topic_name())
                + count_subscribers(mPubCloud->get_topic_name());
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return false;
    }

    return topics_sub>0 || isPosTrackingRequired();
}

void ZedCamera::applyDepthSettings() {
    if ( isDepthRequired() ) {
        mDynParMutex.lock();
        mRunParams.confidence_threshold = mDepthConf; // Update depth confidence if changed
        mRunParams.texture_confidence_threshold = mDepthTextConf; // Update depth texture confidence if changed
        mDynParMutex.unlock();

        mRunParams.enable_depth = true;
    }  else {
        mRunParams.enable_depth = false;
    }
}

void ZedCamera::applyVideoSettings() {
    if( !mSvoMode && mFrameCount%5 == 0 ) {
        mDynParMutex.lock();
        if (mCamAutoExpGain) {
            if (mTriggerAutoExpGain) {
                mZed.setCameraSettings(sl::VIDEO_SETTINGS::AEC_AGC, 1);
                mTriggerAutoExpGain = false;
            }
        } else {
            int exposure = mZed.getCameraSettings(sl::VIDEO_SETTINGS::EXPOSURE);
            if (exposure != mCamExposure) {
                mZed.setCameraSettings(sl::VIDEO_SETTINGS::EXPOSURE, mCamExposure);
            }

            int gain = mZed.getCameraSettings(sl::VIDEO_SETTINGS::GAIN);
            if (gain != mCamGain) {
                mZed.setCameraSettings(sl::VIDEO_SETTINGS::GAIN, mCamGain);
            }
        }
        if (mCamAutoWB) {
            if (mTriggerAutoWB) {
                mZed.setCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_AUTO, 1);
                mTriggerAutoWB = false;
            }
        } else {
            int wb = mZed.getCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_TEMPERATURE)/100;
            if (wb != mCamWBTemp) {
                mZed.setCameraSettings(sl::VIDEO_SETTINGS::WHITEBALANCE_TEMPERATURE, mCamWBTemp*100);
            }
        }
        int brgt = mZed.getCameraSettings(sl::VIDEO_SETTINGS::BRIGHTNESS);
        if (brgt != mCamBrightness) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::BRIGHTNESS, mCamBrightness);
        }
        int contr = mZed.getCameraSettings(sl::VIDEO_SETTINGS::CONTRAST);
        if (contr != mCamContrast) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::CONTRAST, mCamContrast);
        }
        int hue = mZed.getCameraSettings(sl::VIDEO_SETTINGS::HUE);
        if (hue != mCamHue) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::HUE, mCamHue);
        }
        int sat = mZed.getCameraSettings(sl::VIDEO_SETTINGS::SATURATION);
        if (sat != mCamSaturation) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::HUE, mCamSaturation);
        }
        int sharp = mZed.getCameraSettings(sl::VIDEO_SETTINGS::SHARPNESS);
        if (sharp != mCamSharpness) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::SHARPNESS, mCamSharpness);
        }
#if (ZED_SDK_MAJOR_VERSION>3 || (ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION>=1))
        int gamma = mZed.getCameraSettings(sl::VIDEO_SETTINGS::GAMMA);
        if (gamma != mCamGamma) {
            mZed.setCameraSettings(sl::VIDEO_SETTINGS::GAMMA, mCamGamma);
        }
#endif
        mDynParMutex.unlock();
    }
}

bool ZedCamera::isPosTrackingRequired() {
    if ( mDepthQuality==sl::DEPTH_MODE::NONE ) {
        return false;
    }

    if (mPublishTF) {
        return true;
    }

    if (mDepthStabilization) {
        return true;
    }

    if (mMappingEnabled||mObjDetEnabled) {
        return true;
    }

    size_t topics_sub = 0;
    try {
        topics_sub = count_subscribers(mPubPose->get_topic_name()) +
                + count_subscribers(mPubPoseCov->get_topic_name())
                + count_subscribers(mPubPosePath->get_topic_name())
                + count_subscribers(mPubOdom->get_topic_name())
                + count_subscribers(mPubOdomPath->get_topic_name());
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return false;
    }


    if (topics_sub>0) {
        return true;
    }

    return false;
}

void ZedCamera::publishDepthMapWithInfo(sl::Mat& depth, rclcpp::Time t) {

    mDepthCamInfoMsg->header.stamp = t;

    if (!mOpenniDepthMode) {
        auto depth_img = sl_tools::imageToROSmsg(depth, mDepthOptFrameId, t);
        mPubDepth.publish( depth_img, mDepthCamInfoMsg ); // TODO CHECK FOR ZERO-COPY
        return;
    }

    // OPENNI CONVERSION (meter -> millimeters - float32 -> uint16)
    std::shared_ptr<sensor_msgs::msg::Image> openniDepthMsg = std::make_shared<sensor_msgs::msg::Image>();

    openniDepthMsg->header.stamp = t;
    openniDepthMsg->header.frame_id = mDepthOptFrameId;
    openniDepthMsg->height = depth.getHeight();
    openniDepthMsg->width = depth.getWidth();

    int num = 1; // for endianness detection
    openniDepthMsg->is_bigendian = !(*(char*)&num == 1);

    openniDepthMsg->step = openniDepthMsg->width * sizeof(uint16_t);
    openniDepthMsg->encoding = sensor_msgs::image_encodings::MONO16;

    size_t size = openniDepthMsg->step * openniDepthMsg->height;
    openniDepthMsg->data.resize(size);

    uint16_t* data = (uint16_t*)(&openniDepthMsg->data[0]);

    int dataSize = openniDepthMsg->width * openniDepthMsg->height;
    sl::float1* depthDataPtr = depth.getPtr<sl::float1>();

    for (int i = 0; i < dataSize; i++) {
        *(data++) = static_cast<uint16_t>(std::round(*(depthDataPtr++) * 1000));    // in mm, rounded
    }

    mPubDepth.publish( openniDepthMsg, mDepthCamInfoMsg );
}

void ZedCamera::publishDisparity(sl::Mat disparity, rclcpp::Time t) {
    sl::CameraInformation zedParam = mZed.getCameraInformation(mMatResolDepth);

    std::shared_ptr<sensor_msgs::msg::Image> disparity_image =
            sl_tools::imageToROSmsg(disparity, mDepthOptFrameId, t);

    dispMsgPtr disparityMsg = std::make_unique<stereo_msgs::msg::DisparityImage>();
    disparityMsg->image = *disparity_image.get();
    disparityMsg->header = disparityMsg->image.header;
#if ZED_SDK_MAJOR_VERSION==3 && ZED_SDK_MINOR_VERSION<1
    disparityMsg->f = zedParam.calibration_parameters.left_cam.fx;
    disparityMsg->t = zedParam.calibration_parameters.T.x;
#else
    disparityMsg->f = zedParam.camera_configuration.calibration_parameters.left_cam.fx;
    disparityMsg->t = zedParam.camera_configuration.calibration_parameters.getCameraBaseline();
#endif
    disparityMsg->min_disparity = disparityMsg->f * disparityMsg->t / mZed.getInitParameters().depth_minimum_distance;
    disparityMsg->max_disparity = disparityMsg->f * disparityMsg->t / mZed.getInitParameters().depth_maximum_distance;

    mPubDisparity->publish(std::move(disparityMsg));
}

bool ZedCamera::publishDepthData(rclcpp::Time timeStamp) {

    static sl::Timestamp lastTs_depth;

    size_t depthSub = 0;
    size_t confMapSub = 0;
    size_t dispSub = 0;
    size_t cloudSub = 0;

    try {
        depthSub = count_subscribers(mPubDepth.getTopic());
        confMapSub = count_subscribers(mPubConfMap->get_topic_name());
        dispSub = count_subscribers(mPubDisparity->get_topic_name());
        cloudSub = count_subscribers(mPubCloud->get_topic_name());
    }
    catch(...) {
        rcutils_reset_error();
        RCLCPP_DEBUG(get_logger(), "Exception while counting subscribers");
        return false;
    }

    sl::Mat depthZEDMat, confMapZedMat, disparityZEDMat;

    if (depthSub+confMapSub+dispSub+cloudSub < 1) {
        return false;
    }

    // ----> Update img timestamp
    sl::Timestamp depth_ts;
    mZed.retrieveMeasure(depthZEDMat, sl::MEASURE::DEPTH, sl::MEM::CPU, mMatResolDepth);
    depth_ts = depthZEDMat.timestamp;

    if (depth_ts==lastTs_depth) {
        // Image not updated
        return true;
    }
    lastTs_depth=depth_ts;
    // <---- Update img timestamp

    // ---->  Publish the depth image if someone has subscribed to
    if (depthSub > 0) {
        publishDepthMapWithInfo(depthZEDMat, timeStamp);
    }
    // <----  Publish the depth image if someone has subscribed to

    // ---->  Publish the confidence image and map if someone has subscribed to
    if (confMapSub > 0) {
        mZed.retrieveMeasure(confMapZedMat, sl::MEASURE::CONFIDENCE, sl::MEM::CPU, mMatResolDepth);

        mPubConfMap->publish(*sl_tools::imageToROSmsg(confMapZedMat, mDepthOptFrameId, timeStamp));
    }

    // <----  Publish the confidence image and map if someone has subscribed to

    // ----> Publish the disparity image if someone has subscribed to
    if (dispSub > 0) {
        mZed.retrieveMeasure(disparityZEDMat, sl::MEASURE::DISPARITY, sl::MEM::CPU, mMatResolDepth);
        publishDisparity(disparityZEDMat, timeStamp);
    }
    // <---- Publish the disparity image if someone has subscribed to

    // ----> Publish the point cloud if someone has subscribed to
    if (cloudSub > 0) {
        // Run the point cloud conversion asynchronously to avoid slowing down
        // all the program
        // Retrieve raw pointCloud data if latest Pointcloud is ready
        std::unique_lock<std::mutex> lock(mPcMutex, std::defer_lock);

        if (lock.try_lock()) {
            mZed.retrieveMeasure(mCloud, sl::MEASURE::XYZBGRA, sl::MEM::CPU, mMatResolDepth);

            mPointCloudTime = timeStamp;

            // Signal Pointcloud thread that a new pointcloud is ready
            mPcDataReady = true;
            //RCLCPP_DEBUG(get_logger(), "publishDepthData -> mPcDataReady TRUE")

            mPcDataReadyCondVar.notify_one();
        }
    }
    // <---- Publish the point cloud if someone has subscribed to

    return true;
}

void ZedCamera::publishPointCloud() {

    pointcloudMsgPtr pcMsg = std::make_unique<sensor_msgs::msg::PointCloud2>();

    // Initialize Point Cloud message
    // https://github.com/ros/common_msgs/blob/jade-devel/sensor_msgs/include/sensor_msgs/point_cloud2_iterator.h

    int width = mMatResolDepth.width;
    int height = mMatResolDepth.height;

    int ptsCount = width * height;
    pcMsg->header.stamp = mPointCloudTime;

    if (pcMsg->width != width || pcMsg->height != height) {
        pcMsg->header.frame_id = mDepthFrameId; // Set the header values of the ROS message

        pcMsg->is_bigendian = false;
        pcMsg->is_dense = false;

        pcMsg->width = width;
        pcMsg->height = height;

        sensor_msgs::PointCloud2Modifier modifier(*(pcMsg.get()));
        modifier.setPointCloud2Fields(4,
                                      "x", 1, sensor_msgs::msg::PointField::FLOAT32,
                                      "y", 1, sensor_msgs::msg::PointField::FLOAT32,
                                      "z", 1, sensor_msgs::msg::PointField::FLOAT32,
                                      "rgb", 1, sensor_msgs::msg::PointField::FLOAT32);
    }

    sl::Vector4<float>* cpu_cloud = mCloud.getPtr<sl::float4>();

    // Data copy
    float* ptCloudPtr = (float*)(&pcMsg->data[0]);
    memcpy(ptCloudPtr, (float*)cpu_cloud, ptsCount * 4 * sizeof(float));

    // Pointcloud publishing
    mPubCloud->publish(std::move(pcMsg));

    // Publish freq calculation
    static std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    double elapsed_usec = std::chrono::duration_cast<std::chrono::microseconds>(now - last_time).count();
    last_time = now;

    double mean = mPcPeriodMean_usec->addValue(elapsed_usec);
    //RCLCPP_INFO_STREAM(get_logger(), "Point cloud freq: " << 1e6/mean);
}

} // namespace stereolabs

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(stereolabs::ZedCamera)
