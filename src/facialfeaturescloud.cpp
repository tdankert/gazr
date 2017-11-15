
#ifdef HEAD_POSE_ESTIMATION_DEBUG
#include <opencv2/highgui/highgui.hpp>
#endif

#include <std_msgs/Char.h>
#include <sensor_msgs/point_cloud2_iterator.h>
#include <cv_bridge/cv_bridge.h>

#include <depth_image_proc/depth_traits.h>


#include "facialfeaturescloud.hpp"

using namespace depth_image_proc;
using namespace std;
using namespace cv;
namespace enc = sensor_msgs::image_encodings;

FacialFeaturesPointCloudPublisher::FacialFeaturesPointCloudPublisher(ros::NodeHandle& rosNode,
                                                                     const std::string& prefix,
                                                                     const std::string& model):
    rosNode(rosNode),
    estimator(model),
    facePrefix(prefix)
{

    /// Subscribing
    rgb_it_.reset( new image_transport::ImageTransport(rosNode) );
    depth_it_.reset( new image_transport::ImageTransport(rosNode) );

    exact_sync_.reset( new ExactSynchronizer(ExactSyncPolicy(5), sub_rgb_, sub_depth_, sub_info_) );
    exact_sync_->registerCallback(bind(&FacialFeaturesPointCloudPublisher::imageCb, this, _1, _2, _3));

    // parameter for depth_image_transport hint
    std::string depth_image_transport_param = "depth_image_transport";

    // depth image can use different transport.(e.g. compressedDepth)
    image_transport::TransportHints depth_hints("raw",ros::TransportHints(), rosNode, depth_image_transport_param);
    sub_depth_.subscribe(*depth_it_, "depth",       1, depth_hints);

    // rgb uses normal ros transport hints.
    image_transport::TransportHints hints("raw", ros::TransportHints(), rosNode);
    sub_rgb_.subscribe(*rgb_it_, "rgb", 1, hints);
    sub_info_.subscribe(rosNode, "camera_info", 1);

    /// Publishing
    facial_features_pub = rosNode.advertise<PointCloud>("facial_features", 1);

}

/**
 * Based on https://github.com/ros-perception/image_pipeline/blob/indigo/depth_image_proc/src/nodelets/point_cloud_xyzrgb.cpp
 */
template<typename T>
Point3f FacialFeaturesPointCloudPublisher::makeFeatureCloud(const vector<Point> points2d,
                                                               const sensor_msgs::ImageConstPtr& depth_msg,
                                                               PointCloud::Ptr& cloud_msg) {

    // Use correct principal point from calibration
    float center_x = cameramodel.cx();
    float center_y = cameramodel.cy();

    // Combine unit conversion (if necessary) with scaling by focal length for computing (X,Y)
    double unit_scaling = DepthTraits<T>::toMeters( T(1) );
    float constant_x = unit_scaling / cameramodel.fx();
    float constant_y = unit_scaling / cameramodel.fy();
    float bad_point = std::numeric_limits<float>::quiet_NaN ();
    int row_step = depth_msg->step / sizeof(T);

    sensor_msgs::PointCloud2Iterator<float> iter_x(*cloud_msg, "x");
    sensor_msgs::PointCloud2Iterator<float> iter_y(*cloud_msg, "y");
    sensor_msgs::PointCloud2Iterator<float> iter_z(*cloud_msg, "z");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_r(*cloud_msg, "r");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_g(*cloud_msg, "g");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_b(*cloud_msg, "b");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_a(*cloud_msg, "a");


    for(size_t i = 0; i < points2d.size(); ++i) {
        auto point2d = points2d[i];
        const T* depth_row = reinterpret_cast<const T*>(&depth_msg->data[0]);

        char validPoints = 0;
        T avgDepth = 0;

        depth_row += row_step * (point2d.y - 2);
        for (size_t j = point2d.y - 2; j <= point2d.y + 2; j++) {
            for (size_t i = point2d.x - 2; i <= point2d.x + 2; i++) {
                T depth = depth_row[i];
                if(DepthTraits<T>::valid(depth)) {
                    validPoints++;
                    avgDepth += depth;
                }
            }
            depth_row += row_step;
        }

        if(validPoints > 0)
        {
            T depth = avgDepth / validPoints;

            // Fill in XYZ
            *iter_x = (point2d.x - center_x) * depth * constant_x;
            *iter_y = (point2d.y - center_y) * depth * constant_y;
            *iter_z = DepthTraits<T>::toMeters(depth);

            *iter_a = 255;

            if(i <= 16) {*iter_r = 100; *iter_g = 100; *iter_b = 100;} // face silhouette
            if(i >= 17 && i <= 21) {*iter_r = 255; *iter_g = 128; *iter_b = 0;} // right eyebrow
            if(i >= 22 && i <= 26) {*iter_r = 255; *iter_g = 128; *iter_b = 0;} // left eyebrow
            if(i >= 27 && i <= 35) {*iter_r = 0; *iter_g = 255; *iter_b = 128;} // nose
            if(i >= 36 && i <= 41) {*iter_r = 0; *iter_g = 128; *iter_b = 255;} // right eye
            if(i >= 42 && i <= 47) {*iter_r = 0; *iter_g = 0; *iter_b = 255;} // left eye
            if(i >= 48 && i <= 59) {*iter_r = 255; *iter_g = 128; *iter_b = 128;} // outer lips
            if(i >= 60 && i <= 67) {*iter_r = 128; *iter_g = 0; *iter_b = 0;} // inner lips
        }
        else
        {
            *iter_x = bad_point;
            *iter_y = bad_point;
            *iter_z = bad_point;
        }

        ++iter_x;
        ++iter_y;
        ++iter_z;

        ++iter_a;
        ++iter_r;
        ++iter_g;
        ++iter_b;
    }

}

void FacialFeaturesPointCloudPublisher::imageCb(const sensor_msgs::ImageConstPtr& rgb_msg,
                                                const sensor_msgs::ImageConstPtr& depth_msg,
                                                const sensor_msgs::CameraInfoConstPtr& camerainfo) {

    ROS_INFO_ONCE("First pair (rgb, depth) received");

    // updating the camera model is cheap if not modified
    cameramodel.fromCameraInfo(camerainfo);

    estimator.focalLength = cameramodel.fx(); 
    estimator.opticalCenterX = cameramodel.cx();
    estimator.opticalCenterY = cameramodel.cy();

    // hopefully no copy here:
    //  - assignement operator of cv::Mat does not copy the data
    //  - toCvShare does no copy if the default (source) encoding is used.
    auto rgb = cv_bridge::toCvShare(rgb_msg, "bgr8")->image; 

    // got an empty image!
    if (rgb.size().area() == 0) return;

    /********************************************************************
    *                      Faces detection                           *
    ********************************************************************/

    auto all_features = estimator.update(rgb);
    if(all_features.size() > 1) ROS_WARN("More than one face detected. 3D facial features computed for the first one only");

    auto features = all_features[0];

    // Allocate new point cloud message
    PointCloud::Ptr cloud_msg (new PointCloud);
    cloud_msg->header = depth_msg->header; // Use depth image time stamp
    cloud_msg->height = 1;
    cloud_msg->width  = 68; // nb of facial features
    cloud_msg->is_dense = false;
    cloud_msg->is_bigendian = false;

    sensor_msgs::PointCloud2Modifier pcd_modifier(*cloud_msg);
    pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");

    if (depth_msg->encoding == enc::TYPE_16UC1)
    {
        ROS_INFO_ONCE("Depth stream is 16UC1: mm encoded as integers");
        makeFeatureCloud<uint16_t>(features, depth_msg, cloud_msg);
    }
    else if (depth_msg->encoding == enc::TYPE_32FC1)
    {
        ROS_INFO_ONCE("Depth stream is 32FC1: m encoded as 32bit floats");
        makeFeatureCloud<float>(features, depth_msg, cloud_msg);
    }

    facial_features_pub.publish(cloud_msg);

    
    auto poses = estimator.poses();
    ROS_INFO_STREAM(poses.size() << " faces detected.");

    std_msgs::Char nb_faces;
    nb_faces.data = poses.size();

    nb_detected_faces_pub.publish(nb_faces);

    for(size_t face_idx = 0; face_idx < poses.size(); ++face_idx) {

        auto trans = poses[face_idx];

        tf::Transform face_pose;

        face_pose.setOrigin( tf::Vector3( trans(0,3),
                                          trans(1,3),
                                          trans(2,3)) );

        tf::Quaternion qrot;
        tf::Matrix3x3 mrot(
                trans(0,0), trans(0,1), trans(0,2),
                trans(1,0), trans(1,1), trans(1,2),
                trans(2,0), trans(2,1), trans(2,2));
        mrot.getRotation(qrot);
        face_pose.setRotation(qrot);

        tf::StampedTransform transform(face_pose, 
                rgb_msg->header.stamp,  // publish the transform with the same timestamp as the frame originally used
                cameramodel.tfFrame(),
                facePrefix + "_" + to_string(face_idx));
        br.sendTransform(transform);

    }

#ifdef HEAD_POSE_ESTIMATION_DEBUG
    if(pub.getNumSubscribers() > 0) {
        ROS_INFO_ONCE("Starting to publish face tracking output for debug");
        auto debugmsg = cv_bridge::CvImage(msg->header, "bgr8", estimator._debug).toImageMsg();
        pub.publish(debugmsg);
    }
#endif
}

