/**
 * octomap_server: A Tool to serve 3D OctoMaps in ROS (binary and as visualization)
 * (inspired by the ROS map_saver)
 * @author A. Hornung, University of Freiburg, Copyright (C) 2010-2012.
 * @see http://octomap.sourceforge.net/
 * License: BSD
 */

/*
 * Copyright (c) 2010-2012, A. Hornung, University of Freiburg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of Freiburg nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <octomap_server/OctomapServer.h>

#define MIN_COST 50

using namespace octomap;
using octomap_msgs::Octomap;

namespace octomap_server {

OctomapServer::OctomapServer(ros::NodeHandle nh) :
		m_nh(), m_pointCloudSub(NULL), m_tfPointCloudSub(NULL), m_octree(NULL),
		m_maxRange(-1.0), m_worldFrameId("/map"), m_baseFrameId("base_footprint"), m_SourceFrameId("stereo_camera"),
		m_useHeightMap(true), m_colorFactor(0.8), m_latchedTopics(true), m_res(0.1),
		m_treeDepth(0), m_maxTreeDepth(0), m_probHit(0.7), m_probMiss(0.4),
		m_thresMin(0.12), m_thresMax(0.97), m_pointcloudMinZ(-std::numeric_limits<double>::max()),
		m_pointcloudMaxZ(std::numeric_limits<double>::max()), m_occupancyMinZ(-std::numeric_limits<double>::max()),
		m_occupancyMaxZ(std::numeric_limits<double>::max()), m_minSizeX(0.0), m_minSizeY(0.0),
		m_filterSpeckles(false), m_filterGroundPlane(false), m_groundFilterDistance(0.04),
		m_groundFilterAngle(0.15), m_groundFilterPlaneDistance(0.07), m_compressMap(false),
		m_incrementalUpdate(false), m_unknownCost(-1) {
	ros::NodeHandle private_nh(nh);
	private_nh.param("frame_id", m_worldFrameId, m_worldFrameId);
	private_nh.param("source_frame_id", m_SourceFrameId, m_SourceFrameId);
	private_nh.param("base_frame_id", m_baseFrameId, m_baseFrameId);
	private_nh.param("height_map", m_useHeightMap, m_useHeightMap);
	private_nh.param("color_factor", m_colorFactor, m_colorFactor);

	private_nh.param("pointcloud_min_z", m_pointcloudMinZ, m_pointcloudMinZ);
	private_nh.param("pointcloud_max_z", m_pointcloudMaxZ, m_pointcloudMaxZ);
	private_nh.param("occupancy_min_z", m_occupancyMinZ, m_occupancyMinZ);
	private_nh.param("occupancy_max_z", m_occupancyMaxZ, m_occupancyMaxZ);
	private_nh.param("min_x_size", m_minSizeX, m_minSizeX);
	private_nh.param("min_y_size", m_minSizeY, m_minSizeY);

	private_nh.param("filter_speckles", m_filterSpeckles, m_filterSpeckles);
	private_nh.param("filter_ground", m_filterGroundPlane, m_filterGroundPlane);
	// distance of points from plane for RANSAC
	private_nh.param("ground_filter/distance", m_groundFilterDistance, m_groundFilterDistance);
	// angular derivation of found plane:
	private_nh.param("ground_filter/angle", m_groundFilterAngle, m_groundFilterAngle);
	// distance of found plane from z=0 to be detected as ground (e.g. to exclude tables)
	private_nh.param("ground_filter/plane_distance", m_groundFilterPlaneDistance, m_groundFilterPlaneDistance);

	private_nh.param("sensor_model/max_range", m_maxRange, m_maxRange);

	private_nh.param("resolution", m_res, m_res);
	private_nh.param("sensor_model/hit", m_probHit, m_probHit);
	private_nh.param("sensor_model/miss", m_probMiss, m_probMiss);
	private_nh.param("sensor_model/min", m_thresMin, m_thresMin);
	private_nh.param("sensor_model/max", m_thresMax, m_thresMax);
	private_nh.param("compress_map", m_compressMap, m_compressMap);
	private_nh.param("incremental_2D_projection", m_incrementalUpdate, m_incrementalUpdate);

	private_nh.param("unknown_cost", m_unknownCost, m_unknownCost);

	double rate;
	private_nh.param("rate", rate, 5.0);

	if (m_filterGroundPlane && (m_pointcloudMinZ > 0.0 || m_pointcloudMaxZ < 0.0)) {
		ROS_WARN_STREAM("You enabled ground filtering but incoming pointclouds will be pre-filtered in [" <<
				m_pointcloudMinZ <<", "<< m_pointcloudMaxZ << "], excluding the ground level z=0. " <<
				"This will not work.");
	}

	// initialize octomap object & params
	m_octree = new OcTreeT(m_res);
	m_octree->setProbHit(m_probHit);
	m_octree->setProbMiss(m_probMiss);
	m_octree->setClampingThresMin(m_thresMin);
	m_octree->setClampingThresMax(m_thresMax);
	m_treeDepth = m_octree->getTreeDepth();
	m_maxTreeDepth = m_treeDepth;
	m_gridmap.info.resolution = m_res;

	double r, g, b, a;
	private_nh.param("color/r", r, 0.0);
	private_nh.param("color/g", g, 0.0);
	private_nh.param("color/b", b, 1.0);
	private_nh.param("color/a", a, 1.0);
	m_color.r = r;
	m_color.g = g;
	m_color.b = b;
	m_color.a = a;

	private_nh.param("latch", m_latchedTopics, m_latchedTopics);
	if (m_latchedTopics) {
		ROS_INFO("Publishing latched (single publish will take longer, all topics are prepared)");
	} else
		ROS_INFO("Publishing non-latched (topics are only prepared as needed, will only be re-published on map change");

	m_markerPub = m_nh.advertise<visualization_msgs::MarkerArray>("occupied_cells_vis_array", 1, m_latchedTopics);
	m_binaryMapPub = m_nh.advertise<Octomap>("octomap_binary", 1, m_latchedTopics);
	m_fullMapPub = m_nh.advertise<Octomap>("octomap_full", 1, m_latchedTopics);
	m_pointCloudPub = m_nh.advertise<sensor_msgs::PointCloud2>("octomap_point_cloud_centers", 1, m_latchedTopics);
	//m_collisionObjectPub = m_nh.advertise<arm_navigation_msgs::CollisionObject>("octomap_collision_object", 1, m_latchedTopics);
	m_mapPub = m_nh.advertise<nav_msgs::OccupancyGrid>("projected_map", 5, m_latchedTopics);
	//m_cmapPub = m_nh.advertise<arm_navigation_msgs::CollisionMap>("collision_map_out", 1, m_latchedTopics);

	m_pointCloudSub = new message_filters::Subscriber<sensor_msgs::PointCloud2>(m_nh, "cloud_in", 5);
    m_pointCloudGroundSub = new message_filters::Subscriber<sensor_msgs::PointCloud2>(m_nh, "ground_cloud_in", 5);

	m_tfPointCloudSub = new tf::MessageFilter<sensor_msgs::PointCloud2>(*m_pointCloudSub, m_tfListener, m_worldFrameId, 5);
	m_tfPointCloudSub->registerCallback(boost::bind(&OctomapServer::insertCloudCallback, this, _1));

    m_tfPointCloudGroundSub = new tf::MessageFilter<sensor_msgs::PointCloud2>(*m_pointCloudGroundSub, m_tfListener, m_worldFrameId, 5);
    m_tfPointCloudGroundSub->registerCallback(boost::bind(&OctomapServer::insertCloudGroundCallback, this, _1));

	m_octomapBinaryService = m_nh.advertiseService("octomap_binary", &OctomapServer::octomapBinarySrv, this);
	m_octomapFullService = m_nh.advertiseService("octomap_full", &OctomapServer::octomapFullSrv, this);
	m_clearBBXService = private_nh.advertiseService("clear_bbx", &OctomapServer::clearBBXSrv, this);
	m_resetService = private_nh.advertiseService("reset", &OctomapServer::resetSrv, this);
	m_pruneService = private_nh.advertiseService("prune", &OctomapServer::pruneSrv, this);

	dynamic_reconfigure::Server<lrm_octomap_server::OctomapServerConfig>::CallbackType f;

	f = boost::bind(&OctomapServer::reconfigureCallback, this, _1, _2);
	m_reconfigureServer.setCallback(f);

	m_updated = false;
	m_publisher_timer = nh.createTimer(ros::Duration(1.0 / rate), &OctomapServer::timerCallback, this);
}

void OctomapServer::timerCallback(const ros::TimerEvent& t) {

	boost::lock_guard<boost::mutex> guard(m_mutex);

	if(m_updated) {
		_publishAll(ros::Time::now());
	}
}


OctomapServer::~OctomapServer() {
	if (m_tfPointCloudSub) {
		delete m_tfPointCloudSub;
		m_tfPointCloudSub = NULL;
	}

    if (m_tfPointCloudGroundSub) {
        delete m_tfPointCloudGroundSub;
        m_tfPointCloudGroundSub = NULL;
    }

	if (m_pointCloudSub) {
		delete m_pointCloudSub;
		m_pointCloudSub = NULL;
	}

    if (m_pointCloudGroundSub) {
        delete m_pointCloudGroundSub;
        m_pointCloudGroundSub = NULL;
    }

	if (m_octree) {
		delete m_octree;
		m_octree = NULL;
	}

}

bool OctomapServer::openFile(const std::string& filename) {
	if (filename.length() <= 3)
		return false;

	std::string suffix = filename.substr(filename.length() - 3, 3);
	if (suffix == ".bt") {
		if (!m_octree->readBinary(filename)) {
			return false;
		}
	} else if (suffix == ".ot") {
		AbstractOcTree* tree = AbstractOcTree::read(filename);
		if (!tree) {
			return false;
		}
		if (m_octree) {
			delete m_octree;
			m_octree = NULL;
		}
		m_octree = dynamic_cast<OcTreeT*>(tree);
		if (!m_octree) {
			ROS_ERROR("Could not read OcTree in file, currently there are no other types supported in .ot");
			return false;
		}

	} else {
		return false;
	}

	ROS_INFO("Octomap file %s loaded (%zu nodes).", filename.c_str(), m_octree->size());

	m_treeDepth = m_octree->getTreeDepth();
	m_maxTreeDepth = m_treeDepth;
	m_res = m_octree->getResolution();
	m_gridmap.info.resolution = m_res;
	double minX, minY, minZ;
	double maxX, maxY, maxZ;
	m_octree->getMetricMin(minX, minY, minZ);
	m_octree->getMetricMax(maxX, maxY, maxZ);

	m_updateBBXMin[0] = m_octree->coordToKey(minX);
	m_updateBBXMin[1] = m_octree->coordToKey(minY);
	m_updateBBXMin[2] = m_octree->coordToKey(minZ);

	m_updateBBXMax[0] = m_octree->coordToKey(maxX);
	m_updateBBXMax[1] = m_octree->coordToKey(maxY);
	m_updateBBXMax[2] = m_octree->coordToKey(maxZ);

	publishAll();

	return true;

}

void OctomapServer::insertCloudGroundCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud) {
    ros::WallTime startTime = ros::WallTime::now();

    //ROS_INFO_STREAM("ground in");

    //
    // ground filtering in base frame
    //
    PCLPointCloud pc; // input cloud for filtering and ground-detection
    pcl::fromROSMsg(*cloud, pc);

    PCLPointCloud pc_ground; // segmented ground plane
    PCLPointCloud pc_nonground; // everything else

    tf::StampedTransform sensorTf;

    try {
        m_tfListener.waitForTransform(m_worldFrameId, m_SourceFrameId, cloud->header.stamp, ros::Duration(1.0));
        m_tfListener.lookupTransform(m_worldFrameId, m_SourceFrameId, cloud->header.stamp, sensorTf);
    } catch (tf::TransformException& ex) {
        ROS_ERROR_STREAM(">Transform error of sensor data: " << ex.what() << ", quitting callback");
        return;
    }

    if(m_worldFrameId != cloud->header.frame_id) {
    	tf::StampedTransform sensorToWorldTf;
    	Eigen::Matrix4f sensorToWorld;
		try {
			m_tfListener.waitForTransform(m_worldFrameId, cloud->header.frame_id, cloud->header.stamp, ros::Duration(1.0));
			m_tfListener.lookupTransform(m_worldFrameId, cloud->header.frame_id, cloud->header.stamp, sensorToWorldTf);
		} catch (tf::TransformException& ex) {
			ROS_ERROR_STREAM("Transform error of sensor data: " << ex.what() << ", quitting callback");
			return;
		}
		pcl_ros::transformAsMatrix(sensorToWorldTf, sensorToWorld);
    	pcl::transformPointCloud(pc, pc, sensorToWorld);
    }

    pc_ground = pc;
    // pc_nonground is empty without ground segmentation
    pc_ground.header = pc.header;
    pc_nonground.header = pc.header;

    //insertScan(sensorToWorldTf.getOrigin(), pc_ground, pc_nonground);
    insertScan(sensorTf.getOrigin(), pc_ground, pc_nonground);

    double total_elapsed = (ros::WallTime::now() - startTime).toSec();

    ROS_DEBUG("Pointcloud insertion in OctomapServer done (%zu+%zu pts (ground/nonground), %f sec)", pc_ground.size(), pc_nonground.size(), total_elapsed);

    //publishAll(cloud->header.stamp);
}

void OctomapServer::insertCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud) {
	ros::WallTime startTime = ros::WallTime::now();

	//
	// ground filtering in base frame
	//
	PCLPointCloud pc; // input cloud for filtering and ground-detection
	pcl::fromROSMsg(*cloud, pc);


	//rlklaser/////////////////
	tf::StampedTransform sensorTf;
	try {
		m_tfListener.waitForTransform(m_worldFrameId, m_SourceFrameId, cloud->header.stamp, ros::Duration(1.0));
		m_tfListener.lookupTransform(m_worldFrameId, m_SourceFrameId, cloud->header.stamp, sensorTf);
	} catch (tf::TransformException& ex) {
		ROS_ERROR_STREAM(">Transform error of sensor data: " << ex.what() << ", quitting callback");
		return;
	}
	/////////////////

	// set up filter for height range, also removes NANs:
	pcl::PassThrough < pcl::PointXYZ > pass;
	pass.setFilterFieldName("z");
	pass.setFilterLimits(m_pointcloudMinZ, m_pointcloudMaxZ);

	PCLPointCloud pc_ground; // segmented ground plane
	PCLPointCloud pc_nonground; // everything else

	/*
	if (m_filterGroundPlane) {
		tf::StampedTransform sensorToBaseTf, baseToWorldTf;
		try {
			m_tfListener.waitForTransform(m_baseFrameId, cloud->header.frame_id, cloud->header.stamp, ros::Duration(0.2));
			m_tfListener.lookupTransform(m_baseFrameId, cloud->header.frame_id, cloud->header.stamp, sensorToBaseTf);
			m_tfListener.lookupTransform(m_worldFrameId, m_baseFrameId, cloud->header.stamp, baseToWorldTf);

		} catch (tf::TransformException& ex) {
			ROS_ERROR_STREAM("Transform error for ground plane filter: " << ex.what() << ", quitting callback.\n" "You need to set the base_frame_id or disable filter_ground.");
		}

		Eigen::Matrix4f sensorToBase, baseToWorld;
		pcl_ros::transformAsMatrix(sensorToBaseTf, sensorToBase);
		pcl_ros::transformAsMatrix(baseToWorldTf, baseToWorld);

		// transform pointcloud from sensor frame to fixed robot frame
		pcl::transformPointCloud(pc, pc, sensorToBase);
		pass.setInputCloud(pc.makeShared());
		pass.filter(pc);
		filterGroundPlane(pc, pc_ground, pc_nonground);

		// transform clouds to world frame for insertion
		pcl::transformPointCloud(pc_ground, pc_ground, baseToWorld);
		pcl::transformPointCloud(pc_nonground, pc_nonground, baseToWorld);
	}
	else
	*/
	{

		if(m_worldFrameId != cloud->header.frame_id) {
			tf::StampedTransform sensorToWorldTf;
			Eigen::Matrix4f sensorToWorld;
			try {
				m_tfListener.waitForTransform(m_worldFrameId, cloud->header.frame_id, cloud->header.stamp, ros::Duration(1.0));
				m_tfListener.lookupTransform(m_worldFrameId, cloud->header.frame_id, cloud->header.stamp, sensorToWorldTf);
			} catch (tf::TransformException& ex) {
				ROS_ERROR_STREAM("Transform error of sensor data: " << ex.what() << ", quitting callback");
				return;
			}
			pcl_ros::transformAsMatrix(sensorToWorldTf, sensorToWorld);
			// directly transform to map frame:
			pcl::transformPointCloud(pc, pc, sensorToWorld);
		}

		// just filter height range:
		if(m_pointcloudMinZ !=0 && m_pointcloudMaxZ != 0) {
			pass.setInputCloud(pc.makeShared());
			pass.filter(pc);
		}

		pc_nonground = pc;
		// pc_nonground is empty without ground segmentation
		pc_ground.header = pc.header;
		pc_nonground.header = pc.header;
	}

	//insertScan(sensorToWorldTf.getOrigin(), pc_ground, pc_nonground);
	insertScan(sensorTf.getOrigin(), pc_ground, pc_nonground);

	double total_elapsed = (ros::WallTime::now() - startTime).toSec();
	ROS_DEBUG("Pointcloud insertion in OctomapServer done (%zu+%zu pts (ground/nonground), %f sec)", pc_ground.size(), pc_nonground.size(), total_elapsed);

	publishAll(cloud->header.stamp);
}

OcTreeKey getIndexKey(const OcTreeKey & key, unsigned short depth) {
	// TODO: replace with helper function from next octomap (> 1.5)
	unsigned short int mask = 65535 << (16 - depth);
	OcTreeKey result = key;
	result[0] &= mask;
	result[1] &= mask;
	result[2] &= mask;
	return result;
}

void OctomapServer::insertScan(const tf::Point& sensorOriginTf, const PCLPointCloud& ground, const PCLPointCloud& nonground) {

	boost::lock_guard<boost::mutex> guard(m_mutex);

	point3d sensorOrigin = pointTfToOctomap(sensorOriginTf);

	if (!m_octree->coordToKeyChecked(sensorOrigin, m_updateBBXMin) || !m_octree->coordToKeyChecked(sensorOrigin, m_updateBBXMax)) {
		ROS_ERROR_STREAM("Could not generate Key for origin "<<sensorOrigin);
	}

	// instead of direct scan insertion, compute update to filter ground:
	KeySet free_cells, occupied_cells;
	// insert ground points only as free:
	for (PCLPointCloud::const_iterator it = ground.begin(); it != ground.end(); ++it) {
		point3d point(it->x, it->y, it->z);
		// maxrange check
		if ((m_maxRange > 0.0) && ((point - sensorOrigin).norm() > m_maxRange)) {
			point = sensorOrigin + (point - sensorOrigin).normalized() * m_maxRange;
		}

		// only clear space (ground points)
		if (m_octree->computeRayKeys(sensorOrigin, point, m_keyRay)) {
			free_cells.insert(m_keyRay.begin(), m_keyRay.end());
		}

		octomap::OcTreeKey endKey;
		if (m_octree->coordToKeyChecked(point, endKey)) {
			updateMinKey(endKey, m_updateBBXMin);
			updateMaxKey(endKey, m_updateBBXMax);
		} else {
			ROS_ERROR_STREAM("Could not generate Key for endpoint "<<point);
		}
	}

	// all other points: free on ray, occupied on endpoint:
	for (PCLPointCloud::const_iterator it = nonground.begin(); it != nonground.end(); ++it) {
		point3d point(it->x, it->y, it->z);
		// maxrange check
		if ((m_maxRange < 0.0) || ((point - sensorOrigin).norm() <= m_maxRange)) {

			// free cells
			if (m_octree->computeRayKeys(sensorOrigin, point, m_keyRay)) {
				free_cells.insert(m_keyRay.begin(), m_keyRay.end());
			}
			// occupied endpoint
			OcTreeKey key;
			if (m_octree->coordToKeyChecked(point, key)) {
				//m_octree->integrateNodeColor(key, it->r)
				occupied_cells.insert(key);

				updateMinKey(key, m_updateBBXMin);
				updateMaxKey(key, m_updateBBXMax);
			}
		} else { // ray longer than maxrange:;
			point3d new_end = sensorOrigin + (point - sensorOrigin).normalized() * m_maxRange;
			if (m_octree->computeRayKeys(sensorOrigin, new_end, m_keyRay)) {
				free_cells.insert(m_keyRay.begin(), m_keyRay.end());

				octomap::OcTreeKey endKey;
				if (m_octree->coordToKeyChecked(new_end, endKey)) {
					updateMinKey(endKey, m_updateBBXMin);
					updateMaxKey(endKey, m_updateBBXMax);
				} else {
					ROS_ERROR_STREAM("Could not generate Key for endpoint "<<new_end);
				}

			}
		}
	}

	int in = 0, jn = 0;

	// mark free cells only if not seen occupied in this cloud
	for (KeySet::iterator it = free_cells.begin(), end = free_cells.end(); it != end; ++it) {

		if (occupied_cells.find(*it) == occupied_cells.end()) {
			m_octree->updateNode(*it, false);
			in++;
		}
	}

	// now mark all occupied cells:
	for (KeySet::iterator it = occupied_cells.begin(), end = occupied_cells.end(); it != end; it++) {

		point3d point;
		const OcTreeKey k = *it;
		point = m_octree->keyToCoord(k);
		double dist = point.distance(sensorOrigin);

		float new_odds = std::max(  std::min(  (m_probHit / 2) + (1.0 / dist), 0.9), 0.51);

		m_octree->updateNode(*it, new_odds, true);
		jn++;
	}

	// TODO: eval lazy+updateInner vs. proper insertion
	// non-lazy by default (updateInnerOccupancy() too slow for large maps)
	//m_octree->updateInnerOccupancy();
	octomap::point3d minPt, maxPt;
	ROS_DEBUG_STREAM("Bounding box keys (before): " << m_updateBBXMin[0] << " " <<m_updateBBXMin[1] << " " << m_updateBBXMin[2] << " / " <<m_updateBBXMax[0] << " "<<m_updateBBXMax[1] << " "<< m_updateBBXMax[2]);


//	ROS_INFO_STREAM("occ updt:" << jn << " free uptd:" << in);

	// TODO: snap max / min keys to larger voxels by m_maxTreeDepth
//   if (m_maxTreeDepth < 16)
//   {
//      OcTreeKey tmpMin = getIndexKey(m_updateBBXMin, m_maxTreeDepth); // this should give us the first key at depth m_maxTreeDepth that is smaller or equal to m_updateBBXMin (i.e. lower left in 2D grid coordinates)
//      OcTreeKey tmpMax = getIndexKey(m_updateBBXMax, m_maxTreeDepth); // see above, now add something to find upper right
//      tmpMax[0]+= m_octree->getNodeSize( m_maxTreeDepth ) - 1;
//      tmpMax[1]+= m_octree->getNodeSize( m_maxTreeDepth ) - 1;
//      tmpMax[2]+= m_octree->getNodeSize( m_maxTreeDepth ) - 1;
//      m_updateBBXMin = tmpMin;
//      m_updateBBXMax = tmpMax;
//   }

	// TODO: we could also limit the bbx to be within the map bounds here (see publishing check)
	minPt = m_octree->keyToCoord(m_updateBBXMin);
	maxPt = m_octree->keyToCoord(m_updateBBXMax);
	ROS_DEBUG_STREAM("Updated area bounding box: "<< minPt << " - "<<maxPt);
	ROS_DEBUG_STREAM("Bounding box keys (after): " << m_updateBBXMin[0] << " " <<m_updateBBXMin[1] << " " << m_updateBBXMin[2] << " / " <<m_updateBBXMax[0] << " "<<m_updateBBXMax[1] << " "<< m_updateBBXMax[2]);

	if (m_compressMap)
		m_octree->prune();

}

void OctomapServer::publishAll(const ros::Time& rostime) {

	boost::lock_guard<boost::mutex> guard(m_mutex);

	m_updated = true;
	//_publishAll(rostime);
}

void OctomapServer::_publishAll(const ros::Time& rostime) {

	//boost::lock_guard<boost::mutex> guard(m_mutex);

	ros::WallTime startTime = ros::WallTime::now();
	size_t octomapSize = m_octree->size();
	// TODO: estimate num occ. voxels for size of arrays (reserve)
	if (octomapSize <= 1) {
		ROS_WARN("Nothing to publish, octree is empty");
		return;
	}

	//bool publishCollisionMap = (m_latchedTopics || m_cmapPub.getNumSubscribers() > 0);
	//bool publishCollisionObject = (m_latchedTopics || m_collisionObjectPub.getNumSubscribers() > 0);
	bool publishMarkerArray = (m_latchedTopics || m_markerPub.getNumSubscribers() > 0);
	bool publishPointCloud = (m_latchedTopics || m_pointCloudPub.getNumSubscribers() > 0);
	bool publishBinaryMap = (m_latchedTopics || m_binaryMapPub.getNumSubscribers() > 0);
	bool publishFullMap = (m_latchedTopics || m_fullMapPub.getNumSubscribers() > 0);
	m_publish2DMap = (m_latchedTopics || m_mapPub.getNumSubscribers() > 0);

	// init collision object:
	//arm_navigation_msgs::CollisionObject collisionObject;
	//collisionObject.header.frame_id = m_worldFrameId;
	//collisionObject.header.stamp = rostime;
	//collisionObject.id = "map";
	//arm_navigation_msgs::OrientedBoundingBox collObjBox;
	//collObjBox.axis.x = collObjBox.axis.y = 0.0;
	//collObjBox.axis.z = 1.0;
	//collObjBox.angle = 0.0;

	//init collision map:
	//arm_navigation_msgs::CollisionMap collisionMap;
	//collisionMap.header.frame_id = m_worldFrameId;
	//collisionMap.header.stamp = rostime;
	//arm_navigation_msgs::Shape collObjShape;
	//collObjShape.type = arm_navigation_msgs::Shape::BOX;
	//collObjShape.dimensions.resize(3);

	geometry_msgs::Pose pose;
	pose.orientation = tf::createQuaternionMsgFromYaw(0.0);

	// init markers:
	visualization_msgs::MarkerArray occupiedNodesVis;
	// each array stores all cubes of a different size, one for each depth level:
	occupiedNodesVis.markers.resize(m_treeDepth + 1);

	// init pointcloud:
	pcl::PointCloud < pcl::PointXYZ > pclCloud;

	// call pre-traversal hook:
	handlePreNodeTraversal(rostime);

	// now, traverse all leafs in the tree:
	for (OcTreeT::iterator it = m_octree->begin(m_maxTreeDepth), end = m_octree->end(); it != end; ++it) {
		bool inUpdateBBX = isInUpdateBBX(it.getKey());

		// call general hook:
		handleNode(it);
		if (inUpdateBBX)
			handleNodeInBBX(it);

		if (m_octree->isNodeOccupied(*it)) {
			double z = it.getZ();
			if (z > m_occupancyMinZ && z < m_occupancyMaxZ) {
				double size = it.getSize();
				double x = it.getX();
				double y = it.getY();

				// Ignore speckles in the map:
				if (m_filterSpeckles && (it.getDepth() == m_treeDepth + 1) && isSpeckleNode(it.getKey())) {
					ROS_DEBUG("Ignoring single speckle at (%f,%f,%f)", x, y, z);
					continue;
				} // else: current octree node is no speckle, send it out

				handleOccupiedNode(it);
				if (inUpdateBBX)
					handleOccupiedNodeInBBX(it);

				// create collision object:
				//if (publishCollisionObject) {
				//	collObjShape.dimensions[0] = collObjShape.dimensions[1] = collObjShape.dimensions[2] = size;
				//	collisionObject.shapes.push_back(collObjShape);
				//	pose.position.x = x;
				//	pose.position.y = y;
				//	pose.position.z = z;
				//	collisionObject.poses.push_back(pose);
				//}

				//if (publishCollisionMap) {
				//	collObjBox.extents.x = collObjBox.extents.y = collObjBox.extents.z = size;
				//	collObjBox.center.x = x;
				//	collObjBox.center.y = y;
				//	collObjBox.center.z = z;
				//	collisionMap.boxes.push_back(collObjBox);
				//}

				//create marker:
				if (publishMarkerArray) {
					unsigned idx = it.getDepth();
					assert(idx < occupiedNodesVis.markers.size());

					geometry_msgs::Point cubeCenter;
					cubeCenter.x = x;
					cubeCenter.y = y;
					cubeCenter.z = z;

					occupiedNodesVis.markers[idx].points.push_back(cubeCenter);
					if (m_useHeightMap) {
						double minX, minY, minZ, maxX, maxY, maxZ;
						m_octree->getMetricMin(minX, minY, minZ);
						m_octree->getMetricMax(maxX, maxY, maxZ);

						double h = (1.0 - std::min(std::max((cubeCenter.z - minZ) / (maxZ - minZ), 0.0), 1.0)) * m_colorFactor;
						occupiedNodesVis.markers[idx].colors.push_back(heightMapColor(h));
					}
				}

				// insert into pointcloud:
				if (publishPointCloud)
					pclCloud.push_back(pcl::PointXYZ(x, y, z));

			}
		} else { // node not occupied => mark as free in 2D map if unknown so far
			handleFreeNode(it);
			if (inUpdateBBX)
				handleFreeNodeInBBX(it);
		}
	}

	// call post-traversal hook:
	handlePostNodeTraversal(rostime);

	// finish MarkerArray:
	if (publishMarkerArray) {
		for (unsigned i = 0; i < occupiedNodesVis.markers.size(); ++i) {
			double size = m_octree->getNodeSize(i);

			occupiedNodesVis.markers[i].header.frame_id = m_worldFrameId;
			occupiedNodesVis.markers[i].header.stamp = rostime;
			occupiedNodesVis.markers[i].ns = "map";
			occupiedNodesVis.markers[i].id = i;
			occupiedNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
			occupiedNodesVis.markers[i].scale.x = size;
			occupiedNodesVis.markers[i].scale.y = size;
			occupiedNodesVis.markers[i].scale.z = size;
			occupiedNodesVis.markers[i].color = m_color;

			if (occupiedNodesVis.markers[i].points.size() > 0)
				occupiedNodesVis.markers[i].action = visualization_msgs::Marker::ADD;
			else
				occupiedNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;
		}

		m_markerPub.publish(occupiedNodesVis);
	}

	// finish pointcloud:
	if (publishPointCloud) {
		sensor_msgs::PointCloud2 cloud;
		pcl::toROSMsg(pclCloud, cloud);
		cloud.header.frame_id = m_worldFrameId;
		cloud.header.stamp = rostime;
		m_pointCloudPub.publish(cloud);
	}

	//if (publishCollisionObject)
	//	m_collisionObjectPub.publish(collisionObject);

	//if (publishCollisionMap)
	//	m_cmapPub.publish(collisionMap);

	if (publishBinaryMap)
		publishBinaryOctoMap(rostime);

	if (publishFullMap)
		publishFullOctoMap(rostime);

	double total_elapsed = (ros::WallTime::now() - startTime).toSec();
	ROS_DEBUG("Map publishing in OctomapServer took %f sec", total_elapsed);

	m_updated = false;
}

bool OctomapServer::octomapBinarySrv(OctomapSrv::Request &req, OctomapSrv::Response &res) {
	ROS_INFO("Sending binary map data on service request");
	res.map.header.frame_id = m_worldFrameId;
	res.map.header.stamp = ros::Time::now();
	if (!octomap_msgs::binaryMapToMsg(*m_octree, res.map))
		return false;

	return true;
}

bool OctomapServer::octomapFullSrv(OctomapSrv::Request &req, OctomapSrv::Response &res) {
	ROS_INFO("Sending full map data on service request");
	res.map.header.frame_id = m_worldFrameId;
	res.map.header.stamp = ros::Time::now();

	if (!octomap_msgs::fullMapToMsg(*m_octree, res.map))
		return false;

	return true;
}

bool OctomapServer::clearBBXSrv(BBXSrv::Request& req, BBXSrv::Response& resp) {
	point3d min = pointMsgToOctomap(req.min);
	point3d max = pointMsgToOctomap(req.max);

	ROS_DEBUG_STREAM("clear_bbx called " << min.x() << ":" << max.x());

	boost::lock_guard<boost::mutex> guard(m_mutex);

	for (OcTreeT::leaf_bbx_iterator it = m_octree->begin_leafs_bbx(min, max), end = m_octree->end_leafs_bbx(); it != end; ++it) {
		//it->setLogOdds(octomap::logodds(m_thresMin));
		//			m_octree->updateNode(it.getKey(), -6.0f);
		//ROS_INFO_STREAM("it: " << m_thresMin);

		//free space
		//it->setLogOdds(octomap::logodds(0));
		m_octree->updateNode(it.getKey(), octomap::logodds(0));
	}

	// TODO: eval which is faster (setLogOdds+updateInner or updateNode)
	//m_octree->updateInnerOccupancy();

	//rlklaser:: let next read publish all
	//publishAll(ros::Time::now());

	ROS_DEBUG_STREAM("octree updated");

	return true;
}

bool OctomapServer::resetSrv(std_srvs::Empty::Request& req, std_srvs::Empty::Response& resp) {
	visualization_msgs::MarkerArray occupiedNodesVis;
	occupiedNodesVis.markers.resize(m_treeDepth + 1);
	ros::Time rostime = ros::Time::now();

	boost::lock_guard<boost::mutex>* guard = new boost::lock_guard<boost::mutex>(m_mutex);
	//boost::lock_guard<boost::mutex> guard(m_mutex);

	m_octree->clear();

	// clear 2D map:
	m_gridmap.data.clear();
	m_gridmap.info.height = 0.0;
	m_gridmap.info.width = 0.0;
	m_gridmap.info.resolution = 0.0;
	m_gridmap.info.origin.position.x = 0.0;
	m_gridmap.info.origin.position.y = 0.0;

	//rlklaser
	m_gridmap.data.resize(m_gridmap.info.width * m_gridmap.info.height, m_unknownCost);

	ROS_INFO("Cleared octomap");

	//publishBinaryOctoMap(rostime);

	for (unsigned i = 0; i < occupiedNodesVis.markers.size(); ++i) {

		occupiedNodesVis.markers[i].header.frame_id = m_worldFrameId;
		occupiedNodesVis.markers[i].header.stamp = rostime;
		occupiedNodesVis.markers[i].ns = "map";
		occupiedNodesVis.markers[i].id = i;
		occupiedNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
		occupiedNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;
	}

	m_markerPub.publish(occupiedNodesVis);

	delete guard;

	publishAll(rostime);

	ROS_DEBUG_STREAM("octree reset finished");

	return true;
}

bool OctomapServer::pruneSrv(std_srvs::Empty::Request& req, std_srvs::Empty::Response& resp) {
	ROS_INFO("pruning octomap...");

	boost::lock_guard<boost::mutex> guard(m_mutex);

	m_octree->prune();

	ROS_INFO("pruned.");

	return true;
}

void OctomapServer::publishBinaryOctoMap(const ros::Time& rostime) const {

	Octomap map;
	map.header.frame_id = m_worldFrameId;
	map.header.stamp = rostime;

	if (octomap_msgs::binaryMapToMsg(*m_octree, map))
		m_binaryMapPub.publish(map);
	else
		ROS_ERROR("Error serializing OctoMap");
}

void OctomapServer::publishFullOctoMap(const ros::Time& rostime) const {

	Octomap map;
	map.header.frame_id = m_worldFrameId;
	map.header.stamp = rostime;

	if (octomap_msgs::fullMapToMsg(*m_octree, map))
		m_fullMapPub.publish(map);
	else
		ROS_ERROR("Error serializing OctoMap");

}

void OctomapServer::filterGroundPlane(const PCLPointCloud& pc, PCLPointCloud& ground, PCLPointCloud& nonground) const {
	ground.header = pc.header;
	nonground.header = pc.header;

	if (pc.size() < 50) {
		ROS_WARN("Pointcloud in OctomapServer too small, skipping ground plane extraction");
		nonground = pc;
	} else {
		// plane detection for ground plane removal:
		pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
		pcl::PointIndices::Ptr inliers(new pcl::PointIndices);

		// Create the segmentation object and set up:
		pcl::SACSegmentation < pcl::PointXYZ > seg;
		seg.setOptimizeCoefficients(true);
		// TODO: maybe a filtering based on the surface normals might be more robust / accurate?
		seg.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
		seg.setMethodType(pcl::SAC_RANSAC);
		seg.setMaxIterations(200);
		seg.setDistanceThreshold(m_groundFilterDistance);
		seg.setAxis(Eigen::Vector3f(0, 0, 1));
		seg.setEpsAngle(m_groundFilterAngle);

		PCLPointCloud cloud_filtered(pc);
		// Create the filtering object
		pcl::ExtractIndices < pcl::PointXYZ > extract;
		bool groundPlaneFound = false;

		while (cloud_filtered.size() > 10 && !groundPlaneFound) {
			seg.setInputCloud(cloud_filtered.makeShared());
			seg.segment(*inliers, *coefficients);
			if (inliers->indices.size() == 0) {
				ROS_INFO("PCL segmentation did not find any plane.");

				break;
			}

			extract.setInputCloud(cloud_filtered.makeShared());
			extract.setIndices(inliers);

			if (std::abs(coefficients->values.at(3)) < m_groundFilterPlaneDistance) {
				ROS_DEBUG("Ground plane found: %zu/%zu inliers. Coeff: %f %f %f %f", inliers->indices.size(), cloud_filtered.size(), coefficients->values.at(0), coefficients->values.at(1), coefficients->values.at(2), coefficients->values.at(3));
				extract.setNegative(false);
				extract.filter(ground);

				// remove ground points from full pointcloud:
				// workaround for PCL bug:
				if (inliers->indices.size() != cloud_filtered.size()) {
					extract.setNegative(true);
					PCLPointCloud cloud_out;
					extract.filter(cloud_out);
					nonground += cloud_out;
					cloud_filtered = cloud_out;
				}

				groundPlaneFound = true;
			} else {
				ROS_DEBUG("Horizontal plane (not ground) found: %zu/%zu inliers. Coeff: %f %f %f %f", inliers->indices.size(), cloud_filtered.size(), coefficients->values.at(0), coefficients->values.at(1), coefficients->values.at(2), coefficients->values.at(3));
				pcl::PointCloud < pcl::PointXYZ > cloud_out;
				extract.setNegative(false);
				extract.filter(cloud_out);
				nonground += cloud_out;
				// debug
				//            pcl::PCDWriter writer;
				//            writer.write<pcl::PointXYZ>("nonground_plane.pcd",cloud_out, false);

				// remove current plane from scan for next iteration:
				// workaround for PCL bug:
				if (inliers->indices.size() != cloud_filtered.size()) {
					extract.setNegative(true);
					cloud_out.points.clear();
					extract.filter(cloud_out);
					cloud_filtered = cloud_out;
				} else {
					cloud_filtered.points.clear();
				}
			}

		}
		// TODO: also do this if overall starting pointcloud too small?
		if (!groundPlaneFound) { // no plane found or remaining points too small
			ROS_WARN("No ground plane found in scan");

			// do a rough fitlering on height to prevent spurious obstacles
			pcl::PassThrough < pcl::PointXYZ > second_pass;
			second_pass.setFilterFieldName("z");
			second_pass.setFilterLimits(-m_groundFilterPlaneDistance, m_groundFilterPlaneDistance);
			second_pass.setInputCloud(pc.makeShared());
			second_pass.filter(ground);

			second_pass.setFilterLimitsNegative(true);
			second_pass.filter(nonground);
		}

		// debug:
		//        pcl::PCDWriter writer;
		//        if (pc_ground.size() > 0)
		//          writer.write<pcl::PointXYZ>("ground.pcd",pc_ground, false);
		//        if (pc_nonground.size() > 0)
		//          writer.write<pcl::PointXYZ>("nonground.pcd",pc_nonground, false);

	}

}

void OctomapServer::handlePreNodeTraversal(const ros::Time& rostime) {
	if (m_publish2DMap) {
		// init projected 2D map:
		m_gridmap.header.frame_id = m_worldFrameId;
		m_gridmap.header.stamp = rostime;
		nav_msgs::MapMetaData oldMapInfo = m_gridmap.info;

		// TODO: move most of this stuff into c'tor and init map only once (adjust if size changes)
		double minX, minY, minZ, maxX, maxY, maxZ;
		m_octree->getMetricMin(minX, minY, minZ);
		m_octree->getMetricMax(maxX, maxY, maxZ);

		octomap::point3d minPt(minX, minY, minZ);
		octomap::point3d maxPt(maxX, maxY, maxZ);
		octomap::OcTreeKey minKey = m_octree->coordToKey(minPt, m_maxTreeDepth);
		octomap::OcTreeKey maxKey = m_octree->coordToKey(maxPt, m_maxTreeDepth);

		ROS_DEBUG("MinKey: %d %d %d / MaxKey: %d %d %d", minKey[0], minKey[1], minKey[2], maxKey[0], maxKey[1], maxKey[2]);

		// add padding if requested (= new min/maxPts in x&y):
		double halfPaddedX = 0.5 * m_minSizeX;
		double halfPaddedY = 0.5 * m_minSizeY;
		minX = std::min(minX, -halfPaddedX);
		maxX = std::max(maxX, halfPaddedX);
		minY = std::min(minY, -halfPaddedY);
		maxY = std::max(maxY, halfPaddedY);
		minPt = octomap::point3d(minX, minY, minZ);
		maxPt = octomap::point3d(maxX, maxY, maxZ);

		OcTreeKey paddedMaxKey;
		if (!m_octree->coordToKeyChecked(minPt, m_maxTreeDepth, m_paddedMinKey)) {
			ROS_ERROR("Could not create padded min OcTree key at %f %f %f", minPt.x(), minPt.y(), minPt.z());
			return;
		}
		if (!m_octree->coordToKeyChecked(maxPt, m_maxTreeDepth, paddedMaxKey)) {
			ROS_ERROR("Could not create padded max OcTree key at %f %f %f", maxPt.x(), maxPt.y(), maxPt.z());
			return;
		}

		ROS_DEBUG("Padded MinKey: %d %d %d / padded MaxKey: %d %d %d", m_paddedMinKey[0], m_paddedMinKey[1], m_paddedMinKey[2], paddedMaxKey[0], paddedMaxKey[1], paddedMaxKey[2]);
		assert(paddedMaxKey[0] >= maxKey[0] && paddedMaxKey[1] >= maxKey[1]);

		m_multires2DScale = 1 << (m_treeDepth - m_maxTreeDepth);
		m_gridmap.info.width = (paddedMaxKey[0] - m_paddedMinKey[0]) / m_multires2DScale + 1;
		m_gridmap.info.height = (paddedMaxKey[1] - m_paddedMinKey[1]) / m_multires2DScale + 1;

		int mapOriginX = minKey[0] - m_paddedMinKey[0];
		int mapOriginY = minKey[1] - m_paddedMinKey[1];
		assert(mapOriginX >= 0 && mapOriginY >= 0);

		// might not exactly be min / max of octree:
		octomap::point3d origin = m_octree->keyToCoord(m_paddedMinKey, m_treeDepth);
		double gridRes = m_octree->getNodeSize(m_maxTreeDepth);
		m_projectCompleteMap = (!m_incrementalUpdate || (std::abs(gridRes - m_gridmap.info.resolution) > 1e-6));
		m_gridmap.info.resolution = gridRes;
		m_gridmap.info.origin.position.x = origin.x() - gridRes * 0.5;
		m_gridmap.info.origin.position.y = origin.y() - gridRes * 0.5;
		if (m_maxTreeDepth != m_treeDepth) {
			m_gridmap.info.origin.position.x -= m_res / 2.0;
			m_gridmap.info.origin.position.y -= m_res / 2.0;
		}

		// workaround for  multires. projection not working properly for inner nodes:
		// force re-building complete map
		if (m_maxTreeDepth < m_treeDepth)
			m_projectCompleteMap = true;

		if (m_projectCompleteMap) {
			ROS_DEBUG("Rebuilding complete 2D map");
			m_gridmap.data.clear();
			// init to unknown:
			//m_gridmap.data.resize(m_gridmap.info.width * m_gridmap.info.height, -1);
//rlkaser:init to free
			m_gridmap.data.resize(m_gridmap.info.width * m_gridmap.info.height, MIN_COST);
		} else {

			if (mapChanged(oldMapInfo, m_gridmap.info)) {
				ROS_DEBUG("2D grid map size changed to %dx%d", m_gridmap.info.width, m_gridmap.info.height);
				adjustMapData(m_gridmap, oldMapInfo);
			}
			nav_msgs::OccupancyGrid::_data_type::iterator startIt;
			size_t mapUpdateBBXMinX = std::max(0, (int(m_updateBBXMin[0]) - int(m_paddedMinKey[0])) / int(m_multires2DScale));
			size_t mapUpdateBBXMinY = std::max(0, (int(m_updateBBXMin[1]) - int(m_paddedMinKey[1])) / int(m_multires2DScale));
			size_t mapUpdateBBXMaxX = std::min(int(m_gridmap.info.width - 1), (int(m_updateBBXMax[0]) - int(m_paddedMinKey[0])) / int(m_multires2DScale));
			size_t mapUpdateBBXMaxY = std::min(int(m_gridmap.info.height - 1), (int(m_updateBBXMax[1]) - int(m_paddedMinKey[1])) / int(m_multires2DScale));

			assert(mapUpdateBBXMaxX > mapUpdateBBXMinX);
			assert(mapUpdateBBXMaxY > mapUpdateBBXMinY);

			size_t numCols = mapUpdateBBXMaxX - mapUpdateBBXMinX + 1;

			// test for max idx:
			uint max_idx = m_gridmap.info.width * mapUpdateBBXMaxY + mapUpdateBBXMaxX;
			if (max_idx >= m_gridmap.data.size())
				ROS_ERROR("BBX index not valid: %d (max index %zu for size %d x %d) update-BBX is: [%zu %zu]-[%zu %zu]", max_idx, m_gridmap.data.size(), m_gridmap.info.width, m_gridmap.info.height, mapUpdateBBXMinX, mapUpdateBBXMinY, mapUpdateBBXMaxX, mapUpdateBBXMaxY);

			// reset proj. 2D map in bounding box:
			for (unsigned int j = mapUpdateBBXMinY; j <= mapUpdateBBXMaxY; ++j) {
				//std::fill_n(m_gridmap.data.begin() + m_gridmap.info.width * j + mapUpdateBBXMinX, numCols, -1);
//rlkaser:init to free
				std::fill_n(m_gridmap.data.begin() + m_gridmap.info.width * j + mapUpdateBBXMinX, numCols, MIN_COST);
			}

		}

	}

}

void OctomapServer::handlePostNodeTraversal(const ros::Time& rostime) {

	if (m_publish2DMap)
		m_mapPub.publish(m_gridmap);
}

void OctomapServer::handleOccupiedNode(const OcTreeT::iterator& it) {

	if (m_publish2DMap && m_projectCompleteMap) {
		update2DMap(it, true);
	}
}

void OctomapServer::handleFreeNode(const OcTreeT::iterator& it) {

	if (m_publish2DMap && m_projectCompleteMap) {
		update2DMap(it, false);
	}
}

void OctomapServer::handleOccupiedNodeInBBX(const OcTreeT::iterator& it) {

	if (m_publish2DMap && !m_projectCompleteMap) {
		update2DMap(it, true);
	}
}

void OctomapServer::handleFreeNodeInBBX(const OcTreeT::iterator& it) {

	if (m_publish2DMap && !m_projectCompleteMap) {
		update2DMap(it, false);
	}
}

void OctomapServer::update2DMap(const OcTreeT::iterator& it, bool occupied) {

	// update 2D map (occupied always overrides):

	if (it.getDepth() == m_maxTreeDepth) {
		unsigned idx = mapIdx(it.getKey());
		if (occupied) {
			m_gridmap.data[mapIdx(it.getKey())] = 100;
		}
//		else if (m_gridmap.data[idx] == -1) {
//			m_gridmap.data[idx] = 0;
//		}
//rlklaser:loss of occupancy
		else {
			m_gridmap.data[idx] *= 0.8;
		}

	} else {
		int intSize = 1 << (m_maxTreeDepth - it.getDepth());
		octomap::OcTreeKey minKey = it.getIndexKey();
		for (int dx = 0; dx < intSize; dx++) {
			int i = (minKey[0] + dx - m_paddedMinKey[0]) / m_multires2DScale;
			for (int dy = 0; dy < intSize; dy++) {
				unsigned idx = mapIdx(i, (minKey[1] + dy - m_paddedMinKey[1]) / m_multires2DScale);
				if (occupied) {
					m_gridmap.data[idx] = 100;
				}
//				else if (m_gridmap.data[idx] == -1) {
//					m_gridmap.data[idx] = 0;
//				}
//rlklaser:loss of occupancy
				else {
					m_gridmap.data[idx] *= 0.8;
				}
			}
		}
	}

}

bool OctomapServer::isSpeckleNode(const OcTreeKey&nKey) const {
	OcTreeKey key;
	bool neighborFound = false;
	for (key[2] = nKey[2] - 1; !neighborFound && key[2] <= nKey[2] + 1; ++key[2]) {
		for (key[1] = nKey[1] - 1; !neighborFound && key[1] <= nKey[1] + 1; ++key[1]) {
			for (key[0] = nKey[0] - 1; !neighborFound && key[0] <= nKey[0] + 1; ++key[0]) {
				if (key != nKey) {
					OcTreeNode* node = m_octree->search(key);
					if (node && m_octree->isNodeOccupied(node)) {
						// we have a neighbor => break!
						neighborFound = true;
					}
				}
			}
		}
	}

	return neighborFound;
}

void OctomapServer::reconfigureCallback(lrm_octomap_server::OctomapServerConfig& config, uint32_t level) {
	if (m_maxTreeDepth != unsigned(config.max_depth)) {
		m_maxTreeDepth = unsigned(config.max_depth);

		publishAll();
	}

}

void OctomapServer::adjustMapData(nav_msgs::OccupancyGrid& map, const nav_msgs::MapMetaData& oldMapInfo) const {
	if (map.info.resolution != oldMapInfo.resolution) {
		ROS_ERROR("Resolution of map changed, cannot be adjusted");
		return;
	}

	int i_off = int((oldMapInfo.origin.position.x - map.info.origin.position.x) / map.info.resolution + 0.5);
	int j_off = int((oldMapInfo.origin.position.y - map.info.origin.position.y) / map.info.resolution + 0.5);

	if (i_off < 0 || j_off < 0 || oldMapInfo.width + i_off > map.info.width || oldMapInfo.height + j_off > map.info.height) {
		ROS_ERROR("New 2D map does not contain old map area, this case is not implemented");
		return;
	}

	nav_msgs::OccupancyGrid::_data_type oldMapData = map.data;

	map.data.clear();
	// init to unknown:
	map.data.resize(map.info.width * map.info.height, m_unknownCost);

	nav_msgs::OccupancyGrid::_data_type::iterator fromStart, fromEnd, toStart;

	for (int j = 0; j < int(oldMapInfo.height); ++j) {
		// copy chunks, row by row:
		fromStart = oldMapData.begin() + j * oldMapInfo.width;
		fromEnd = fromStart + oldMapInfo.width;
		toStart = map.data.begin() + ((j + j_off) * m_gridmap.info.width + i_off);
		copy(fromStart, fromEnd, toStart);

//    for (int i =0; i < int(oldMapInfo.width); ++i){
//      map.data[m_gridmap.info.width*(j+j_off) +i+i_off] = oldMapData[oldMapInfo.width*j +i];
//    }

	}

}

std_msgs::ColorRGBA OctomapServer::heightMapColor(double h) {

	std_msgs::ColorRGBA color;
	color.a = 1.0;
	// blend over HSV-values (more colors)

	double s = 1.0;
	double v = 1.0;

	h -= floor(h);
	h *= 6;
	int i;
	double m, n, f;

	i = floor(h);
	f = h - i;
	if (!(i & 1))
		f = 1 - f; // if i is even
	m = v * (1 - s);
	n = v * (1 - s * f);

	switch (i) {
	case 6:
	case 0:
		color.r = v;
		color.g = n;
		color.b = m;
		break;
	case 1:
		color.r = n;
		color.g = v;
		color.b = m;
		break;
	case 2:
		color.r = m;
		color.g = v;
		color.b = n;
		break;
	case 3:
		color.r = m;
		color.g = n;
		color.b = v;
		break;
	case 4:
		color.r = n;
		color.g = m;
		color.b = v;
		break;
	case 5:
		color.r = v;
		color.g = m;
		color.b = n;
		break;
	default:
		color.r = 1;
		color.g = 0.5;
		color.b = 0.5;
		break;
	}

	return color;
}
}

