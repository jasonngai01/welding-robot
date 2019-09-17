#include <ros/ros.h>
#include <math.h>
#include <iostream>   
#include <vector>
#include <algorithm.h>

#include <image_transport/image_transport.h>
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/PoseStamped.h>

// PCL lib
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl_conversions/pcl_conversions.h>
#include <transformation.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>//点云文件pcd 读写
#include <pcl/visualization/cloud_viewer.h>//点云可视化
#include <pcl/visualization/pcl_visualizer.h>// 高级可视化点云类
#include <pcl/features/normal_3d.h>//法线特征
#include <pcl/kdtree/kdtree_flann.h>//搜索方法
#include <boost/thread/thread.hpp>

#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
 
// 定义点云类型
typedef pcl::PointCloud<pcl::PointXYZRGB> PointCloud; 
typedef pcl::PointCloud<pcl::PointXYZRGBL> PointCloudL;  
typedef pcl::PointCloud<pcl::PointXYZ>  Cloud;
typedef pcl::PointXYZ PointType;

using namespace cv;
using namespace std;
//namespace enc = sensor_msgs::image_encodings;

// 相机内参
const double camera_factor = 985;
const double camera_cx = 320;
const double camera_cy = 240;
const double camera_fx = 615.899;
const double camera_fy = 616.468;

// 全局变量：图像矩阵和点云
cv_bridge::CvImagePtr color_ptr, depth_ptr;
cv::Mat color_pic, depth_pic;

//receive robot current pose flag
int receive_pose_flag = 0;
float current_x = 0  , current_y = 0    , current_z = 0;
float current_yaw = 0, current_pitch = 0, current_roll = 0;


void color_Callback(const sensor_msgs::ImageConstPtr& color_msg)
{
  try
  {
    cv::imshow("color_view", cv_bridge::toCvShare(color_msg, sensor_msgs::image_encodings::BGR8)->image);
    color_ptr = cv_bridge::toCvCopy(color_msg, sensor_msgs::image_encodings::BGR8);    
  }
  catch (cv_bridge::Exception& e )
  {
    ROS_ERROR("Could not convert from '%s' to 'bgr8'.", color_msg->encoding.c_str());
  }
  color_pic = color_ptr->image;

  waitKey(1); 
}

void depth_Callback(const sensor_msgs::ImageConstPtr& depth_msg)
{
  try
  {
    // cv::imshow("depth_view", cv_bridge::toCvShare(depth_msg, sensor_msgs::image_encodings::TYPE_32FC1)->image);
    depth_ptr = cv_bridge::toCvCopy(depth_msg, sensor_msgs::image_encodings::TYPE_32FC1); 
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("Could not convert from '%s' to 'mono16'.", depth_msg->encoding.c_str());
  }

  depth_pic = depth_ptr->image;
  waitKey(1);
}

void robot_currentpose_Callback(const geometry_msgs::PoseStamped::ConstPtr& msg) //Note it is geometry_msgs::PoseStamped, not std_msgs::PoseStamped
{

  ROS_INFO("I heard the pose from the robot"); 
  ROS_INFO("the position(x,y,z) is %f , %f, %f", msg->pose.position.x, msg->pose.position.y, msg->pose.position.z);
  ROS_INFO("the orientation(yaw, pitch, roll) is %f , %f, %f ", msg->pose.orientation.x, msg->pose.orientation.y, msg->pose.orientation.z);
  ROS_INFO("the time we get the pose is %f",  msg->header.stamp.sec + 1e-9*msg->header.stamp.nsec);

  current_x     = msg->pose.position.x;
  current_y     = msg->pose.position.y; 
  current_z     = msg->pose.position.z;

  current_yaw   = msg->pose.orientation.x;
  current_pitch = msg->pose.orientation.y;
  current_roll  = msg->pose.orientation.z;

  if (current_roll != 0)
  {
    receive_pose_flag = 1;
  }
  cout << " \n" << endl; //add two more blank row so that we can see the message more clearly
}


int main(int argc, char **argv)
{
  ros::init(argc, argv, "image_listener");
  ros::NodeHandle nh;

  //subscriber:
  image_transport::ImageTransport it(nh);
  image_transport::Subscriber color_sub = it.subscribe("/camera/color/image_raw", 1, color_Callback);
  image_transport::Subscriber depth_sub = it.subscribe("/camera/depth/image_rect_raw", 1, depth_Callback);
  ros::Subscriber sub = nh.subscribe("robot_currentpose", 10, robot_currentpose_Callback);
 
  //publisher:
  ros::Publisher pointcloud_publisher = nh.advertise<sensor_msgs::PointCloud2>("generated_pc", 1);
  ros::Publisher map_publisher = nh.advertise<sensor_msgs::PointCloud2>("generated_map", 1);

  // 点云变量
  // 使用智能指针，创建一个空点云。这种指针用完会自动释放。
  PointCloud::Ptr cloud ( new PointCloud );
  PointCloud::Ptr map   ( new PointCloud );

  sensor_msgs::PointCloud2 pub_pointcloud;
  sensor_msgs::PointCloud2 pub_map;

  pcl::PCDWriter writer;

  double sample_rate = 1000.0; // 1000HZ 
  ros::Rate naptime(sample_rate); // use to regulate loop rate 
 
  int initial_flag = 1;

  float pic_count = 0;




  PointCloud::Ptr cloud_output (new PointCloud);

  Cloud::Ptr cloud_ptr = read_pointcloud();

  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
  ne.setInputCloud (cloud_ptr);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ> ());
  ne.setSearchMethod (tree);

  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals_ptr (new pcl::PointCloud<pcl::Normal>);
  pcl::PointCloud<pcl::Normal>& cloud_normals = *cloud_normals_ptr;
  ne.setRadiusSearch (0.005);
  ne.compute (cloud_normals);

  float total_pointcount = 0, ave_normal_x = 0, ave_normal_y = 0, ave_normal_z = 0;
  for(float i = 0; i < cloud_ptr->points.size(); i++)
  { 
    if ( __isnan(cloud_normals[i].curvature) == true)
    {
      continue;
    }

    total_pointcount++;

    ave_normal_x += cloud_normals[i].normal_x;
    ave_normal_y += cloud_normals[i].normal_y;
    ave_normal_z += cloud_normals[i].normal_z;
  }

  ave_normal_x = 1.0 * ave_normal_x / total_pointcount;
  ave_normal_y = 1.0 * ave_normal_y / total_pointcount;
  ave_normal_z = 1.0 * ave_normal_z / total_pointcount;

  cout << "compute is done!!! " << endl;
  cout << "cloud_ptr->points.size(): " << cloud_output->points.size() << endl;
  cout << "nan-point count: " << cloud_normals.size() - total_pointcount << endl;//858
  cout << "ave_normal_x: " << ave_normal_x << endl;
  cout << "ave_normal_y: " << ave_normal_y << endl;
  cout << "ave_normal_z: " << ave_normal_z << endl << endl;

  vector<float> Dir_descriptor ;
  for(float i = 0; i < cloud_ptr->points.size(); i++)
  { 
    if ( __isnan(cloud_normals[i].curvature) == true)
    {
      continue;
    }

    float dir_descriptor;
    dir_descriptor = sqrt(pow(cloud_normals[i].normal_x - ave_normal_x, 2) + 
                          pow(cloud_normals[i].normal_y - ave_normal_y, 2) + 
                          pow(cloud_normals[i].normal_z - ave_normal_z, 2));

    Dir_descriptor.push_back( dir_descriptor );     


    pcl::PointXYZRGB p;
    p.x = cloud_ptr->points[i].x; 
    p.y = cloud_ptr->points[i].y;
    p.z = cloud_ptr->points[i].z;
    p.b = 200; 
    p.g = 200; 
    p.r = 200; 
 
    cloud_output->points.push_back( p );        
  }

  float Dir_descriptor_max = 0, Dir_descriptor_min = 0;
  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    if(i == 0)
    {
      Dir_descriptor_max = Dir_descriptor[0];
      Dir_descriptor_min = Dir_descriptor[0];
    }

    if (Dir_descriptor_max < Dir_descriptor[i])
    {
      Dir_descriptor_max = Dir_descriptor[i];
    }

    if(Dir_descriptor_min > Dir_descriptor[i])
    {
      Dir_descriptor_min = Dir_descriptor[i];
    }
  }

  cout << "Dir_descriptor_max: "    << Dir_descriptor_max << endl;
  cout << "Dir_descriptor_min: "    << Dir_descriptor_min << endl;

  cout << "Dir_descriptor size(): " << Dir_descriptor.size()       << endl;
  cout << "cloud_output size(): "   << cloud_output->points.size() << endl << endl;

  float weight_of_descriptor = 0;
  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    weight_of_descriptor = (Dir_descriptor[i] - Dir_descriptor_min) / (Dir_descriptor_max - Dir_descriptor_min);


    cloud_output->points[i].b = 255 * weight_of_descriptor;
    cloud_output->points[i].g = 0;
    cloud_output->points[i].r = 255 * (1 - weight_of_descriptor);
  }
 

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_tree (new pcl::PointCloud<pcl::PointXYZ>);
  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    pcl::PointXYZ p;

    p.x = cloud_output->points[i].x;
    p.y = cloud_output->points[i].y;
    p.z = cloud_output->points[i].z;

    cloud_tree->points.push_back( p );
  }
  cout << "cloud_output->points.size(): " << cloud_output->points.size() << endl;
  cout << "cloud_tree->points.size(): "   << cloud_tree->points.size() << endl << endl;

  // 创建一个 KdTree 对象
  pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
 
  // 将前面创建的随机点云作为 KdTree 输入
  kdtree.setInputCloud (cloud_tree);

 // 创建两个向量，分别存放近邻的索引值、近邻的中心距
  vector<int> pointIdxRadiusSearch;
  vector<float> pointRadiusSquaredDistance;
 
  // 指定随机半径
  float radius = 0.005;

  vector<float> variance_descriptor ;

  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    kdtree.radiusSearch (cloud_tree->points[i], radius, pointIdxRadiusSearch, pointRadiusSquaredDistance);  

    float variance = 0, sum = 0, average_kdtree = 0;

    for (float j = 0; j < pointIdxRadiusSearch.size(); j++)
    {
      sum += Dir_descriptor[ pointIdxRadiusSearch[j] ];
    }
    average_kdtree = sum / pointIdxRadiusSearch.size();
    // cout << "sum:" << average_kdtree << endl;

    for (float j = 0; j < pointIdxRadiusSearch.size(); j++)
    {
      variance += pow(Dir_descriptor[pointIdxRadiusSearch[j]] - average_kdtree, 2);
    }    

    variance = variance / pointIdxRadiusSearch.size();
    // cout << "variance:" << variance << endl;
    variance_descriptor.push_back( variance );
  }

  float Var_descriptor_max = 0, Var_descriptor_min = 0;
  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    if(i == 0)
    {
      Var_descriptor_max = variance_descriptor[0];
      Var_descriptor_min = variance_descriptor[0];
    }

    if (Var_descriptor_max < variance_descriptor[i])
    {
      Var_descriptor_max = variance_descriptor[i];
    }

    if(Var_descriptor_min > variance_descriptor[i])
    {
      Var_descriptor_min = variance_descriptor[i];
    }
  }

  cout << "Var_descriptor_max: "    << Var_descriptor_max << endl;
  cout << "Var_descriptor_min: "    << Var_descriptor_min << endl;

  cout << "cloud_output->points.size(): " << cloud_output->points.size() << endl;
  cout << "variance_descriptor.size(): "   << variance_descriptor.size() << endl << endl;

  float weight_variance_descriptor = 0;
  float weight_variance_threshold = 0.05;

  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    weight_variance_descriptor = (variance_descriptor[i] - Var_descriptor_min) / (Var_descriptor_max - Var_descriptor_min);

    if( weight_variance_descriptor > weight_variance_threshold)
    {
      cloud_output->points[i].b = 200;
      cloud_output->points[i].g = 0;
      cloud_output->points[i].r = 0;
    }
    else
    {
      cloud_output->points[i].b = 200;
      cloud_output->points[i].g = 200;
      cloud_output->points[i].r = 200;
    }
  }
 



  // 创建一个 KdTree 对象
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_tree_sreenout (new pcl::PointCloud<pcl::PointXYZ>);
  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    pcl::PointXYZ p;

    p.x = cloud_output->points[i].x;
    p.y = cloud_output->points[i].y;
    p.z = cloud_output->points[i].z;

    cloud_tree_sreenout->points.push_back( p );
  }
  cout << "cloud_output->points.size(): " << cloud_output->points.size() << endl;
  cout << "cloud_tree_sreenout->points.size(): "   << cloud_tree_sreenout->points.size() << endl << endl;

  pcl::KdTreeFLANN<pcl::PointXYZ> kdtree_sreenout;
 
  // 将前面创建的随机点云作为 KdTree 输入
  kdtree_sreenout.setInputCloud (cloud_tree_sreenout);

 // 创建两个向量，分别存放近邻的索引值、近邻的中心距
  vector<int> pointIdxRadiusSearch_sreenout;
  vector<float> pointRadiusSquaredDistance_sreenout;
 
  // 指定随机半径
  float radius_sreenout = 0.005;

  PointCloud::Ptr cloud_output_screenout (new PointCloud);

  for(float i = 0; i < cloud_output->points.size(); i++)
  { 
    pcl::PointXYZRGB p;

    kdtree_sreenout.radiusSearch (cloud_tree_sreenout->points[i], radius_sreenout, pointIdxRadiusSearch_sreenout, pointRadiusSquaredDistance_sreenout);  

    float blue_count = 0;
    for (float j = 0; j < pointIdxRadiusSearch_sreenout.size(); j++)
    {
      if (cloud_output->points[ pointIdxRadiusSearch_sreenout[j] ].b == 200 &&
          cloud_output->points[ pointIdxRadiusSearch_sreenout[j] ].g == 0   &&
          cloud_output->points[ pointIdxRadiusSearch_sreenout[j] ].r == 0)
          {
            blue_count++;
          }
    }

    if (1.0 * blue_count / pointIdxRadiusSearch_sreenout.size() > 0.5 && pointIdxRadiusSearch_sreenout.size() > 1)
    {
      p.x = cloud_output->points[ i ].x;
      p.y = cloud_output->points[ i ].y;
      p.z = cloud_output->points[ i ].z;
      p.b = 200;
      p.g = 0;
      p.r = 0;

      cloud_output_screenout->points.push_back( p );
    }

  }
  cout << "cloud_output->points.size(): " << cloud_output->points.size() << endl;
  cout << "cloud_output_screenout->points.size(): " << cloud_output_screenout->points.size() << endl << endl;



  //为提取点云时使用的搜素对象利用输入点云cloud_filtered创建Kd树对象tree。
  pcl::PointCloud<pcl::PointXYZ>::Ptr ec_tree_cloud (new pcl::PointCloud<pcl::PointXYZ>);
  for(float i = 0; i < cloud_output_screenout->points.size(); i++)
  { 
    pcl::PointXYZ p;

    p.x = cloud_output_screenout->points[ i ].x;
    p.y = cloud_output_screenout->points[ i ].y;
    p.z = cloud_output_screenout->points[ i ].z;

    ec_tree_cloud->points.push_back( p );
  }

  pcl::search::KdTree<pcl::PointXYZ>::Ptr ec_tree (new pcl::search::KdTree<pcl::PointXYZ>);
  ec_tree->setInputCloud (ec_tree_cloud);//创建点云索引向量，用于存储实际的点云信息
  
  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZ> EC;

  EC.setClusterTolerance (0.01); //设置近邻搜索的搜索半径为2cm
  EC.setMinClusterSize (1);//设置一个聚类需要的最少点数目为100
  EC.setMaxClusterSize (100000); //设置一个聚类需要的最大点数目为25000
  EC.setSearchMethod (ec_tree);//设置点云的搜索机制
  EC.setInputCloud (ec_tree_cloud);
  EC.extract (cluster_indices);//从点云中提取聚类，并将点云索引保存在cluster_indices中
  
  cout << "ec_tree_cloud->points.size(): "  << ec_tree_cloud->points.size() << endl ;
  cout << "cluster_indices.size(): " << cluster_indices.size() << endl << endl;

  PointCloud::Ptr cloud_output_goal (new PointCloud);

  for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it)
  {
    pcl::PointXYZRGB p;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster (new pcl::PointCloud<pcl::PointXYZ>);
    
    for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); ++pit)
    {
      cloud_cluster->points.push_back (ec_tree_cloud->points[*pit]);  
    }

    cout << "cloud_cluster->points.size(): " << cloud_cluster->points.size() << endl;

    for(float i = 0; i < cloud_cluster->points.size(); i++)
    { 
      p.x = cloud_cluster->points[ i ].x;
      p.y = cloud_cluster->points[ i ].y;
      p.z = cloud_cluster->points[ i ].z;
      p.b = 200;
      p.g = 0;
      p.r = 0;

      cloud_output_goal->points.push_back( p ); 
    }

    break;
  }
  cout << "cloud_output_goal->points.size(): " << cloud_output_goal->points.size() << endl << endl;




  float min_x = 0, max_x = 0;
  float min_y = 0, max_y = 0;
  float min_z = 0, max_z = 0;
  for(float i = 0; i < cloud_output_goal->points.size(); i++)
  { 
    if(i == 0)
    {
      min_x = cloud_output_goal->points[ i ].x;
      min_y = cloud_output_goal->points[ i ].y;
      min_z = cloud_output_goal->points[ i ].z;

      max_x = cloud_output_goal->points[ i ].x;
      max_y = cloud_output_goal->points[ i ].y;
      max_z = cloud_output_goal->points[ i ].z;
    }
    //x
    if (max_x < cloud_output_goal->points[ i ].x)
    {
      max_x = cloud_output_goal->points[ i ].x;
    }

    if(min_x > cloud_output_goal->points[ i ].x)
    {
      min_x = cloud_output_goal->points[ i ].x;
    }
    //y
    if (max_y < cloud_output_goal->points[ i ].y)
    {
      max_y = cloud_output_goal->points[ i ].y;
    }

    if(min_y > cloud_output_goal->points[ i ].y)
    {
      min_y = cloud_output_goal->points[ i ].y;
    }
    //z
    if (max_z < cloud_output_goal->points[ i ].z)
    {
      max_z = cloud_output_goal->points[ i ].z;
    }

    if(min_z > cloud_output_goal->points[ i ].z)
    {
      min_z = cloud_output_goal->points[ i ].z;
    }
  }

  float range_x = max_x - min_x, range_y = max_y - min_y, range_z = max_z - min_z;
  cout << "max_x - min_x:  " << range_x << endl;
  cout << "max_y - min_y:  " << range_y << endl;
  cout << "max_z - min_z:  " << range_z << endl ;

  char range_max = '0';
  if( range_x > range_y)
  {
    range_max = 'x';
  }
  else
  {
    range_max = 'y';
  }
  
  if( range_max == range_x)
  {
    if( range_x > range_z)
    {
      range_max = 'x';
    }
    else
    {
      range_max = 'z';
    }
  }
  if( range_max == range_y)
  {
    if( range_y > range_z)
    {
      range_max = 'y';
    }
    else
    {
      range_max = 'z';
    }
  }
  cout << "range_max:  " << range_max << endl ;

  float segmentation_count = 50, seg_interval = range_x / segmentation_count;
  vector< vector<int> > seg_pointcloud;
  seg_pointcloud.resize(segmentation_count);

  switch (range_max)
  {
    case 'x':
      for( int j = 0; j < segmentation_count; j++)
      {
        for(float i = 0; i < cloud_output_goal->points.size(); i++)
        {
          if(cloud_output_goal->points[i].x >= min_x + seg_interval*(j) && cloud_output_goal->points[i].x < seg_interval*(j+1) + min_x)
          {
            seg_pointcloud[j].push_back( i );
          }
          if( cloud_output_goal->points[i].x == seg_interval*(segmentation_count) + min_x && j == segmentation_count - 1)
          {
            seg_pointcloud[segmentation_count - 1].push_back( i );            
          }
        }
      } 
      break;

    case 'y':
      for(float i = 0; i < cloud_output_goal->points.size(); i++)
      {
        
      } 
      break;

    case 'z':
      for(float i = 0; i < cloud_output_goal->points.size(); i++)
      {
        
      } 
      break;

    default:
      break;
  }

  float seg_counttt = 0;
  for( float j = 0; j < seg_pointcloud.size(); j++)
  {
    cout << "seg_pointcloud[j].size(): " << seg_pointcloud[j].size() << endl; 
    seg_counttt += seg_pointcloud[j].size();
  }

  cout << "seg_counttt:" << seg_counttt << endl;
  cout << "cloud_output_goal->points.size(): " << cloud_output_goal->points.size() << endl << endl;

  //show segmentation


  for(int k = 0; k < seg_pointcloud.size(); k++)
  {
    for( float j = 0; j < seg_pointcloud[k].size(); j++)
    {
      switch (k % 3)
      {
        case 0:
          cloud_output_goal->points[ seg_pointcloud[k][j] ].b = 200;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].g = 0;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].r = 0;
          break;
        case 1:
          cloud_output_goal->points[ seg_pointcloud[k][j] ].b = 0;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].g = 200;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].r = 0;
          break;
        case 2:
          cloud_output_goal->points[ seg_pointcloud[k][j] ].b = 0;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].g = 0;
          cloud_output_goal->points[ seg_pointcloud[k][j] ].r = 200;
        break;
      }
    }
  }
  cout << "seg_pointcloud.size(): " << seg_pointcloud.size() << endl;
  cout <<"segmentation show!!!" << endl << endl;


  PointCloud::Ptr path_cloud (new PointCloud);
  pcl::PointXYZRGB Xp_goal;
  Xp_goal.x = 0; Xp_goal.y = 0; Xp_goal.z = 0;

  pcl::PointXYZ Xp;
  Xp.x = 0; Xp.y = 0; Xp.z = 0;

  pcl::PointXYZ Xpi; 
  Xpi.x = 0; Xpi.y = 0; Xpi.z = 0;

  float theta = 0.01;
  float loop_max = 10000;
  float epsilon = 0.0001;
  float loss = 0;

  for(int k = 0; k < seg_pointcloud.size(); k++)
  {
    for (float loop_count = 0; loop_count < loop_max; loop_count++)
    {
      // cost computation:
      float cost_function = 0;
      for( float j = 0; j < seg_pointcloud[k].size(); j++)
      {
        cost_function += sqrt(pow(Xp.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x, 2) + 
                              pow(Xp.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y, 2) + 
                              pow(Xp.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z, 2) );
      }

      //gradient computation:
      pcl::PointXYZ Gradient_Xp;
      for( float j = 0; j < seg_pointcloud[k].size(); j++)
      {
        Gradient_Xp.x += (Xp.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x) / 
                          sqrt(pow(Xp.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x, 2) + 
                               pow(Xp.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y, 2) + 
                               pow(Xp.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z, 2) );

        Gradient_Xp.y += (Xp.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y) / 
                          sqrt(pow(Xp.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x, 2) + 
                               pow(Xp.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y, 2) + 
                               pow(Xp.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z, 2) );

        Gradient_Xp.z += (Xp.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z) / 
                          sqrt(pow(Xp.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x, 2) + 
                               pow(Xp.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y, 2) + 
                               pow(Xp.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z, 2) );
      }

      Xpi.x = Xp.x - theta * Gradient_Xp.x;
      Xpi.y = Xp.y - theta * Gradient_Xp.y;
      Xpi.z = Xp.z - theta * Gradient_Xp.z;

      float cost_function_update = 0;
      for( float j = 0; j < seg_pointcloud[k].size(); j++)
      {
        cost_function_update += sqrt(pow(Xpi.x - cloud_output_goal->points[ seg_pointcloud[k][j] ].x, 2) + 
                                     pow(Xpi.y - cloud_output_goal->points[ seg_pointcloud[k][j] ].y, 2) + 
                                     pow(Xpi.z - cloud_output_goal->points[ seg_pointcloud[k][j] ].z, 2) );
      }

      //判断loss是否小于阈值
      loss = cost_function - cost_function_update;
      // cout << "loss: " << loss << endl << endl;

      if(loss > epsilon)
      {
        Xp.x = Xpi.x;
        Xp.y = Xpi.y;
        Xp.z = Xpi.z;

        cost_function = cost_function_update;
      }

      else if(cost_function_update - cost_function > epsilon)
      {
        theta = theta * 0.8;
      }

      else
      {
        Xp_goal.x = Xp.x;
        Xp_goal.y = Xp.y;
        Xp_goal.z = Xp.z;
        Xp_goal.b = 0;
        Xp_goal.g = 0;
        Xp_goal.r = 0;
        cloud_output_goal->points.push_back( Xp_goal ) ; 

        break;
      }

    }
    cout << "seg_count: " << k+1 << endl;
    cout << "Xp_goal: " << Xp_goal << endl << endl;
    path_cloud->points.push_back( Xp_goal );

    if(k > 0)
    {
      pcl::PointXYZRGB p_add1, p_add2, p_add3, p_add4, p_add5, p_add6, p_add7;

      p_add1.x = (path_cloud->points[k - 1].x + path_cloud->points[k].x) / 2.0;
      p_add1.y = (path_cloud->points[k - 1].y + path_cloud->points[k].y) / 2.0;
      p_add1.z = (path_cloud->points[k - 1].z + path_cloud->points[k].z) / 2.0;
      cloud_output_goal->points.push_back( p_add1 );

      p_add2.x = (path_cloud->points[k - 1].x + p_add1.x) / 2.0;
      p_add2.y = (path_cloud->points[k - 1].y + p_add1.y) / 2.0;
      p_add2.z = (path_cloud->points[k - 1].z + p_add1.z) / 2.0;
      cloud_output_goal->points.push_back( p_add2 );

      p_add3.x = (p_add1.x + path_cloud->points[k].x) / 2.0;
      p_add3.y = (p_add1.y + path_cloud->points[k].y) / 2.0;
      p_add3.z = (p_add1.z + path_cloud->points[k].z) / 2.0;
      cloud_output_goal->points.push_back( p_add3 );

      p_add4.x = (path_cloud->points[k - 1].x + p_add2.x) / 2.0;
      p_add4.y = (path_cloud->points[k - 1].y + p_add2.y) / 2.0;
      p_add4.z = (path_cloud->points[k - 1].z + p_add2.z) / 2.0;
      cloud_output_goal->points.push_back( p_add4 );

      p_add5.x = (p_add1.x + p_add2.x) / 2.0;
      p_add5.y = (p_add1.y + p_add2.y) / 2.0;
      p_add5.z = (p_add1.z + p_add2.z) / 2.0;
      cloud_output_goal->points.push_back( p_add5 );

      p_add6.x = (p_add1.x + p_add3.x) / 2.0;
      p_add6.y = (p_add1.y + p_add3.y) / 2.0;
      p_add6.z = (p_add1.z + p_add3.z) / 2.0;
      cloud_output_goal->points.push_back( p_add6 );

      p_add7.x = (path_cloud->points[k].x + p_add3.x) / 2.0;
      p_add7.y = (path_cloud->points[k].y + p_add3.y) / 2.0;
      p_add7.z = (path_cloud->points[k].z + p_add3.z) / 2.0;
      cloud_output_goal->points.push_back( p_add7 );

    }
  }
 
  cout << "path_cloud->points.size(): " << path_cloud->points.size() << endl << endl;

  





  while (ros::ok()) 
  {
    // 遍历深度图
    for (int m = 0; m < depth_pic.rows; m++)  //480
    {
      for (int n = 0; n < depth_pic.cols; n++) //640
      {
        // 获取深度图中(m,n)处的值
        float d = depth_pic.ptr<float>(m)[n]; 
        // d 可能没有值，若如此，跳过此点
        if (d == 0)
            continue;

        // d 存在值，则向点云增加一个点
        pcl::PointXYZRGB p, p_z, p_x, p_y;
        pcl::PointXYZRGB p_transform;

        // 计算这个点的空间坐标
        p.z = double(d) / camera_factor;
        p.x = (n - camera_cx) * p.z / camera_fx; 
        p.y = (m - camera_cy) * p.z / camera_fy;

        // 从rgb图像中获取它的颜色
        // rgb是三通道的BGR格式图，所以按下面的顺序获取颜色
        p.b = color_pic.ptr<uchar>(m)[n*3];
        p.g = color_pic.ptr<uchar>(m)[n*3+1];
        p.r = color_pic.ptr<uchar>(m)[n*3+2];

        // cout << "p.b:" << p.b << endl;
        // 把p加入到点云中
        cloud->points.push_back( p );        
      }
    }
 
    // pic_count++;
    // cout << "pic_count :" << pic_count << endl;
    // if (pic_count >= 2000 && initial_flag == 1)
    // {
      
    //   cloud->width = 1;
    //   cloud->height = cloud->points.size();

    //   cout << "cloud->points.size()" << cloud->points.size() << endl;
    //   pcl::PCDWriter writer;
    //   writer.write("/home/rick/Documents/a_system/src/seam_detection/save_pcd/test.pcd", *cloud, false) ;

    //   initial_flag = 0;
    // }
    // pcl::toROSMsg(*cloud, pub_pointcloud);


    pcl::toROSMsg(*cloud_output_goal, pub_pointcloud);
    // pcl::toROSMsg(*map  , pub_map);

    pub_pointcloud.header.frame_id = "camera_color_optical_frame";
    pub_pointcloud.header.stamp = ros::Time::now();

    pub_map.header.frame_id = "world";
    pub_map.header.stamp = ros::Time::now();

    pointcloud_publisher.publish(pub_pointcloud);
    // map_publisher.publish(pub_map);
 
    cloud->points.clear();
    // map->points.clear();

    ros::spinOnce(); //allow data update from callback; 
    naptime.sleep(); // wait for remainder of specified period; 
  }


  ros::spin();
 
}