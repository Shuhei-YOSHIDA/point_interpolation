#include <iostream>
#include <vector>
#include <stack>
#include <map>
#include <set>
#include <chrono>

#include <Open3D/Open3D.h>
#include <opencv2/opencv.hpp>
#include <Eigen/Core>
#include <Eigen/Eigen>
#include <Eigen/Sparse>
#include <eigen3/unsupported/Eigen/NonLinearOptimization>
#include <time.h>

#include "quality_metrics_OpenCV_2.cpp"
#include "models/envParams.cpp"
#include "methods/linear.cpp"
#include "methods/mrf.cpp"
#include "methods/pwas.cpp"
#include "methods/original.cpp"

using namespace std;
using namespace open3d;

ofstream ofs;

void calc_grid(shared_ptr<geometry::PointCloud> raw_pcd_ptr, EnvParams envParams,
               vector<vector<double>> &original_grid, vector<vector<double>> &filtered_grid,
               vector<vector<double>> &original_interpolate_grid, vector<vector<double>> &filtered_interpolate_grid,
               vector<vector<int>> &target_vs, vector<vector<int>> &base_vs, int layer_cnt = 16)
{
    vector<double> tans;
    double PI = acos(-1);
    double delta_rad = 0.52698 * PI / 180;
    double max_rad = (16.6 + 0.26349) * PI / 180;
    double rad = (-16.6 + 0.26349) * PI / 180;
    while (rad < max_rad + 0.00001)
    {
        tans.emplace_back(tan(rad));
        rad += delta_rad;
    }

    vector<vector<Eigen::Vector3d>> all_layers(64, vector<Eigen::Vector3d>());
    double rollVal = (envParams.roll - 500) / 1000.0;
    double pitchVal = (envParams.pitch - 500) / 1000.0;
    double yawVal = (envParams.yaw - 500) / 1000.0;
    Eigen::MatrixXd calibration_mtx(3, 3);
    calibration_mtx << cos(yawVal) * cos(pitchVal), cos(yawVal) * sin(pitchVal) * sin(rollVal) - sin(yawVal) * cos(rollVal), cos(yawVal) * sin(pitchVal) * cos(rollVal) + sin(yawVal) * sin(rollVal),
        sin(yawVal) * cos(pitchVal), sin(yawVal) * sin(pitchVal) * sin(rollVal) + cos(yawVal) * cos(rollVal), sin(yawVal) * sin(pitchVal) * cos(rollVal) - cos(yawVal) * sin(rollVal),
        -sin(pitchVal), cos(pitchVal) * sin(rollVal), cos(pitchVal) * cos(rollVal);

    for (int i = 0; i < raw_pcd_ptr->points_.size(); i++)
    {
        double rawX = raw_pcd_ptr->points_[i][1];
        double rawY = -raw_pcd_ptr->points_[i][2];
        double rawZ = -raw_pcd_ptr->points_[i][0];

        double r = sqrt(rawX * rawX + rawZ * rawZ);
        double xp = calibration_mtx(0, 0) * rawX + calibration_mtx(0, 1) * rawY + calibration_mtx(0, 2) * rawZ;
        double yp = calibration_mtx(1, 0) * rawX + calibration_mtx(1, 1) * rawY + calibration_mtx(1, 2) * rawZ;
        double zp = calibration_mtx(2, 0) * rawX + calibration_mtx(2, 1) * rawY + calibration_mtx(2, 2) * rawZ;
        double x = xp + (envParams.X - 500) / 100.0;
        double y = yp + (envParams.Y - 500) / 100.0;
        double z = zp + (envParams.Z - 500) / 100.0;

        if (z > 0)
        {
            int u = (int)(envParams.width / 2 + envParams.f_xy * x / z);
            int v = (int)(envParams.height / 2 + envParams.f_xy * y / z);
            if (0 <= u && u < envParams.width && 0 <= v && v < envParams.height)
            {
                auto it = lower_bound(tans.begin(), tans.end(), rawY / r);
                int index = it - tans.begin();
                all_layers[index].emplace_back(x, y, z);
            }
        }
    }

    for (int i = 0; i < 64; i++)
    {
        // Remove occlusion
        // no sort
        vector<Eigen::Vector3d> removed;
        for (size_t j = 0; j < all_layers[i].size(); j++)
        {
            while (removed.size() > 0 && removed.back()[0] * all_layers[i][j][2] >= all_layers[i][j][0] * removed.back()[2])
            {
                removed.pop_back();
            }
            removed.emplace_back(all_layers[i][j]);
        }
    }

    target_vs = vector<vector<int>>(64, vector<int>(envParams.width, -1));
    base_vs = vector<vector<int>>(layer_cnt, vector<int>(envParams.width, -1));
    for (int i = 0; i < 64; i++)
    {
        for (int j = 0; j < envParams.width; j++)
        {
            double tan = tans[i];
            double rawZ = 1;
            double rawY = rawZ * tan;
            double x_coef = envParams.f_xy * calibration_mtx(0, 0) - (j - envParams.width / 2) * calibration_mtx(2, 0);
            double right_value = ((j - envParams.width / 2) * calibration_mtx(2, 1) - envParams.f_xy * calibration_mtx(0, 1)) * rawY + ((j - envParams.width / 2) * calibration_mtx(2, 2) - envParams.f_xy * calibration_mtx(0, 2)) * rawZ;
            double rawX = right_value / x_coef;
            double y = calibration_mtx(1, 0) * rawX + calibration_mtx(1, 1) * rawY + calibration_mtx(1, 2) * rawZ;
            double z = calibration_mtx(2, 0) * rawX + calibration_mtx(2, 1) * rawY + calibration_mtx(2, 2) * rawZ;
            int v = (int)(envParams.f_xy * y + envParams.height / 2 * z) / z;

            target_vs[i][j] = v;
        }
    }

    original_grid = vector<vector<double>>(64, vector<double>(envParams.width, -1));
    filtered_grid = vector<vector<double>>(layer_cnt, vector<double>(envParams.width, -1));
    original_interpolate_grid = vector<vector<double>>(64, vector<double>(envParams.width, -1));
    filtered_interpolate_grid = vector<vector<double>>(layer_cnt, vector<double>(envParams.width, -1));
    for (int i = 0; i < 64; i++)
    {
        if (all_layers[i].size() == 0)
        {
            continue;
        }

        int now = 0;
        int uPrev = (int)(envParams.width / 2 + envParams.f_xy * all_layers[i][0][0] / all_layers[i][0][2]);
        int vPrev = (int)(envParams.height / 2 + envParams.f_xy * all_layers[i][0][1] / all_layers[i][0][2]);
        while (now < uPrev)
        {
            original_interpolate_grid[i][now] = all_layers[i][0][2];
            now++;
        }
        for (int j = 0; j + 1 < all_layers[i].size(); j++)
        {
            int u = (int)(envParams.width / 2 + envParams.f_xy * all_layers[i][j + 1][0] / all_layers[i][j + 1][2]);
            int v = (int)(envParams.height / 2 + envParams.f_xy * all_layers[i][j + 1][1] / all_layers[i][j + 1][2]);
            original_grid[i][uPrev] = all_layers[i][j][2];
            target_vs[i][uPrev] = vPrev;

            while (now < min(envParams.width, u))
            {
                double angle = (all_layers[i][j + 1][2] - all_layers[i][j][2]) / (all_layers[i][j + 1][0] - all_layers[i][j][0]);
                double tan = (now - envParams.width / 2) / envParams.f_xy;
                double z = (all_layers[i][j][2] - angle * all_layers[i][j][0]) / (1 - tan * angle);
                original_interpolate_grid[i][now] = z;
                now++;
            }
            uPrev = u;
            vPrev = v;
        }

        original_grid[i][uPrev] = all_layers[i].back()[2];
        target_vs[i][uPrev] = vPrev;
        while (now < envParams.width)
        {
            original_interpolate_grid[i][now] = all_layers[i].back()[2];
            now++;
        }
    }
    for (int i = 0; i < layer_cnt; i++)
    {
        for (int j = 0; j < envParams.width; j++)
        {
            filtered_grid[i][j] = original_grid[i * (64 / layer_cnt)][j];
            filtered_interpolate_grid[i][j] = original_interpolate_grid[i * (64 / layer_cnt)][j];
            base_vs[i][j] = target_vs[i * (64 / layer_cnt)][j];
        }
    }

    { // Check
        auto original_ptr = make_shared<geometry::PointCloud>();
        auto filtered_ptr = make_shared<geometry::PointCloud>();
        for (int i = 0; i < 64; i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                double z = original_grid[i][j];
                if (z < 0)
                {
                    continue;
                }
                double x = z * (j - envParams.width / 2) / envParams.f_xy;
                double y = z * (target_vs[i][j] - envParams.height / 2) / envParams.f_xy;
                original_ptr->points_.emplace_back(x, y, z);
            }

            if (i % (64 / layer_cnt) == 0)
            {
                for (int j = 0; j < envParams.width; j++)
                {
                    double z = filtered_interpolate_grid[i / (64 / layer_cnt)][j];
                    if (z < 0)
                    {
                        continue;
                    }
                    double x = z * (j - envParams.width / 2) / envParams.f_xy;
                    double y = z * (target_vs[i][j] - envParams.height / 2) / envParams.f_xy;
                    filtered_ptr->points_.emplace_back(x, y, z);
                }
            }
        }
        //visualization::DrawGeometries({original_ptr}, "Points", 1200, 720);
    }
}

double segmentate(int data_no, EnvParams envParams, bool see_res = false)
{
    string img_path = envParams.folder_path + to_string(data_no);
    if (envParams.isRGB)
    {
        img_path += "_rgb.png";
    }
    else
    {
        img_path += ".png";
    }
    const string pcd_path = envParams.folder_path + to_string(data_no) + ".pcd";

    auto img = cv::imread(img_path);
    cv::Mat blured;
    cv::GaussianBlur(img, blured, cv::Size(3, 3), 0.5);

    geometry::PointCloud pointcloud;
    auto pcd_ptr = make_shared<geometry::PointCloud>();
    if (!io::ReadPointCloud(pcd_path, pointcloud))
    {
        cout << "Cannot read" << endl;
    }

    auto start = chrono::system_clock::now();

    vector<vector<double>> original_grid, filtered_grid, original_interpolate_grid, filtered_interpolate_grid;
    vector<vector<int>> target_vs, base_vs;
    *pcd_ptr = pointcloud;
    int layer_cnt = 16;
    calc_grid(pcd_ptr, envParams, original_grid, filtered_grid, original_interpolate_grid, filtered_interpolate_grid, target_vs, base_vs, layer_cnt);

    vector<vector<int>> original_vs;
    if (envParams.isFullHeight)
    {
        original_vs = vector<vector<int>>(envParams.height, vector<int>(envParams.width, 0));
        for (int i = 0; i < envParams.height; i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                original_vs[i][j] = i;
            }
        }
        swap(original_vs, target_vs);
        cout << original_vs.size() << endl;
        cout << target_vs.size() << endl;
    }
    else
    {
        original_vs = target_vs;
    }

    vector<vector<double>> interpolated_z;
    if (envParams.method == "linear")
    {
        linear(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams);
    }
    if (envParams.method == "mrf")
    {
        mrf(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
    }
    if (envParams.method == "pwas")
    {
        pwas(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
        cout << "aaa" << endl;
    }
    if (envParams.method == "original")
    {
        original(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
    }

    {
        cv::Mat interpolate_img = cv::Mat::zeros(target_vs.size(), envParams.width, CV_8UC1);
        for (int i = 0; i < target_vs.size(); i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                if (interpolated_z[i][j] > 0)
                {
                    interpolate_img.at<uchar>(i, j) = 255;
                }
            }
        }
        //cv::imshow("hoge", interpolate_img);
        //cv::waitKey();
    }

    double error = 0;
    cv::Mat original_Mat = cv::Mat::zeros(envParams.height, envParams.width, CV_64FC1);
    cv::Mat interpolated_Mat = cv::Mat::zeros(envParams.height, envParams.width, CV_64FC1);
    { // Evaluation
        double tim = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - start).count();

        cv::Mat original_reproject_Mat = cv::Mat::zeros(original_vs.size(), envParams.width, CV_64FC1);
        cv::Mat interpolated_reproject_Mat = cv::Mat::zeros(original_vs.size(), envParams.width, CV_64FC1);
        for (int i = 0; i < target_vs.size(); i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                interpolated_Mat.at<double>(target_vs[i][j], j) = interpolated_z[i][j];
            }
        }
        for (int i = 0; i < original_vs.size(); i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                original_Mat.at<double>(original_vs[i][j], j) = original_grid[i][j];
                original_reproject_Mat.at<double>(i, j) = original_grid[i][j];
                interpolated_reproject_Mat.at<double>(i, j) = interpolated_Mat.at<double>(original_vs[i][j], j);
            }
        }

        cout << original_reproject_Mat.rows << endl;
        double ssim = qm::ssim(original_reproject_Mat, interpolated_reproject_Mat, 64 / layer_cnt);
        double mse = qm::eqm(original_reproject_Mat, interpolated_reproject_Mat);
        double mre = qm::mre(original_reproject_Mat, interpolated_reproject_Mat);
        cout << tim << "ms" << endl;
        cout << "SSIM = " << fixed << setprecision(5) << ssim << endl;
        cout << "MSE = " << mse << endl;
        cout << "MRE = " << mre << endl;
        ofs << data_no << "," << tim << "," << ssim << "," << mse << "," << mre << "," << endl;
        error = mre;
    }

    if (see_res)
    {
        auto interpolated_ptr = make_shared<geometry::PointCloud>();
        auto original_colored_ptr = make_shared<geometry::PointCloud>();
        for (int i = 0; i < envParams.height; i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                double z = interpolated_Mat.at<double>(i, j);
                double original_z = original_Mat.at<double>(i, j);
                if (z <= 0 || original_z <= 0)
                {
                    continue;
                }

                z = min(z, 100.0);
                double x = z * (j - envParams.width / 2) / envParams.f_xy;
                double y = z * (i - envParams.height / 2) / envParams.f_xy;
                interpolated_ptr->points_.emplace_back(100 + x, z, -y);

                cv::Vec3b color = img.at<cv::Vec3b>(i, j);
                interpolated_ptr->colors_.emplace_back(color[2] / 255.0, color[1] / 255.0, color[0] / 255.0);
            }
        }
        for (int i = 0; i < envParams.height; i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                double original_z = original_Mat.at<double>(i, j);
                if (original_z <= 0)
                {
                    continue;
                }

                double original_x = original_z * (j - envParams.width / 2) / envParams.f_xy;
                double original_y = original_z * (i - envParams.height / 2) / envParams.f_xy;
                original_colored_ptr->points_.emplace_back(original_x, original_z, -original_y);

                cv::Vec3b color = img.at<cv::Vec3b>(i, j);
                original_colored_ptr->colors_.emplace_back(color[2] / 255.0, color[1] / 255.0, color[0] / 255.0);
            }
        }
        visualization::DrawGeometries({original_colored_ptr, interpolated_ptr}, "Original", 1600, 900);
        if (!io::WritePointCloudToPCD(envParams.folder_path + to_string(data_no) + "_linear.pcd", *original_colored_ptr))
        {
            cout << "Cannot write" << endl;
        }
        Eigen::MatrixXd front(4, 4);
        front << 1, 0, 0, 0,
            0, -1, 0, 0,
            0, 0, -1, 0,
            0, 0, 0, 1;
        pcd_ptr->Transform(front);
    }

    return error;
}

int main(int argc, char *argv[])
{
    //vector<int> data_nos = {550, 1000, 1125, 1260, 1550}; // 03_03_miyanosawa
    //vector<int> data_nos = {10, 20, 30, 40, 50}; // 02_04_13jo
    //vector<int> data_nos = {700, 1290, 1460, 2350, 3850}; // 02_04_miyanosawa

    vector<int> data_nos;
    for (int i = 1100; i <= 1300; i++)
    {
        data_nos.emplace_back(i);
    }

    EnvParams params_13jo = {938, 606, 938 / 2 * 1.01, 498, 485, 509, 481, 517, 500, "../../../data/2020_02_04_13jo/", {10, 20, 30, 40, 50}, "res_linear_13jo.csv", "linear", true, false};
    EnvParams params_miyanosawa = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", {700, 1290, 1460, 2350, 3850}, "res_linear_miyanosawa.csv", "linear", false, true};
    EnvParams params_miyanosawa_champ = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", {1207, 1262, 1264, 1265, 1277}, "res_linear_miyanosawa_RGB.csv", "linear", false, true};

    EnvParams params_miyanosawa_3_3 = {640, 480, 640, 498, 489, 388, 554, 560, 506, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_linear_miyanosawa_0303_1100-1300_RGB.csv", "linear", false, true};
    EnvParams params_miyanosawa_3_3_pwas = {640, 480, 640, 498, 489, 388, 554, 560, 506, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_pwas_miyanosawa_0303_1100-1300_RGB.csv", "pwas", false, true};
    EnvParams params_miyanosawa_3_3_pwas_champ = {640, 480, 640, 498, 489, 388, 554, 560, 506, "../../../data/2020_03_03_miyanosawa/", {1207, 1262, 1264, 1265, 1277}, "res_pwas_miyanosawa_0303_RGB.csv", "pwas", false, true};
    EnvParams params_miyanosawa_3_3_original = {640, 480, 640, 498, 489, 388, 554, 560, 506, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_original_miyanosawa_0303_1100-1300_RGB.csv", "original", false, true};

    EnvParams params_miyanosawa_3_3_thermal = {938, 606, 938 / 2 * 1.01, 495, 466, 450, 469, 503, 487, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_linear_miyanosawa_0303_1100-1300_Thermal.csv", "linear", false, false};
    EnvParams params_miyanosawa_3_3_thermal_pwas = {938, 606, 938 / 2 * 1.01, 495, 466, 450, 469, 503, 487, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_pwas_miyanosawa_0303_1100-1300_Thermal.csv", "pwas", false, false};
    EnvParams params_miyanosawa_3_3_thermal_original = {938, 606, 938 / 2 * 1.01, 495, 466, 450, 469, 503, 487, "../../../data/2020_03_03_miyanosawa/", data_nos, "res_original_miyanosawa_0303_1100-1300_Thermal.csv", "original", false, false};

    EnvParams params_miyanosawa_0204_rgb_linear = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_linear_miyanosawa_0204_1100-1300_RGB.csv", "linear", false, true};
    EnvParams params_miyanosawa_0204_rgb_mrf = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_mrf_miyanosawa_0204_1100-1300_RGB.csv", "mrf", false, true};
    EnvParams params_miyanosawa_0204_rgb_pwas = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_pwas_miyanosawa_0204_1100-1300_RGB.csv", "pwas", false, true};
    EnvParams params_miyanosawa_0204_rgb_original = {640, 480, 640, 506, 483, 495, 568, 551, 510, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_original_miyanosawa_0204_1100-1300_RGB.csv", "original", false, true};

    EnvParams params_miyanosawa_0204_thermal_linear = {938, 606, 938 / 2 * 1.01, 495, 475, 458, 488, 568, 500, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_linear_miyanosawa_0204_1100-1300_Thermal.csv", "linear", false, false};
    EnvParams params_miyanosawa_0204_thermal_mrf = {938, 606, 938 / 2 * 1.01, 495, 475, 458, 488, 568, 500, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_mrf_miyanosawa_0204_1100-1300_Thermal.csv", "mrf", false, false};
    EnvParams params_miyanosawa_0204_thermal_pwas = {938, 606, 938 / 2 * 1.01, 495, 475, 458, 488, 568, 500, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_pwas_miyanosawa_0204_1100-1300_Thermal.csv", "pwas", false, false};
    EnvParams params_miyanosawa_0204_thermal_original = {938, 606, 938 / 2 * 1.01, 495, 475, 458, 488, 568, 500, "../../../data/2020_02_04_miyanosawa/", data_nos, "res_original_miyanosawa_0204_1100-1300_Thermal.csv", "original", false, false};

    EnvParams params_use = params_miyanosawa_0204_rgb_mrf;
    ofs = ofstream(params_use.of_name);

    for (int i = 0; i < params_use.data_ids.size(); i++)
    {
        segmentate(params_use.data_ids[i], params_use, false);
    }
    return 0;

    params_use = params_miyanosawa_3_3_pwas_champ;
    double best_error = 1000000;
    double best_sigma_c = 1;
    double best_sigma_s = 1;
    double best_sigma_r = 1;
    int best_r = 1;
    // best params 2020/08/03 sigma_c:1000 sigma_s:1.99 sigma_r:19 r:7
    // best params 2020/08/10 sigma_c:12000 sigma_s:1.6 sigma_r:19 r:7
    // best params 2020/08/10 sigma_c:8000 sigma_s:1.6 sigma_r:19 r:7

    for (double sigma_c = 10; sigma_c <= 1000; sigma_c += 10)
    {
        for (double sigma_s = 0.1; sigma_s < 1.7; sigma_s += 0.1)
        {
            for (double sigma_r = 1; sigma_r < 100; sigma_r += 10)
            {
                for (int r = 1; r < 9; r += 2)
                {
                    double error = 0;
                    for (int i = 0; i < params_use.data_ids.size(); i++)
                    {
                        error += segmentate(params_use.data_ids[i], params_use);
                    }

                    if (best_error > error)
                    {
                        best_error = error;
                        best_sigma_c = sigma_c;
                        best_sigma_s = sigma_s;
                        best_sigma_r = sigma_r;
                        best_r = r;
                    }
                }
            }
        }
    }

    cout << "Sigma C = " << best_sigma_c << endl;
    cout << "Sigma S = " << best_sigma_s << endl;
    cout << "Sigma R = " << best_sigma_r << endl;
    cout << "R = " << best_r << endl;
    cout << "Mean error = " << best_error / params_use.data_ids.size() << endl;
}