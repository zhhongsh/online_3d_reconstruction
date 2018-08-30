/* STEPS:
*
* 1. extract 2D features
* 2. find feature correspondence
* 3. convert corresponding features to 3D using disparity image information
* 4. find transformation between corresponding 3D points using estimateAffine3D: Output 3D affine transformation matrix  3 x 4
* 5. use decomposeProjectionMatrix to get rotation or Euler angles
*
*
*
* */

#include "pose.h"
#include <boost/filesystem.hpp>

Pose::Pose(int argc, char* argv[])
{
	if (parseCmdArgs(argc, argv) == -1) return;
	
	if (visualize)
	{
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb = read_PLY_File(read_PLY_filename0);
		if(displayUAVPositions)
			hexPos_cloud = read_PLY_File(read_PLY_filename1);
		pcl::PolygonMesh mesh;
		//visualize_pt_cloud(true, cloudrgb, false, mesh, read_PLY_filename0);
		visualize_pt_cloud(cloudrgb, read_PLY_filename0);
		return;
	}
	
	if (smooth_surface)
	{
		smoothPtCloud();
		return;
	}
	
	currentDateTimeStr = currentDateTime();
	cout << "currentDateTime=" << currentDateTimeStr << "\n\n";
	
	//create directory
	folder = folder + currentDateTimeStr + "/";
	boost::filesystem::path dir(folder);
	if(boost::filesystem::create_directory(dir)) {
		cout << "Created save directory " << folder << endl;
	}
	else {
		cout << "Could not create save directory!" << folder << endl;
		return;
	}
	save_log_to = folder + "log.txt";
	
#if 0
	cv::setBreakOnError(true);
#endif
	
	if (downsample)
	{
		//read cloud
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb = read_PLY_File(read_PLY_filename0);
		
		//downsample cloud
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_filtered = downsamplePtCloud(cloudrgb, false);
		
		string writePath = "downsampled_" + read_PLY_filename0;
		save_pt_cloud_to_PLY_File(cloudrgb_filtered, writePath);
		
		pcl::PolygonMesh mesh;
		visualize_pt_cloud(true, cloudrgb_filtered, false, mesh, "Downsampled Point Cloud");
		
		cout << "Cya!" << endl;
		return;
	}
	
	if(mesh_surface)
	{
		void meshSurface();
		return;
	}
	
	if (align_point_cloud)
	{
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_in = read_PLY_File(read_PLY_filename0);
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_out = read_PLY_File(read_PLY_filename1);
		
		pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4 tf_icp = runICPalignment(cloud_in, cloud_out);
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_Fitted(new pcl::PointCloud<pcl::PointXYZRGB> ());
		transformPtCloud(cloud_in, cloud_Fitted, tf_icp);
		
		string writePath = "ICP_aligned_" + read_PLY_filename0;
		save_pt_cloud_to_PLY_File(cloud_Fitted, writePath);
		
		pcl::PolygonMesh mesh;
		visualize_pt_cloud(true, cloud_Fitted, false, mesh, writePath);
		
		return;
	}
	
	readCalibFile();
	readPoseFile();
	populateData();
	
	//start program
	int64 app_start_time = getTickCount();
	
	//initialize some variables
	finder = makePtr<OrbFeaturesFinder>();
	//for (int i = 0; i < 6; i++)
	//{
	//	Ptr<FeaturesFinder> finder = makePtr<OrbFeaturesFinder>();
	//	finderVec.push_back(finder);
	//}
	
	features = vector<ImageFeatures>(img_numbers.size());
	for (int i = 0; i < img_numbers.size(); i++)
	{
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr keypoints3dptcloud (new pcl::PointCloud<pcl::PointXYZRGB> ());
		keypoints3dptcloud->is_dense = true;
		keypoints3DVec.push_back(keypoints3dptcloud);
	}
	
	//main point clouds
	//pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_MAVLink (new pcl::PointCloud<pcl::PointXYZRGB> ());
	//pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_FeatureMatched_big (new pcl::PointCloud<pcl::PointXYZRGB> ());
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_big (new pcl::PointCloud<pcl::PointXYZRGB> ());
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_small (new pcl::PointCloud<pcl::PointXYZRGB> ());
	cloud_big->is_dense = true;
	cloud_small->is_dense = true;
	//cloudrgb_MAVLink->is_dense = true;
	//cloudrgb_FeatureMatched_big->is_dense = true;
	
	//vectors to store transformations of point clouds
	vector<pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4> t_matVec;
	vector<pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4> t_FMVec;
	
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_hexPos_MAVLink(new pcl::PointCloud<pcl::PointXYZRGB> ());
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_hexPos_FM(new pcl::PointCloud<pcl::PointXYZRGB> ());
	cloud_hexPos_MAVLink->is_dense = true;
	cloud_hexPos_FM->is_dense = true;
	
	int n_cycle;
	if(online)
		n_cycle = ceil(1.0 * img_numbers.size() / seq_len);
	else
		n_cycle = 1;
	
	boost::thread the_visualization_thread;
	
	bool log_uav_positions = false;
	
	for (int cycle = 0; cycle < n_cycle; cycle++)
	{
		int64 t0 = getTickCount();
		
		start_idx = cycle * seq_len;
		
		if(online)
			end_idx = min((cycle + 1) * seq_len - 1, (int)(img_numbers.size()) - 1);
		else
			end_idx = img_numbers.size() - 1;
		
		cout << "\nCycle " << cycle << " : Images " << start_idx << " to " << end_idx << endl;
		log_file << "\nCycle " << cycle << " : Images " << start_idx << " to " << end_idx << endl;
		
		findFeatures();
		
		int64 t1 = getTickCount();
		cout << "\nFinding features time: " << (t1 - t0) / getTickFrequency() << " sec\n" << endl;
		log_file << "Finding features time:\t\t\t\t" << (t1 - t0) / getTickFrequency() << " sec" << endl;

		if(log_uav_positions) log_file << "\nrecorded hexacopter positions" << endl;
		
		for (int i = start_idx; i < end_idx + 1; i++)
		{
			//SEARCH PROCESS: get NSECS from images_times_data and search for corresponding or nearby entry in pose_data and heading_data
			int pose_index = data_index_finder(img_numbers[i]);
			pcl::PointXYZRGB hexPosMAVLink = addPointFromPoseFile(pose_index);
			cloud_hexPos_MAVLink->points.push_back(hexPosMAVLink);
			if(log_uav_positions) log_file << img_numbers[i] << "," << pose_data[pose_index][tx_ind] << "," << pose_data[pose_index][ty_ind] << "," << pose_data[pose_index][tz_ind] << endl;
			
			pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4 t_mat = generateTmat(pose_data[pose_index]);
			t_matVec.push_back(t_mat);
			
			if (i == 0)
			{
				t_FMVec.push_back(t_mat);
				cloud_hexPos_FM->points.push_back(hexPosMAVLink);
				continue;
			}
			
			//Feature Matching Alignment
			//generate point clouds of matched keypoints and estimate rigid body transform between them
			pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4 T_SVD_matched_pts = generate_tf_of_Matched_Keypoints_Point_Cloud(i, t_FMVec, t_mat);
			
			t_FMVec.push_back(T_SVD_matched_pts * t_mat);
			
			pcl::PointXYZRGB hexPosFM = transformPoint(hexPosMAVLink, T_SVD_matched_pts);
			cloud_hexPos_FM->points.push_back(hexPosFM);
		}
		int64 t2 = getTickCount();
		cout << "\nMatching features and finding transformations time: " << (t2 - t1) / getTickFrequency() << " sec\n" << endl;
		log_file << "Matching features n transformations time:\t" << (t2 - t1) / getTickFrequency() << " sec" << endl;
		
		////finding normals of the hexPos
		//cout << "cloud_hexPos_FM: ";
		//findNormalOfPtCloud(cloud_hexPos_FM);
		//cout << "cloud_hexPos_MAVLink: ";
		//findNormalOfPtCloud(cloud_hexPos_MAVLink);
		
		if(log_uav_positions)
		{
			log_file << "\nfeature matched hexacopter positions" << endl;
			for (int i = start_idx; i < end_idx + 1; i++)
				log_file << img_numbers[i] << "," << cloud_hexPos_FM->points[i].x << "," << cloud_hexPos_FM->points[i].y << "," << cloud_hexPos_FM->points[i].z << endl;
		}
		
		//transforming the camera positions using ICP
		pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4 tf_icp = runICPalignment(cloud_hexPos_FM, cloud_hexPos_MAVLink);
		
		//correcting old point cloud
		transformPtCloud(cloud_big, cloud_big, tf_icp);
		
		//correcting old tf_mats
		for (int i = 0; i < end_idx + 1; i++)
			t_FMVec[i] = tf_icp * t_FMVec[i];
		
		//fit FM camera positions to MAVLink camera positions using ICP and use the tf to correct point cloud
		transformPtCloud(cloud_hexPos_FM, cloud_hexPos_FM, tf_icp);
		
		if(log_uav_positions)
		{
			log_file << "\ncorrected hexacopter positions" << endl;
			for (int i = start_idx; i < end_idx + 1; i++)
				log_file << img_numbers[i] << "," << cloud_hexPos_FM->points[i].x << "," << cloud_hexPos_FM->points[i].y << "," << cloud_hexPos_FM->points[i].z << endl;
		}
		
		int64 t3 = getTickCount();
		cout << "\nICP alignment and point cloud correction time: " << (t3 - t2) / getTickFrequency() << " sec\n" << endl;
		log_file << "ICP point cloud correction time:\t\t" << (t3 - t2) / getTickFrequency() << " sec" << endl;
		
		//add additional correction for each individual UAV position -> translate back to MAVLink location by a percentage value
		//find distance and then translate
		//for (int i = 0; i < end_idx + 1; i++)
		//{
		//	double delx = cloud_hexPos_MAVLink->points[i].x - cloud_hexPos_FM->points[i].x;
		//	double dely = cloud_hexPos_MAVLink->points[i].y - cloud_hexPos_FM->points[i].y;
		//	double delz = cloud_hexPos_MAVLink->points[i].z - cloud_hexPos_FM->points[i].z;
		//	double linear_dist = sqrt(delx*delx + dely*dely);
		//	double linear_dist_threshold = 0;
		//	if (linear_dist > linear_dist_threshold)
		//	{
		//		double K = 0.5;
		//		pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4 t_correct;
		//		for (int i = 0; i < 4; i++)
		//			for (int j = 0; j < 4; j++)
		//				t_correct(i,j) = 0;
		//		t_correct(0,0) = t_correct(1,1) = t_correct(2,2) = t_correct(3,3) = 1.0;
		//		t_correct(0,3) = K * delx;
		//		t_correct(1,3) = K * dely;
		//		//t_correct(2,3) = K * delz;
		//		t_FMVec[i] = t_correct * t_FMVec[i];
		//		cloud_hexPos_FM->points[i].x += K * delx;
		//		cloud_hexPos_FM->points[i].y += K * dely;
		//		//cloud_hexPos_FM->points[i].z += K * delz;
		//	}
		//}
		
		//adding new points to point cloud
		cout << "Adding Point Cloud number/points ";
		
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_FeatureMatched (new pcl::PointCloud<pcl::PointXYZRGB> ());
		
		//log_file << "Adding Point Cloud number/points ";
		int i = start_idx;
		while(i < end_idx + 1)
		{
			int i0 = i;
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb1 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb2 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb3 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb4 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb5 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb6 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloudrgb7 ( new pcl::PointCloud<pcl::PointXYZRGB>() );
			
			boost::thread pt_cloud_thread1, pt_cloud_thread2, pt_cloud_thread3, pt_cloud_thread4, pt_cloud_thread5, pt_cloud_thread6, pt_cloud_thread7;
			
			pt_cloud_thread1 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb1);
			if(++i < end_idx + 1) pt_cloud_thread2 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb2);
			if(++i < end_idx + 1) pt_cloud_thread3 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb3);
			if(++i < end_idx + 1) pt_cloud_thread4 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb4);
			if(++i < end_idx + 1) pt_cloud_thread5 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb5);
			if(++i < end_idx + 1) pt_cloud_thread6 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb6);
			if(++i < end_idx + 1) pt_cloud_thread7 = boost::thread(&Pose::createAndTransformPtCloud, this, i, t_FMVec, transformed_cloudrgb7);
			
			
			//generating the bigger point cloud
			pt_cloud_thread1.join();
			i = i0;
			cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb1->begin(),transformed_cloudrgb1->end());
			pt_cloud_thread2.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb2->begin(),transformed_cloudrgb2->end());
			pt_cloud_thread3.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb3->begin(),transformed_cloudrgb3->end());
			pt_cloud_thread4.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb4->begin(),transformed_cloudrgb4->end());
			pt_cloud_thread5.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb5->begin(),transformed_cloudrgb5->end());
			pt_cloud_thread6.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb6->begin(),transformed_cloudrgb6->end());
			pt_cloud_thread7.join();
			if(++i < end_idx + 1) cloudrgb_FeatureMatched->insert(cloudrgb_FeatureMatched->end(),transformed_cloudrgb7->begin(),transformed_cloudrgb7->end());
			//cout << "Transformed and added." << endl;
		}
		
		int64 t4 = getTickCount();
		cout << "\n\nPoint Cloud Creation time: " << (t4 - t3) / getTickFrequency() << " sec" << endl;
		log_file << "Point Cloud Creation time:\t\t\t" << (t4 - t3) / getTickFrequency() << " sec" << endl;
		
		if(online)
		{
			cout << "joining..." << endl;
			//adding the new downsampled points to old downsampled cloud
			cloud_big->insert(cloud_big->end(),cloudrgb_FeatureMatched->begin(),cloudrgb_FeatureMatched->end());
		}
		else
		{
			//downsample
			wait_at_visualizer = false;
			cout << "downsampling..." << endl;
			cloud_small = downsamplePtCloud(cloudrgb_FeatureMatched, true);
			//adding the new downsampled points to old downsampled cloud
			cloud_big->insert(cloud_big->end(),cloudrgb_FeatureMatched->begin(),cloudrgb_FeatureMatched->end());
		}
		
		int64 t5 = getTickCount();
		
		cout << "\nDownsampling/Joining time: " << (t5 - t4) / getTickFrequency() << " sec" << endl;
		log_file << "Downsampling/Joining time:\t\t\t" << (t5 - t4) / getTickFrequency() << " sec" << endl;
		
		//visualize
		if(preview)
		{
			if (online)
			{
				pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_big_copy (new pcl::PointCloud<pcl::PointXYZRGB>());
				copyPointCloud(*cloud_big, *cloud_big_copy);
				
				if(cycle > 0)
					the_visualization_thread.join();
				
				the_visualization_thread = boost::thread(&Pose::displayPointCloudOnline, this, cloud_big_copy, cloud_hexPos_FM, cloud_hexPos_MAVLink, cycle, n_cycle);
			}
			else
			{
				the_visualization_thread = boost::thread(&Pose::displayPointCloudOnline, this, cloud_small, cloud_hexPos_FM, cloud_hexPos_MAVLink, cycle, n_cycle);
			}
		}
		
		int64 t6 = getTickCount();
		
		cout << "\nCycle time: " << (t6 - t0) / getTickFrequency() << " sec" << endl;
		log_file << "Cycle time:\t\t\t\t\t" << (t6 - t0) / getTickFrequency() << " sec" << endl;
	}
	
	int64 tend = getTickCount();
	
	cout << "\nFinished Pose Estimation, total time: " << ((tend - app_start_time) / getTickFrequency()) << " sec at " << 1.0*img_numbers.size()/((tend - app_start_time) / getTickFrequency()) << " fps" 
		<< "\nimages " << img_numbers.size()
		<< "\njump_pixels " << jump_pixels
		<< "\nseq_len " << seq_len
		<< "\nrange_width " << range_width
		<< "\nblur_kernel " << blur_kernel
		<< "\nvoxel_size " << voxel_size
		<< "\nmax_depth " << max_depth
		<< "\nmax_height " << max_height
		<< "\nmin_points_per_voxel " << min_points_per_voxel
		<< "\ndist_nearby " << dist_nearby
		<< "\ngood_matched_imgs " << good_matched_imgs
		<< endl;
	log_file << "\nFinished Pose Estimation, total time: " << ((tend - app_start_time) / getTickFrequency()) << " sec at " << 1.0*img_numbers.size()/((tend - app_start_time) / getTickFrequency()) << " fps" 
		<< "\nimages " << img_numbers.size()
		<< "\njump_pixels " << jump_pixels
		<< "\nseq_len " << seq_len
		<< "\nrange_width " << range_width
		<< "\nblur_kernel " << blur_kernel
		<< "\nvoxel_size " << voxel_size
		<< "\nmax_depth " << max_depth
		<< "\nmax_height " << max_height
		<< "\nmin_points_per_voxel " << min_points_per_voxel
		<< "\ndist_nearby " << dist_nearby
		<< "\ngood_matched_imgs " << good_matched_imgs
		<< endl;
	
	if(online)
	{
		cout << "downsample before saving..." << endl;
		cloud_small = downsamplePtCloud(cloud_big, true);
		cout << "downsampled." << endl;
	}
	
	cout << "Saving point clouds..." << endl;
	read_PLY_filename0 = folder + "cloud.ply";
	save_pt_cloud_to_PLY_File(cloud_small, read_PLY_filename0);
	//read_PLY_filename0 = "cloudrgb_MAVLink_" + currentDateTimeStr + ".ply";
	//save_pt_cloud_to_PLY_File(cloudrgb_MAVLink, read_PLY_filename0);
	read_PLY_filename1 = folder + "cloud_big.ply";
	save_pt_cloud_to_PLY_File(cloud_big, read_PLY_filename1);
	
	//downsampling
	//cout << "downsampling..." << endl;
	//log_file << "downsampling..." << endl;
	//pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_MAVLink_downsamp = downsamplePtCloud(cloudrgb_MAVLink, false);
	//read_PLY_filename0 = "downsampled_" + read_PLY_filename0;
	//save_pt_cloud_to_PLY_File(cloudrgb_MAVLink_downsamp, read_PLY_filename0);
	
	cloud_hexPos_FM->insert(cloud_hexPos_FM->end(),cloud_hexPos_MAVLink->begin(),cloud_hexPos_MAVLink->end());
	string hexpos_filename = folder + "cloud_uavpos.ply";
	save_pt_cloud_to_PLY_File(cloud_hexPos_FM, hexpos_filename);
	
	if(preview)
		the_visualization_thread.join();
}

void Pose::findNormalOfPtCloud(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud)
{
	// Create the normal estimation class, and pass the input dataset to it
	pcl::NormalEstimation<pcl::PointXYZRGB, pcl::Normal> ne;
	ne.setInputCloud (cloud);

	// Create an empty kdtree representation, and pass it to the normal estimation object.
	// Its content will be filled inside the object, based on the given input dataset (as no other search surface is given).
	pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZRGB> ());
	ne.setSearchMethod (tree);

	// Output datasets
	pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>);

	// Use all neighbors in a sphere of radius 3cm
	ne.setRadiusSearch (1000);

	// Compute the features
	ne.compute (*cloud_normals);

	// cloud_normals->points.size () should have the same size as the input cloud->points.size ()*
	cout << "cloud_normals->size() " << cloud_normals->size() << endl;
	for (int i = 0; i < cloud_normals->size(); i++)
	{
		cout << cloud_normals->points[i].normal_x << " " << cloud_normals->points[i].normal_y << " " << cloud_normals->points[i].normal_z << " " << endl;
	}
	
}

void Pose::createAndTransformPtCloud(int img_index, 
	vector<pcl::registration::TransformationEstimation<pcl::PointXYZRGB, pcl::PointXYZRGB>::Matrix4> &t_FMVec, 
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr &cloudrgb_return)
{
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb (new pcl::PointCloud<pcl::PointXYZRGB> ());
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_transformed (new pcl::PointCloud<pcl::PointXYZRGB> ());
	
	if(jump_pixels == 1)
		createPtCloud(img_index, cloudrgb);
	else
		createFeaturePtCloud(img_index, cloudrgb);
	//cout << "Created point cloud " << i << endl;
	
	transformPtCloud(cloudrgb, cloudrgb_transformed, t_FMVec[img_index]);
	
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb_downsampled = downsamplePtCloud(cloudrgb_transformed, false);
	
	copyPointCloud(*cloudrgb_downsampled, *cloudrgb_return);
}

void Pose::displayPointCloudOnline(pcl::PointCloud<pcl::PointXYZRGB>::Ptr &cloud_combined_copy, 
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr &cloud_hexPos_FM, pcl::PointCloud<pcl::PointXYZRGB>::Ptr &cloud_hexPos_MAVLink, int cycle, int n_cycle)
{
	wait_at_visualizer = false;
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb (new pcl::PointCloud<pcl::PointXYZRGB> ());
	if(online)
	{
		cloudrgb = downsamplePtCloud(cloud_combined_copy, true);
	}
	else
	{
		cloudrgb = cloud_combined_copy;
	}
	
	displayUAVPositions = true;
	pcl::PolygonMesh mesh;
	//visualize_pt_cloud(true, cloudrgb_FeatureMatched_downsampA, false, mesh, "cloudrgb_FM_Fitted_downsampledA");
	
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr hexPos_cloud_online (new pcl::PointCloud<pcl::PointXYZRGB>());
	hexPos_cloud_online->insert(hexPos_cloud_online->begin(),cloud_hexPos_FM->begin(),cloud_hexPos_FM->end());
	hexPos_cloud_online->insert(hexPos_cloud_online->end(),cloud_hexPos_MAVLink->begin(),cloud_hexPos_MAVLink->end());
	hexPos_cloud = hexPos_cloud_online;
	if (cycle == 0)
	{
		viewer_online = visualize_pt_cloud(true, cloudrgb, false, mesh, "cloudrgb_visualization_Online");
	}
	else
	{
		visualize_pt_cloud_update(cloudrgb, "cloudrgb_visualization_Online", viewer_online);
	}
	if(cycle == n_cycle -1)
	{
		while (!viewer_online->wasStopped ()) { // Display the visualiser until 'q' key is pressed
			viewer_online->spinOnce();
		}
	}
}

